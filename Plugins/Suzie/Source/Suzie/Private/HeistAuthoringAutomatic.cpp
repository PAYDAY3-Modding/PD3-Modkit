#include "HeistAuthoring.h"

#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/CoreDelegates.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/MetaData.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr const TCHAR* GeneratedTransformKey = TEXT("Suzie.Heists.GeneratedSpawnTransform");
    FDelegateHandle EngineInitHandle, AddedHandle, MovedHandle, PropertyHandle, SaveHandle;
    FTSTicker::FDelegateHandle DeferredHandle;
    TSet<TWeakObjectPtr<AActor>> PendingActors;
    bool bUpdating = false;

    bool IsAuthoredEditorActor(const AActor& Actor)
    {
        const UWorld* World = Actor.GetWorld();
        return GIsEditor && !IsRunningCookCommandlet() && IsValid(&Actor) && !Actor.IsTemplate()
            && World && World->WorldType == EWorldType::Editor && !World->IsBeingCleanedUp()
            && !Actor.GetPackage()->HasAnyPackageFlags(PKG_Cooked);
    }

    bool IsNativeDescendant(const AActor& Actor, const TCHAR* Path)
    {
        for (const UClass* Class = Actor.GetClass(); Class; Class = Class->GetSuperClass())
            if (Class->GetPathName() == Path) return true;
        return false;
    }

    void UpdateActor(AActor& Actor)
    {
        if (!IsAuthoredEditorActor(Actor)) return;
        SuzieHeistAuthoring::UpdateSpawnAuthoringData(Actor, true);

        // Repair the exact old engine-constructor fingerprint on authored native
        // escape volumes. Blueprint/archetype tick overrides remain intentional.
        if (Actor.GetClass()->GetPathName() == TEXT("/Script/Starbreeze.SBZPlayerEscapeVolume")
            && !Actor.PrimaryActorTick.bCanEverTick && Actor.PrimaryActorTick.bStartWithTickEnabled
            && Actor.PrimaryActorTick.TickGroup == TG_PrePhysics)
        {
            const AActor* CDO = Actor.GetClass()->GetDefaultObject<AActor>();
            if (CDO->PrimaryActorTick.bCanEverTick)
            {
                Actor.Modify();
                SuzieHeistAuthoring::CopyActorTickConfiguration(Actor, *CDO);
            }
        }
    }

    void UpdateActorTree(AActor* Actor)
    {
        if (bUpdating || !Actor || !IsAuthoredEditorActor(*Actor)) return;
        TGuardValue<bool> Guard(bUpdating, true);
        UpdateActor(*Actor);
        TArray<AActor*> Children;
        Actor->GetAllChildActors(Children, true);
        Actor->GetAttachedActors(Children, false, true);
        TSet<AActor*> Visited;
        for (AActor* Child : Children)
            if (Child && !Visited.Contains(Child)) { Visited.Add(Child); UpdateActor(*Child); }
    }

    bool FlushNewActors(float)
    {
        DeferredHandle.Reset();
        TSet<TWeakObjectPtr<AActor>> Actors = MoveTemp(PendingActors);
        PendingActors.Reset();
        for (const TWeakObjectPtr<AActor>& Actor : Actors) UpdateActorTree(Actor.Get());
        return false;
    }

    void OnActorAdded(AActor* Actor)
    {
        if (!Actor || !IsAuthoredEditorActor(*Actor)) return;
        // Spawn callbacks normally follow construction. Also defer one pass for
        // paste/deferred construction and child actors whose final transform is later.
        UpdateActorTree(Actor);
        PendingActors.Add(Actor);
        if (!DeferredHandle.IsValid())
            DeferredHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FlushNewActors));
    }

    void OnPropertyChanged(UObject* Object, FPropertyChangedEvent&)
    {
        if (AActor* Actor = Cast<AActor>(Object)) UpdateActorTree(Actor);
        else if (const USceneComponent* Component = Cast<USceneComponent>(Object)) UpdateActorTree(Component->GetOwner());
    }

    void OnPreSave(UObject* Object, FObjectPreSaveContext Context)
    {
        if (!Context.IsCooking()) UpdateActorTree(Cast<AActor>(Object));
    }

    void BindEngineHooks()
    {
        if (!GEngine || AddedHandle.IsValid()) return;
        AddedHandle = GEngine->OnLevelActorAdded().AddStatic(&OnActorAdded);
        MovedHandle = GEngine->OnActorMoved().AddStatic(&UpdateActorTree);
    }
}

bool SuzieHeistAuthoring::UpdateSpawnAuthoringData(AActor& Actor, bool bFollowGeneratedTransform)
{
    if (!IsAuthoredEditorActor(Actor)
        || !IsNativeDescendant(Actor, TEXT("/Script/Starbreeze.SBZPawnSpawnBase"))) return false;
    FArrayProperty* Array = FindFProperty<FArrayProperty>(Actor.GetClass(), TEXT("SpawnDefinitionArray"));
    FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
    FStructProperty* Transform = Inner ? FindFProperty<FStructProperty>(Inner->Struct, TEXT("Transform")) : nullptr;
    FStructProperty* Weighted = FindFProperty<FStructProperty>(Actor.GetClass(), TEXT("WeightedLocation"));
    FNameProperty* NameType = Inner ? FindFProperty<FNameProperty>(Inner->Struct, TEXT("NameType")) : nullptr;
    if (!Transform || Transform->Struct != TBaseStructure<FTransform>::Get()
        || !Weighted || Weighted->Struct != TBaseStructure<FVector>::Get() || !NameType) return false;

    FScriptArrayHelper Definitions(Array, Array->ContainerPtrToValuePtr<void>(&Actor));
    UMetaData* Metadata = Actor.GetPackage()->GetMetaData();
    const FString PreviousText = Metadata->GetValue(&Actor, GeneratedTransformKey);
    FTransform Previous;
    const bool bManaged = Previous.InitFromString(PreviousText);
    const FTransform Current = Actor.GetActorTransform();

    if (Definitions.Num() > 0)
    {
        if (!bFollowGeneratedTransform || !bManaged) return false;
        // Relinquish automatic tracking after any custom array/definition edit.
        if (Definitions.Num() != 1
            || !Transform->ContainerPtrToValuePtr<FTransform>(Definitions.GetRawPtr(0))->Equals(Previous, 0.001)
            || !Weighted->ContainerPtrToValuePtr<FVector>(&Actor)->Equals(Previous.GetLocation(), 0.001)
            || !NameType->GetPropertyValue_InContainer(Definitions.GetRawPtr(0)).IsNone())
        {
            Metadata->SetFlags(RF_Transactional);
            Metadata->Modify();
            Metadata->RemoveValue(&Actor, GeneratedTransformKey);
            return false;
        }
        if (Transform->ContainerPtrToValuePtr<FTransform>(Definitions.GetRawPtr(0))->Equals(Current)
            && Weighted->ContainerPtrToValuePtr<FVector>(&Actor)->Equals(Current.GetLocation())) return false;
    }

    Actor.Modify();
    Metadata->SetFlags(RF_Transactional);
    Metadata->Modify();
    if (!Definitions.Num()) Definitions.AddValue();
    *Transform->ContainerPtrToValuePtr<FTransform>(Definitions.GetRawPtr(0)) = Current;
    *Weighted->ContainerPtrToValuePtr<FVector>(&Actor) = Current.GetLocation();
    // Package metadata is editor-only: no new runtime class, component or actor tag.
    Metadata->SetValue(&Actor, GeneratedTransformKey, *Current.ToString());
    return true;
}

void SuzieHeistAuthoring::RegisterEditorHooks()
{
    if (IsRunningCookCommandlet()) return;
    BindEngineHooks();
    if (!EngineInitHandle.IsValid()) EngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&BindEngineHooks);
    if (!PropertyHandle.IsValid()) PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&OnPropertyChanged);
    if (!SaveHandle.IsValid()) SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&OnPreSave);
}

void SuzieHeistAuthoring::UnregisterEditorHooks()
{
    FCoreDelegates::OnPostEngineInit.Remove(EngineInitHandle); EngineInitHandle.Reset();
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle); PropertyHandle.Reset();
    FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle); SaveHandle.Reset();
    if (GEngine)
    {
        GEngine->OnLevelActorAdded().Remove(AddedHandle);
        GEngine->OnActorMoved().Remove(MovedHandle);
    }
    AddedHandle.Reset(); MovedHandle.Reset();
    if (DeferredHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(DeferredHandle);
    DeferredHandle.Reset(); PendingActors.Reset();
}
