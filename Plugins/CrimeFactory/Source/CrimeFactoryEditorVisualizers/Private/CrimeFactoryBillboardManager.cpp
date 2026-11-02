#include "CrimeFactoryBillboardManager.h"

#include "CrimeFactoryActorVisualisationSettings.h"
#include "CrimeFactoryBillboardComponent.h"
#include "CrimeFactoryVisualisationComponents.h"
#include "Components/BillboardComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EditorReimportHandler.h"
#include "HAL/PlatformTime.h"
#include "UObject/PackageReload.h"
#include "UObject/ICookInfo.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogCrimeFactoryVisualizers, Log, All);

namespace
{
    const FName ManagedTag(TEXT("CrimeFactory.EditorVisualisation"));
    const FName BillboardName(TEXT("CrimeFactoryEditorBillboard"));

    bool IsManaged(const UBillboardComponent* Component)
    {
        return CrimeFactoryVisualisation::IsManaged(Component, ManagedTag);
    }
}

void FCrimeFactoryBillboardManager::Initialize()
{
    Bind(GetMutableDefault<UCrimeFactoryActorVisualisationSettings>()->OnSettingChanged(),
        [this](UObject*, FPropertyChangedEvent&) { RequestRefresh(true); });

    GEngine->OnLevelActorAdded().AddRaw(this, &FCrimeFactoryBillboardManager::QueueActor);
    GEngine->OnLevelActorDeleted().AddRaw(this, &FCrimeFactoryBillboardManager::RemoveActor);
    GEngine->OnActorMoved().AddRaw(this, &FCrimeFactoryBillboardManager::QueueActor);
    Bind(GEngine->OnLevelActorListChanged(), [this]() { RequestRefresh(); });
    Bind(FWorldDelegates::OnPostWorldInitialization, [this](UWorld* World, const UWorld::InitializationValues)
    {
        if (World->WorldType == EWorldType::Editor) { RequestRefresh(); }
    });
    Bind(FWorldDelegates::LevelAddedToWorld, [this](ULevel* Level, UWorld* World)
    {
        if (World && World->WorldType == EWorldType::Editor)
        {
            TrackLevel(Level);
            if (Level) { for (AActor* Actor : Level->Actors) { QueueActor(Actor); } }
        }
    });
    Bind(FWorldDelegates::LevelRemovedFromWorld, [this](ULevel* Level, UWorld*)
    {
        if (Level)
        {
            for (AActor* Actor : Level->Actors) { RemoveActor(Actor); }
            FLevelHooks Hooks;
            if (Levels.RemoveAndCopyValue(Level, Hooks))
            {
                Level->OnLoadedActorAddedToLevelEvent.Remove(Hooks.Added);
                Level->OnLoadedActorRemovedFromLevelEvent.Remove(Hooks.Removed);
            }
        }
    });
    Bind(FWorldDelegates::OnWorldCleanup, [this](UWorld* World, bool, bool)
    {
        TArray<TWeakObjectPtr<AActor>> Actors;
        Billboards.GetKeys(Actors);
        PawnPreviews.GetActors(Actors);
        for (const auto& WeakActor : Actors)
        {
            AActor* Actor = WeakActor.Get();
            if (Actor && Actor->GetWorld() == World) { RemoveActor(Actor); }
        }
        for (auto It = Levels.CreateIterator(); It; ++It)
        {
            ULevel* Level = It.Key().Get();
            if (!Level || Level->GetWorld() == World)
            {
                if (Level)
                {
                    Level->OnLoadedActorAddedToLevelEvent.Remove(It.Value().Added);
                    Level->OnLoadedActorRemovedFromLevelEvent.Remove(It.Value().Removed);
                }
                It.RemoveCurrent();
            }
        }
    });
    Bind(FEditorDelegates::OnMapOpened, [this](const FString&, bool) { RequestRefresh(); });

    Bind(FCoreUObjectDelegates::OnObjectsReinstanced, [this](const FCoreUObjectDelegates::FReplacementObjectMap&)
    {
        RequestRefresh(true);
    });
    Bind(FCoreUObjectDelegates::OnObjectsReplaced, [this](const TMap<UObject*, UObject*>&)
    {
        RequestRefresh(true);
    });
    Bind(FCoreUObjectDelegates::OnObjectPropertyChanged, [this](UObject* Object, FPropertyChangedEvent&)
    {
        if (bUpdating) { return; }
        if (PawnPreviews.InvalidateForObject(Object)) { RequestRefresh(); }
        if (Object == GetDefault<UCrimeFactoryActorVisualisationSettings>()) { RequestRefresh(true); }
        else if (AActor* Actor = Cast<AActor>(Object)) { QueueActor(Actor); }
        else if (USceneComponent* Component = Cast<USceneComponent>(Object))
        {
            if (!Component->ComponentHasTag(ManagedTag)) { QueueActor(Component->GetOwner()); }
        }
    });
    Bind(FCoreUObjectDelegates::OnObjectTransacted, [this](UObject* Object, const FTransactionObjectEvent&)
    {
        if (PawnPreviews.InvalidateForObject(Object)) { RequestRefresh(); }
        if (AActor* Actor = Cast<AActor>(Object)) { QueueActor(Actor); }
    });
    Bind(FReimportManager::Instance()->OnPostReimport(), [this](UObject* Object, bool bSucceeded)
    {
        if (bSucceeded && PawnPreviews.InvalidateForObject(Object)) { RequestRefresh(); }
    });
    Bind(FCoreUObjectDelegates::OnPackageReloaded, [this](EPackageReloadPhase Phase, FPackageReloadedEvent*)
    {
        if (Phase == EPackageReloadPhase::PostBatchPostGC) { RequestRefresh(true); }
    });

    // Remove previews before save/reference gathering, and recreate only after saving has finished.
    // This also covers individual World Partition external actor saves.
    Bind(FEditorDelegates::PreSaveWorldWithContext, [this](UWorld*, FObjectPreSaveContext)
    {
        RemoveAll();
        RequestRefresh();
    });
    Bind(FCoreUObjectDelegates::OnObjectPreSave, [this](UObject* Object, FObjectPreSaveContext)
    {
        if (AActor* Actor = Cast<AActor>(Object))
        {
            RemoveActor(Actor);
            QueueActor(Actor);
        }
    });
    Bind(FEditorDelegates::PostSaveWorldWithContext, [this](UWorld*, FObjectPostSaveContext) { RequestRefresh(); });

    Bind(FEditorDelegates::PreBeginPIE, [this](bool)
    {
        bSuspendedForPIE = true;
        RemoveAll();
    });
    Bind(FEditorDelegates::EndPIE, [this](bool)
    {
        bSuspendedForPIE = false;
        RequestRefresh();
    });

    // Exclude previews from clipboard text and editor duplication, including their texture references.
    FEditorDelegates::OnEditCopyActorsBegin.AddRaw(this, &FCrimeFactoryBillboardManager::BeginActorExport);
    FEditorDelegates::OnEditCopyActorsEnd.AddRaw(this, &FCrimeFactoryBillboardManager::EndActorExport);
    FEditorDelegates::OnEditCutActorsBegin.AddRaw(this, &FCrimeFactoryBillboardManager::BeginActorExport);
    FEditorDelegates::OnEditCutActorsEnd.AddRaw(this, &FCrimeFactoryBillboardManager::EndActorExport);
    FEditorDelegates::OnDuplicateActorsBegin.AddRaw(this, &FCrimeFactoryBillboardManager::BeginActorExport);
    FEditorDelegates::OnDuplicateActorsEnd.AddRaw(this, &FCrimeFactoryBillboardManager::EndActorExport);
    Bind(FEditorDelegates::OnEditPasteActorsEnd, [this]() { RequestRefresh(); });

    RequestRefresh(true);
}

void FCrimeFactoryBillboardManager::Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    for (auto& Unbind : Unbinders) { Unbind(); }
    Unbinders.Empty();
    if (GEngine)
    {
        GEngine->OnLevelActorAdded().RemoveAll(this);
        GEngine->OnLevelActorDeleted().RemoveAll(this);
        GEngine->OnActorMoved().RemoveAll(this);
    }
    FEditorDelegates::OnEditCopyActorsBegin.RemoveAll(this);
    FEditorDelegates::OnEditCopyActorsEnd.RemoveAll(this);
    FEditorDelegates::OnEditCutActorsBegin.RemoveAll(this);
    FEditorDelegates::OnEditCutActorsEnd.RemoveAll(this);
    FEditorDelegates::OnDuplicateActorsBegin.RemoveAll(this);
    FEditorDelegates::OnDuplicateActorsEnd.RemoveAll(this);
    for (const auto& Entry : Levels)
    {
        if (ULevel* Level = Entry.Key.Get())
        {
            Level->OnLoadedActorAddedToLevelEvent.Remove(Entry.Value.Added);
            Level->OnLoadedActorRemovedFromLevelEvent.Remove(Entry.Value.Removed);
        }
    }
    Levels.Empty();
    RemoveAll();
    Definitions.Empty();
    DefinitionClasses.Empty();
    PendingActors.Empty();
}

bool FCrimeFactoryBillboardManager::IsEligible(const AActor* Actor)
{
    return IsValid(Actor) && !Actor->IsTemplate() && !Actor->IsActorBeingDestroyed()
        && !Actor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
        && Actor->GetLevel() && Actor->GetWorld() && Actor->GetWorld()->WorldType == EWorldType::Editor;
}

void FCrimeFactoryBillboardManager::RequestRefresh(bool bReload)
{
    bScanWorlds = true;
    bReloadDefinitions |= bReload;
    if (!TickHandle.IsValid())
    {
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCrimeFactoryBillboardManager::Tick));
    }
}

void FCrimeFactoryBillboardManager::QueueActor(AActor* Actor)
{
    if (IsEligible(Actor))
    {
        PendingActors.Add(Actor);
        if (!TickHandle.IsValid())
        {
            TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCrimeFactoryBillboardManager::Tick));
        }
    }
}

void FCrimeFactoryBillboardManager::TrackLevel(ULevel* Level)
{
    if (!IsValid(Level) || Levels.Contains(Level)) { return; }
    FLevelHooks Hooks;
    Hooks.Added = Level->OnLoadedActorAddedToLevelEvent.AddLambda([this](AActor& Actor) { QueueActor(&Actor); });
    Hooks.Removed = Level->OnLoadedActorRemovedFromLevelEvent.AddLambda([this](AActor& Actor) { RemoveActor(&Actor); });
    Levels.Add(Level, Hooks);
}

bool FCrimeFactoryBillboardManager::Tick(float)
{
    if (GIsSavingPackage || IsGarbageCollecting()) { return true; }
    if (bSuspendedForPIE || ActorExportDepth > 0) { TickHandle.Reset(); return false; }
    TGuardValue<bool> UpdatingGuard(bUpdating, true);
    if (bReloadDefinitions)
    {
        bReloadDefinitions = false;
        ReloadDefinitions();
    }
    if (bScanWorlds)
    {
        bScanWorlds = false;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* World = Context.World();
            if (!World || World->WorldType != EWorldType::Editor) { continue; }
            for (ULevel* Level : World->GetLevels()) { TrackLevel(Level); }
            for (TActorIterator<AActor> It(World); It; ++It) { PendingActors.Add(*It); }
        }
    }
    // Bound work across editor frames when opening a large level. Shared request appearances
    // are resolved once; there is no actor polling after this event-driven queue is drained.
    const double Deadline = FPlatformTime::Seconds() + 0.008;
    while (PendingActors.Num())
    {
        auto It = PendingActors.CreateIterator();
        const TWeakObjectPtr<AActor> Actor = *It;
        It.RemoveCurrent();
        if (Actor.IsValid()) { UpdateActor(Actor.Get()); }
        if (FPlatformTime::Seconds() >= Deadline) { break; }
    }
    for (auto It = Billboards.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid() || !It.Value().IsValid()) { It.RemoveCurrent(); }
    }
    for (auto It = Levels.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) { It.RemoveCurrent(); }
    }
    PawnPreviews.Prune();
    const bool bMoreWork = PendingActors.Num() || bScanWorlds || bReloadDefinitions;
    if (!bMoreWork) { TickHandle.Reset(); }
    return bMoreWork;
}

void FCrimeFactoryBillboardManager::ReloadDefinitions()
{
    Definitions.Empty();
    DefinitionClasses.Empty();
    PawnPreviews.ReloadDefinitions();
    const UCrimeFactoryActorVisualisationSettings* Settings = GetDefault<UCrimeFactoryActorVisualisationSettings>();
    Scale = FMath::IsFinite(Settings->BillboardScale) ? FMath::Max(0.01f, Settings->BillboardScale) : 1.0f;
    TSet<UClass*> SeenClasses;
    // Match the engine's own editor sprite loading: never record these loads as runtime dependencies.
    FCookLoadScope EditorOnlyLoadScope(ECookLoadType::EditorOnly);
    for (const auto& Definition : Settings->ActorBillboardDefinitions)
    {
        if (Definition.ActorClass.IsNull()) { continue; }
        UClass* Class = Definition.ActorClass.LoadSynchronous();
        if (!Class || !Class->IsChildOf(AActor::StaticClass()))
        {
            UE_LOG(LogCrimeFactoryVisualizers, Warning, TEXT("Cannot resolve billboard actor class: %s"), *Definition.ActorClass.ToString());
            continue;
        }
        if (SeenClasses.Contains(Class))
        {
            Definitions.Remove(Class);
            UE_LOG(LogCrimeFactoryVisualizers, Warning, TEXT("Duplicate billboard definitions for %s were ignored. Configure each actor class once."), *Class->GetPathName());
            continue;
        }
        SeenClasses.Add(Class);
        UTexture2D* Texture = Definition.BillboardTexture.LoadSynchronous();
        if (!Texture)
        {
            UE_LOG(LogCrimeFactoryVisualizers, Warning, TEXT("Billboard texture for %s is missing: %s"), *Class->GetPathName(), *Definition.BillboardTexture.ToString());
            continue;
        }
        DefinitionClasses.Emplace(Class);
        Definitions.Emplace(Class, TStrongObjectPtr<UTexture2D>(Texture));
    }
}

void FCrimeFactoryBillboardManager::UpdateActor(AActor* Actor)
{
    if (!IsEligible(Actor)) { RemoveActor(Actor); return; }
    PawnPreviews.UpdateActor(*Actor);
    UTexture2D* Texture = nullptr;
    for (UClass* Class = Actor->GetClass(); Class; Class = Class->GetSuperClass())
    {
        if (const auto* Match = Definitions.Find(Class)) { Texture = Match->Get(); break; }
    }
    if (!Texture)
    {
        Billboards.Remove(Actor);
        TInlineComponentArray<UBillboardComponent*> Components(Actor);
        for (UBillboardComponent* Component : Components)
        {
            if (IsManaged(Component))
            {
                Component->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
                Component->DestroyComponent();
            }
        }
        return;
    }

    UBillboardComponent* Billboard = nullptr;
    TInlineComponentArray<UBillboardComponent*> Components(Actor);
    for (UBillboardComponent* Candidate : Components)
    {
        if (!IsManaged(Candidate)) { continue; }
        if (!Candidate->IsA<UCrimeFactoryBillboardComponent>())
        {
            Candidate->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
            Candidate->DestroyComponent();
            continue;
        }
        if (!Billboard) { Billboard = Candidate; }
        else
        {
            Candidate->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
            Candidate->DestroyComponent();
        }
    }
    if (!Billboard)
    {
        FCookLoadScope EditorOnlyLoadScope(ECookLoadType::EditorOnly);
        Billboard = NewObject<UCrimeFactoryBillboardComponent>(Actor,
            MakeUniqueObjectName(Actor, UCrimeFactoryBillboardComponent::StaticClass(), BillboardName),
            CrimeFactoryVisualisation::PreviewFlags);
        CrimeFactoryVisualisation::Configure(*Billboard, ManagedTag);
        Billboard->bIsScreenSizeScaled = true;
        Billboard->bUseInEditorScaling = true;
        // Do not call AddInstanceComponent: that array is serialized into maps and Blueprints.
    }
    Billboards.Add(Actor, Billboard);
    USceneComponent* Root = Actor->GetRootComponent();
    if (Billboard->GetAttachParent() != Root)
    {
        if (Billboard->IsRegistered()) { Billboard->UnregisterComponent(); }
        Billboard->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
        Billboard->SetupAttachment(Root);
    }
    if (Root)
    {
        Billboard->SetRelativeTransform(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(Scale)));
    }
    else
    {
        Billboard->SetWorldTransform(FTransform(Actor->GetActorQuat(), Actor->GetActorLocation(), FVector(Scale)));
    }
    if (Billboard->Sprite != Texture) { Billboard->SetSprite(Texture); }
    if (!Billboard->IsRegistered()) { Billboard->RegisterComponent(); }
}

void FCrimeFactoryBillboardManager::RemoveActor(AActor* Actor)
{
    if (!Actor) { return; }
    PawnPreviews.RemoveActor(*Actor);
    Billboards.Remove(Actor);
    PendingActors.Remove(Actor);
    TInlineComponentArray<UBillboardComponent*> Components(Actor);
    for (UBillboardComponent* Component : Components)
    {
        if (IsManaged(Component))
        {
            Component->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
            Component->DestroyComponent();
        }
    }
}

void FCrimeFactoryBillboardManager::RemoveAll()
{
    TArray<TWeakObjectPtr<AActor>> Actors;
    Billboards.GetKeys(Actors);
    for (const auto& Actor : Actors) { if (Actor.IsValid()) { RemoveActor(Actor.Get()); } }
    Billboards.Empty();
    PawnPreviews.RemoveAll();
}

void FCrimeFactoryBillboardManager::BeginActorExport()
{
    ++ActorExportDepth;
    RemoveAll();
}

void FCrimeFactoryBillboardManager::EndActorExport()
{
    ActorExportDepth = FMath::Max(0, ActorExportDepth - 1);
    RequestRefresh();
}
