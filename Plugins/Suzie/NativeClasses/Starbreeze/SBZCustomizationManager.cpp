/**
 * @file SBZCustomizationManager.cpp
 * @brief Restores armor variation selection through the reflected customization functions.
 *
 * ArmorConfig.Data supplies a soft MeshData reference to SBZVariationSetData. Apply that
 * set to GlobalArmor, creating a transient editor preview when no display actor exists.
 * The native layout and function signatures remain owned by Suzie and the current JMAP.
 */

#include "SBZVariationNative.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/SoftObjectPtr.h"

namespace SuzieArmorCustomization
{
    DEFINE_LOG_CATEGORY_STATIC(LogSuzieArmorVariation, Log, All);

    /** State for transient displays owned by a manager, not externally supplied armor actors. */
    struct FDisplay
    {
        TWeakObjectPtr<AActor> OwnedActor;
        bool bVisible = true;
    };

    /** Remove native delegate subscriptions when the module's static storage is released. */
    struct FDisplays
    {
        TMap<TWeakObjectPtr<AActor>, FDisplay> Owners;
        TMap<TWeakObjectPtr<UWorld>, FDelegateHandle> Worlds;

        ~FDisplays()
        {
            for (const auto& Pair : Worlds)
            {
                if (UWorld* World = Pair.Key.Get()) { World->RemoveOnActorDestroyedHandler(Pair.Value); }
            }
        }
    };
    static FDisplays Displays;

    /** Destroy only the transient armor actor created for a deleted customization manager. */
    static void ActorDestroyed(AActor* Actor)
    {
        FDisplay Display;
        if (Displays.Owners.RemoveAndCopyValue(Actor, Display))
        {
            if (AActor* Armor = Display.OwnedActor.Get(); IsValid(Armor) && Armor->GetOwner() == Actor)
            { Armor->Destroy(); }
        }
    }

    /** Create the existing reflected MenuArmor class without modifying any saved actor or asset. */
    static AActor* EnsureDisplay(FSuzieFunctionContext& Context, AActor* Manager)
    {
        auto* Existing = Context.GetProperty<AActor*>("GlobalArmor");
        if (IsValid(Existing))
        {
            if (FDisplay* Display = Displays.Owners.Find(Manager))
            {
                if (AActor* Previous = Display->OwnedActor.Get(); IsValid(Previous) && Previous != Existing && Previous->GetOwner() == Manager)
                {
                    Display->OwnedActor.Reset();
                    Previous->Destroy();
                }
            }
            return Existing;
        }
        UWorld* World = Manager->GetWorld();
        UClass* ArmorClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZMenuArmor"));
        if (!World || !ArmorClass || ArmorClass->HasAnyClassFlags(CLASS_Abstract)) { return nullptr; }

        for (auto It = Displays.Owners.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        for (auto It = Displays.Worlds.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        if (!Displays.Worlds.Contains(World))
        {
            Displays.Worlds.Add(World, World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateStatic(&ActorDestroyed)));
        }

        FActorSpawnParameters Spawn;
        Spawn.Owner = Manager;
        Spawn.ObjectFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;
        Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Armor = World->SpawnActor<AActor>(ArmorClass, Manager->GetActorTransform(), Spawn);
        if (!Armor) { return nullptr; }
        Armor->bIsEditorOnlyActor = true;
        if (!Context.SetProperty<AActor*>("GlobalArmor", Armor)) { Armor->Destroy(); return nullptr; }
        Displays.Owners.FindOrAdd(Manager).OwnedActor = Armor;
        return Armor;
    }

    /** Resolve the armor data's checked soft reference; loading runs only in deferred editor work. */
    static UObject* ResolveSet(FSuzieFunctionContext& Context, UObject* Data)
    {
        const auto* Property = FindFProperty<FSoftObjectProperty>(Data->GetClass(), TEXT("MeshData"));
        if (!Property || Property->ArrayDim != 1)
        {
            Context.Fail(TEXT("SBZArmorData.MeshData must be a soft object property"));
            return nullptr;
        }
        FSoftObjectPtr Reference = Property->GetPropertyValue_InContainer(Data);
        if (Reference.IsNull()) { return nullptr; }
        return Reference.LoadSynchronous();
    }

    /** Apply the selected full armor variation without guessing chunk bit-packing semantics. */
    static void Apply(FSuzieFunctionContext& Context)
    {
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
        auto* Manager = Cast<AActor>(Context.GetSelf());
        if (!SuzieVariation::Ready(Manager)) { return; }

        Context.ClearDependencies();
        UObject* Data = Context.Property("ArmorConfig").Field("Data").Get<UObject*>();
        if (IsValid(Data)) { Context.WatchDependency(Data, "MeshData"); }
        auto* Armor = Context.GetProperty<AActor*>("GlobalArmor");
        if (IsValid(Armor)) { Context.WatchDependency(Armor); }
        if (!Context.IsValid() || !SuzieVariation::Ready(Data)) { return; }

        UObject* Set = ResolveSet(Context, Data);
        if (IsValid(Set)) { Context.WatchDependency(Set); }
        if (!Context.IsValid() || !SuzieVariation::Ready(Set)) { return; }
        const UClass* SetClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZVariationSetData"));
        if (!SetClass || !Set->IsA(SetClass)) { return; }

        // Chunk-selection mode does not establish a variation-array index. Keep the
        // base entry in that mode rather than interpreting chunk enums as CPD masks.
        const bool bChunkSelection = Context.GetProperty<bool>("bIsArmorVariationChunkType");
        const int32 Index = bChunkSelection ? 0 : Context.GetProperty<int32>("ArmorVariationIndex");
        const int32 Count = Context.Property("Variations", Set).Num();
        if (!Context.IsValid() || Index < 0 || Index >= Count) { return; }

        Armor = EnsureDisplay(Context, Manager);
        if (!SuzieVariation::Ready(Armor) || !SuzieVariation::IsPreviewActor(Armor)) { return; }
        Context.WatchDependency(Armor);
        if (SuzieVariation::BindPreview(Armor, Set, Index))
        {
            const bool bVisible = Displays.Owners.FindOrAdd(Manager).bVisible;
            Armor->SetActorHiddenInGame(!bVisible);
            Armor->SetIsTemporarilyHiddenInEditor(!bVisible);
        }
        if (bChunkSelection)
        {
            UE_LOG(LogSuzieArmorVariation, Verbose, TEXT("%s: showing the base armor variation; chunk-specific CPD/material packing is not reconstructed"), *Manager->GetPathName());
        }
    }

    /** Copy the validated reflected config, preserving its dynamic chunk array and object references. */
    static bool StoreConfig(FSuzieFunctionContext& Context)
    {
        if (Context.IsTemplate() || IsRunningCommandlet()) { return false; }
        const auto* Source = FindFProperty<FStructProperty>(&Context.GetFunction(), TEXT("Config"));
        const auto* Target = FindFProperty<FStructProperty>(Context.GetSelf()->GetClass(), TEXT("ArmorConfig"));
        if (!Source || !Target || Source->ArrayDim != 1 || Target->ArrayDim != 1 || !Source->SameType(Target))
        {
            Context.Fail(TEXT("SetArmorConfig.Config must match the reflected ArmorConfig structure"));
            return false;
        }
        Target->CopyCompleteValue(Target->ContainerPtrToValuePtr<void>(Context.GetSelf()), Source->ContainerPtrToValuePtr<void>(Context.GetParameterMemory()));
        return true;
    }
}

/** Store the supplied config and resolve its armor mesh variation for the preview. */
SUZIE_FUNCTION(SBZCustomizationManager, SetArmorConfig)
{
    if (SuzieArmorCustomization::StoreConfig(Context)) { SuzieArmorCustomization::Apply(Context); }
}

/** Select an explicit full-variation index; unsupported indices leave the current display intact. */
SUZIE_FUNCTION(SBZCustomizationManager, SetArmorVariationIndex)
{
    if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
    const int32 Index = Context.GetParam<int32>("Index");
    if (Context.IsValid() && Index >= 0 && Context.SetProperty<int32>("ArmorVariationIndex", Index))
    { SuzieArmorCustomization::Apply(Context); }
}

/** Update visibility of the assigned armor display without changing its materials or CPD. */
SUZIE_FUNCTION(SBZCustomizationManager, SetArmorVisible)
{
    if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
    const bool bVisible = Context.GetParam<bool>("bInIsVisible");
    auto* Manager = Cast<AActor>(Context.GetSelf());
    if (!Context.IsValid() || !Manager) { return; }
    SuzieArmorCustomization::Displays.Owners.FindOrAdd(Manager).bVisible = bVisible;
    if (auto* Armor = Context.GetProperty<AActor*>("GlobalArmor"); SuzieVariation::Ready(Armor) && !Armor->IsTemplate())
    {
        Armor->SetActorHiddenInGame(!bVisible);
        Armor->SetIsTemporarilyHiddenInEditor(!bVisible);
    }
}

/** Find an existing customization manager in the supplied object's world; never spawn a gameplay manager. */
SUZIE_FUNCTION(SBZCustomizationManager, GetCustomizationManager)
{
    UObject* WorldContext = Context.GetParam<UObject*>("WorldContextObject");
    AActor* Result = nullptr;
    UWorld* World = GEngine && IsValid(WorldContext) ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
    UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZCustomizationManager"));
    if (World && Class)
    {
        for (TActorIterator<AActor> It(World, Class); It; ++It)
        {
            if (SuzieVariation::Ready(*It) && !It->IsTemplate()) { Result = *It; break; }
        }
    }
    Context.SetReturn<AActor*>(Result);
}

/** Restore an existing reflected config after required loading completes. */
SUZIE_POST_INITIALIZE(SBZCustomizationManager)
{
    SuzieArmorCustomization::Apply(Context);
}

/** Refresh when a config's data or chunk fields are edited. */
SUZIE_PROPERTY_CHANGED(SBZCustomizationManager, ArmorConfig)
{
    SuzieArmorCustomization::Apply(Context);
}

/** Refresh an explicit editor variation-index change. */
SUZIE_PROPERTY_CHANGED(SBZCustomizationManager, ArmorVariationIndex)
{
    SuzieArmorCustomization::Apply(Context);
}

/** Rebind an externally assigned display actor without taking ownership of its lifetime. */
SUZIE_PROPERTY_CHANGED(SBZCustomizationManager, GlobalArmor)
{
    SuzieArmorCustomization::Apply(Context);
}

/** Keep the base preview valid when switching to or from unsupported chunk-selection mode. */
SUZIE_PROPERTY_CHANGED(SBZCustomizationManager, bIsArmorVariationChunkType)
{
    SuzieArmorCustomization::Apply(Context);
}

/** Refresh after armor data, its soft variation asset or the display actor changes. */
SUZIE_REFRESH(SBZCustomizationManager)
{
    SuzieArmorCustomization::Apply(Context);
}
