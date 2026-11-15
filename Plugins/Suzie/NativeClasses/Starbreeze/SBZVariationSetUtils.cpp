/**
 * @file SBZVariationSetUtils.cpp
 * @brief Applies PAYDAY 3 mesh variations through the existing Suzie-generated utility functions.
 *
 * Read the current JMAP's variation layout through reflection and use Unreal component APIs
 * for mesh assignment, material overrides, morph weights and runtime/default Custom Primitive Data.
 * Collision settings remain unchanged because their retail bit interpretation is unverified.
 */

#include "SBZVariationNative.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Editor.h"
#include "Misc/Guid.h"

namespace SuzieVariation
{
    DEFINE_LOG_CATEGORY_STATIC(LogSuzieVariation, Log, All);

    /** Assign appearance seeds only for explicit editor placements, never on load or refresh. */
    struct FPlacementSeeds
    {
        /** Weak instance identities deduplicate placement/drop notifications and protect manual edits. */
        TSet<TWeakObjectPtr<AActor>> Assigned;
        FDelegateHandle Dropped;
        FDelegateHandle Placed;

        FPlacementSeeds()
        {
            Dropped = FEditorDelegates::OnNewActorsDropped.AddRaw(this, &FPlacementSeeds::OnDropped);
            Placed = FEditorDelegates::OnNewActorsPlaced.AddRaw(this, &FPlacementSeeds::OnPlaced);
        }

        ~FPlacementSeeds()
        {
            FEditorDelegates::OnNewActorsDropped.Remove(Dropped);
            FEditorDelegates::OnNewActorsPlaced.Remove(Placed);
        }

        void Preserve(AActor* Actor)
        {
            for (auto It = Assigned.CreateIterator(); It; ++It) { if (!It->IsValid()) { It.RemoveCurrent(); } }
            if (IsValid(Actor) && !Actor->IsTemplate()) { Assigned.Add(Actor); }
        }

        /** Both notifications can describe the same placement; preview actors are never seeded. */
        void SeedActors(const TArray<AActor*>& Actors)
        {
            if (!IsInGameThread() || IsRunningCommandlet()) { return; }
            for (AActor* Actor : Actors)
            {
                if (!Ready(Actor) || Actor->IsTemplate() || Actor->HasAnyFlags(RF_Transient) ||
                    !Actor->GetWorld() || Actor->GetWorld()->WorldType != EWorldType::Editor || Assigned.Contains(Actor)) { continue; }
                const UClass* Wheeled = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZWheeledVehicle"));
                const UClass* Static = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZStaticVehicle"));
                const UClass* AI = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZAICharacter"));
                const UClass* Spawner = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZPawnSpawnPredefined"));
                const bool bCharacter = AI && Actor->IsA(AI);
                const bool bSpawner = Spawner && Actor->IsA(Spawner);
                if (!bCharacter && !bSpawner && !(Wheeled && Actor->IsA(Wheeled)) && !(Static && Actor->IsA(Static))) { continue; }
                FIntProperty* Property = FindFProperty<FIntProperty>(Actor->GetClass(), TEXT("Seed"));
                if (!Property) { continue; }

                // Randomness is used only to author this one saved integer. Appearance
                // selection remains deterministic. AI's signed remainder clamps negative
                // seeds to its first category entry. Spawner previews use that same
                // appearance rule, so generate nonnegative AI/spawner seeds.
                FRandomStream Stream(static_cast<int32>(GetTypeHash(FGuid::NewGuid())));
                const int32 Seed = Stream.RandRange((bCharacter || bSpawner) ? 0 : -10000, 10000);
                Actor->Modify();
                Preserve(Actor);
                FSuzieFunctionContext Context(Actor);
                if (!Context.SetProperty<int32>("Seed", Seed)) { continue; }
                FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
                Actor->PostEditChangeProperty(Event);
                Context.RequestRefresh();
            }
        }

        void OnDropped(const TArray<UObject*>&, const TArray<AActor*>& Actors) { SeedActors(Actors); }
        void OnPlaced(UObject*, const TArray<AActor*>& Actors) { SeedActors(Actors); }
    };

    // The editor dependency is loaded before Suzie. Unregister both callbacks when
    // this module unloads, so native-body reloads cannot leave stale function pointers.
    static FPlacementSeeds PlacementSeeds;

    void PreserveVehicleSeed(AActor* Actor) { PlacementSeeds.Preserve(Actor); }
    void PreserveCharacterSeed(AActor* Actor) { PlacementSeeds.Preserve(Actor); }
    void PreserveSpawnerSeed(AActor* Actor) { PlacementSeeds.Preserve(Actor); }

    /** Weak material references used to restore an override previously applied by this utility. */
    struct FMaterialSlot
    {
        /** Effective material before the utility took ownership of this slot. */
        TWeakObjectPtr<UMaterialInterface> Before;
        /** Last material assigned by the utility; used to detect subsequent external edits. */
        TWeakObjectPtr<UMaterialInterface> Applied;
    };

    /** Material overrides owned by the utility for one component and its last selected mesh. */
    struct FMaterialState
    {
        /** Mesh associated with the saved slot state. */
        TWeakObjectPtr<UObject> Mesh;
        /** Original and applied material references, keyed by material slot index. */
        TMap<int32, FMaterialSlot> Slots;
    };
    // Track only overrides owned by this utility. Weak references neither retain
    // deleted preview components nor keep their asset dependencies alive.
    static TMap<TWeakObjectPtr<UMeshComponent>, FMaterialState> MaterialStates;

    /**
     * Restore obsolete utility-owned overrides and assign the new non-null materials by slot.
     *
     * Keep external edits intact when retiring an old override. Use the selected mesh's default
     * material if the old baseline is unavailable or belongs to another mesh.
     *
     * @param Component Ready skeletal or static mesh component to update.
     * @param Mesh Mesh currently assigned to the component.
     * @param Materials Variation override array in material-slot order. Null entries are not assigned.
     */
    static void ApplyMaterials(UMeshComponent* Component, UObject* Mesh, const TArray<UMaterialInterface*>& Materials)
    {
        for (auto It = MaterialStates.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        FMaterialState& State = MaterialStates.FindOrAdd(Component);
        for (auto It = State.Slots.CreateIterator(); It; ++It)
        {
            const int32 Slot = It.Key();
            if (Slot >= Component->GetNumMaterials()) { It.RemoveCurrent(); continue; }
            if (State.Mesh == Mesh && Materials.IsValidIndex(Slot) && Materials[Slot] == It.Value().Applied.Get()) { continue; }
            // Preserve edits made by somebody else after our last application.
            if (Component->GetMaterial(Slot) == It.Value().Applied.Get())
            {
                UMaterialInterface* Restore = State.Mesh == Mesh ? It.Value().Before.Get() : nullptr;
                if (!Restore)
                {
                    if (auto* Skeletal = Cast<USkeletalMesh>(Mesh); Skeletal && Skeletal->GetMaterials().IsValidIndex(Slot))
                    { Restore = Skeletal->GetMaterials()[Slot].MaterialInterface; }
                    if (auto* Static = Cast<UStaticMesh>(Mesh); Static && Static->GetStaticMaterials().IsValidIndex(Slot))
                    { Restore = Static->GetStaticMaterials()[Slot].MaterialInterface; }
                }
                if (Restore) { Component->SetMaterial(Slot, Restore); }
            }
            It.RemoveCurrent();
        }
        State.Mesh = Mesh;
        for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
        {
            if (Slot >= Component->GetNumMaterials())
            {
                if (Materials[Slot]) { UE_LOG(LogSuzieVariation, Warning, TEXT("%s: variation material slot %d does not exist"), *Component->GetPathName(), Slot); }
                continue;
            }
            if (Materials[Slot] && Component->GetMaterial(Slot) != Materials[Slot])
            {
                FMaterialSlot& Entry = State.Slots.FindOrAdd(Slot);
                Entry.Before = Component->GetMaterial(Slot);
                Entry.Applied = Materials[Slot];
                Component->SetMaterial(Slot, Materials[Slot]);
            }
        }
    }

    /** @copydoc SuzieVariation::Ready */
    bool Ready(const UObject* Object)
    {
        return IsValid(Object) && !Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad |
            RF_NeedPostLoad | RF_NeedPostLoadSubobjects) &&
            !Object->HasAnyInternalFlags(EInternalObjectFlags_AsyncLoading | EInternalObjectFlags::Async);
    }

    /** @copydoc SuzieVariation::GetVehicleVariationIndex */
    int32 GetVehicleVariationIndex(FSuzieFunctionContext& Context, UObject* Set, bool bMixMissionSeed)
    {
        if (!IsInGameThread() || Context.IsTemplate() || !Ready(Set)) { return INDEX_NONE; }
        auto* Actor = Cast<AActor>(Context.GetSelf());
        if (!Actor) { return INDEX_NONE; }

        const int32 Count = Context.Property("Variations", Set).Num();
        int32 Seed = Context.GetProperty<int32>("Seed");
        if (!Context.IsValid() || Count <= 0) { return INDEX_NONE; }

        if (bMixMissionSeed)
        {
            if (UWorld* World = Actor->GetWorld())
            {
                Context.WatchDependency(World, "GameState");
                AGameStateBase* State = World->GetGameState();
                // A state still loading must not be read as an initialized mission.
                if (State && !Ready(State)) { return INDEX_NONE; }
                const UClass* MissionClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZMissionState"));
                if (State && MissionClass && State->IsA(MissionClass))
                {
                    Context.WatchDependency(State, "RandomSeed");
                    Seed ^= Context.Property("RandomSeed", State).Get<int32>();
                }
            }
        }

        if (!Context.IsValid()) { return INDEX_NONE; }
        // Retail takes one LCG step, uses its high 23 bits to construct [0, 1),
        // then truncates Fraction * Count. UE 5.5's RandHelper does exactly that.
        // Negative seeds, including the default -1, are ordinary stream seeds.
        FRandomStream Stream(Seed);
        return Stream.RandHelper(Count);
    }

    /** @copydoc SuzieVariation::ApplyToMesh */
    bool ApplyToMesh(FSuzieFunctionContext& Context, UMeshComponent* Component, UObject* Set, int32 Index)
    {
        if (!IsInGameThread() || !Ready(Component) || Component->IsTemplate() || !Ready(Set) ||
            (Component->GetOwner() && Component->GetOwner()->IsTemplate())) { return false; }
        UClass* SetClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZVariationSetData"));
        if (!SetClass || !Set->IsA(SetClass)) { return false; }
        USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component);
        UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component);
        if (!Skeletal && !Static) { return false; }

        auto Variations = Context.Property("Variations", Set);
        if (Index < 0 || Index >= Variations.Num())
        {
            UE_LOG(LogSuzieVariation, Warning, TEXT("%s: variation index %d is outside %s (%d entries)"),
                *Component->GetPathName(), Index, *Set->GetPathName(), Variations.Num());
            return false;
        }

        // Data is a reflected C array, not a TArray. The value-view API deliberately
        // supports dynamic arrays only, so validate the live schema before reading it.
        const FArrayProperty* Entries = FindFProperty<FArrayProperty>(Set->GetClass(), TEXT("Variations"));
        const FStructProperty* EntryType = Entries ? CastField<FStructProperty>(Entries->Inner) : nullptr;
        const FFloatProperty* Data = EntryType ? FindFProperty<FFloatProperty>(EntryType->Struct, TEXT("Data")) : nullptr;
        if (!Data || Data->ArrayDim != 36 || FCustomPrimitiveData::NumCustomPrimitiveDataFloats < Data->ArrayDim)
        {
            Context.Fail(TEXT("SBZCustomPrimitiveData must contain 36 float Data elements supported by the renderer"));
            return false;
        }
        FScriptArrayHelper Values(Entries, Entries->ContainerPtrToValuePtr<void>(Set));
        TArray<float> CPD;
        CPD.Reserve(Data->ArrayDim);
        for (int32 I = 0; I < Data->ArrayDim; ++I)
        {
            // Values already include retail's numeric pattern/color/bit packing.
            // Do not reinterpret their bits or remap zeros to visibility defaults.
            const float Value = Data->GetPropertyValue(Data->ContainerPtrToValuePtr<void>(Values.GetRawPtr(Index), I));
            if (!FMath::IsFinite(Value)) { return false; }
            CPD.Add(Value);
        }
        // Validate referenced assets and weights before assigning the selected mesh or materials.
        const TMap<FName, float> Morphs = Variations.Element(Index).Field("MorphTargets").Get<TMap<FName, float>>();
        const TArray<UMaterialInterface*> Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UMaterialInterface*>>();
        UObject* Mesh = Context.Property(Skeletal ? FName(TEXT("SkeletalMesh")) : FName(TEXT("StaticMesh")), Set).Get<UObject*>();
        // Mesh is a deprecated skeletal reference retained in older data assets.
        if (Skeletal && !Mesh) { Mesh = Context.Property("Mesh", Set).Get<UObject*>(); }
        if (!Context.IsValid() || (Mesh && !Ready(Mesh))) { return false; }
        for (UMaterialInterface* Material : Materials) { if (Material && !Ready(Material)) { return false; } }
        for (const auto& Morph : Morphs) { if (!FMath::IsFinite(Morph.Value)) { return false; } }
        if (Skeletal && Mesh && !Cast<USkeletalMesh>(Mesh)) { return false; }
        if (Static && Mesh && !Cast<UStaticMesh>(Mesh)) { return false; }
        if (Skeletal)
        {
            if (Mesh && Skeletal->GetSkeletalMeshAsset() != Mesh) { Skeletal->SetSkeletalMesh(CastChecked<USkeletalMesh>(Mesh)); }
            if (!Ready(Skeletal->GetSkeletalMeshAsset())) { return false; }
        }
        else
        {
            if (Mesh && Static->GetStaticMesh() != Mesh && !Static->SetStaticMesh(CastChecked<UStaticMesh>(Mesh))) { return false; }
            if (!Ready(Static->GetStaticMesh())) { return false; }
        }
        ApplyMaterials(Component, Skeletal ? static_cast<UObject*>(Skeletal->GetSkeletalMeshAsset()) : Static->GetStaticMesh(), Materials);
        if (Skeletal)
        {
            // A new variation replaces its predecessor's morph state, including weights
            // for names absent from this entry. Missing mesh targets are harmless.
            Skeletal->ClearMorphTargets();
            for (const auto& Morph : Morphs)
            {
                if (Skeletal->GetSkeletalMeshAsset()->FindMorphTarget(Morph.Key)) { Skeletal->SetMorphTarget(Morph.Key, Morph.Value); }
            }
        }
        if (Component->GetDefaultCustomPrimitiveData().Data != CPD)
        {
            // The bulk default setter is protected. Public vector setters cover the
            // complete fixed layout, and survive component re-registration/reset.
            for (int32 I = 0; I < CPD.Num(); I += 4)
            { Component->SetDefaultCustomPrimitiveDataVector4(I, FVector4(CPD[I], CPD[I+1], CPD[I+2], CPD[I+3])); }
        }
        // Default setters may reset runtime data. Finish with the exact runtime array and refresh rendering.
        if (Component->GetCustomPrimitiveData().Data != CPD) { Component->SetCustomPrimitiveData(CPD); }
        Component->MarkRenderStateDirty();
        // CDPFormat describes authoring fields. Its collision-name rows do not
        // establish bit polarity/body ownership; do not guess or change physics.
        UE_LOG(LogSuzieVariation, Verbose, TEXT("Applied %s[%d] to %s: %d CPD floats, %d morph entries"),
            *Set->GetPathName(), Index, *Component->GetPathName(), CPD.Num(), Morphs.Num());
        return true;
    }

    /** @copydoc SuzieVariation::CallUtility */
    bool CallUtility(FSuzieFunctionContext& Context, UMeshComponent* Component, UObject* Set, int32 Index)
    {
        UClass* Utils = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZVariationSetUtils"));
        if (!Utils) { return false; }
        auto Result = Context.CallFunction("ApplyVariationToMesh", [=](FSuzieFunctionContext& Call)
        {
            Call.SetParam<UMeshComponent*>("MeshComponent", Component);
            Call.SetParam<UObject*>("VariationSet", Set);
            Call.SetParam<int32>("VariationIdx", Index);
        }, Utils->GetDefaultObject());
        return Result && Result->IsValid() && Result->GetReturn<bool>();
    }

    /** Caller-owned preview selections; neither actors nor data assets are retained by the registry. */
    struct FPreviewSelection
    {
        TWeakObjectPtr<UObject> Set;
        int32 Index = 0;
    };
    static TMap<TWeakObjectPtr<AActor>, FPreviewSelection> PreviewSelections;

    bool IsPreviewActor(const AActor* Actor)
    {
        if (!IsValid(Actor)) { return false; }
        for (const TCHAR* Path : { TEXT("/Script/Starbreeze.SBZVariationViewActor"), TEXT("/Script/Starbreeze.SBZMenuArmor") })
        {
            const UClass* Class = FindObject<UClass>(nullptr, Path);
            if (Class && Actor->IsA(Class)) { return true; }
        }
        return false;
    }

    bool RefreshPreview(FSuzieFunctionContext& Context)
    {
        auto* Actor = Cast<AActor>(Context.GetSelf());
        if (!IsPreviewActor(Actor) || Context.IsTemplate() || IsRunningCommandlet()) { return false; }

        Context.ClearDependencies();
        const FPreviewSelection* Found = PreviewSelections.Find(Actor);
        if (!Found) { return false; }
        const FPreviewSelection Selection = *Found;
        UObject* Set = Selection.Set.Get();
        if (IsValid(Set)) { Context.WatchDependency(Set); }

        auto* Skeletal = Context.GetProperty<USkeletalMeshComponent*>("SkeletalMeshComponent");
        auto* Static = Context.GetProperty<UStaticMeshComponent*>("StaticMeshComponent");
        if (IsValid(Skeletal)) { Context.WatchDependency(Skeletal, "SkeletalMesh"); }
        if (IsValid(Static)) { Context.WatchDependency(Static, "StaticMesh"); }
        if (!Context.IsValid() || !Ready(Set)) { return false; }

        UObject* SkeletalAsset = Context.Property("SkeletalMesh", Set).Get<UObject*>();
        if (!SkeletalAsset) { SkeletalAsset = Context.Property("Mesh", Set).Get<UObject*>(); }
        UObject* StaticAsset = Context.Property("StaticMesh", Set).Get<UObject*>();
        if (IsValid(SkeletalAsset)) { Context.WatchDependency(SkeletalAsset); }
        if (IsValid(StaticAsset)) { Context.WatchDependency(StaticAsset); }
        const auto Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UObject*>>();
        for (UObject* Material : Materials) { if (IsValid(Material)) { Context.WatchDependency(Material); } }
        if (!Context.IsValid()) { return false; }

        const bool bSkeletalApplied = SkeletalAsset && Ready(Skeletal) && CallUtility(Context, Skeletal, Set, Selection.Index);
        const bool bStaticApplied = StaticAsset && Ready(Static) && CallUtility(Context, Static, Set, Selection.Index);
        // Preview actors carry both component types. Hide the unused representation only
        // after the new representation succeeds; a missing asset must not erase the old preview.
        if (bSkeletalApplied || bStaticApplied)
        {
            if (Ready(Skeletal) && !Skeletal->IsTemplate()) { Skeletal->SetVisibility(bSkeletalApplied, true); }
            if (Ready(Static) && !Static->IsTemplate()) { Static->SetVisibility(bStaticApplied, true); }
        }
        return bSkeletalApplied || bStaticApplied;
    }

    bool BindPreview(AActor* Actor, UObject* Set, int32 Index)
    {
        if (!IsPreviewActor(Actor) || Actor->IsTemplate() || IsRunningCommandlet() || Index < 0) { return false; }
        if (!IsValid(Set)) { return false; }
        const UClass* SetClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZVariationSetData"));
        if (!SetClass || !Set->IsA(SetClass)) { return false; }
        FSuzieFunctionContext Preview(Actor);
        if (Ready(Set))
        {
            const int32 Count = Preview.Property("Variations", Set).Num();
            if (!Preview.IsValid() || Index >= Count) { return false; }
        }
        for (auto It = PreviewSelections.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
        PreviewSelections.Add(Actor, { Set, Index });
        return RefreshPreview(Preview);
    }
}

/**
 * Apply the requested variation to the reflected SkelComponent parameter.
 *
 * @param Context Supplies SkelComponent, VariationSet and VariationIdx.
 * @note The reflected bool ReturnValue reports whether application succeeded.
 */
SUZIE_FUNCTION(SBZVariationSetUtils, ApplyVariationToSkeletal)
{
    auto* Component = Context.GetParam<USkeletalMeshComponent*>("SkelComponent");
    UObject* Set = Context.GetParam<UObject*>("VariationSet");
    const int32 Index = Context.GetParam<int32>("VariationIdx");
    Context.SetReturn<bool>(Context.IsValid() && SuzieVariation::ApplyToMesh(Context, Component, Set, Index));
}

/**
 * Apply the requested variation to the reflected StatComponent parameter.
 *
 * @param Context Supplies StatComponent, VariationSet and VariationIdx.
 * @note The reflected bool ReturnValue reports whether application succeeded.
 */
SUZIE_FUNCTION(SBZVariationSetUtils, ApplyVariationToStatic)
{
    auto* Component = Context.GetParam<UStaticMeshComponent*>("StatComponent");
    UObject* Set = Context.GetParam<UObject*>("VariationSet");
    const int32 Index = Context.GetParam<int32>("VariationIdx");
    Context.SetReturn<bool>(Context.IsValid() && SuzieVariation::ApplyToMesh(Context, Component, Set, Index));
}

/**
 * Apply the requested variation to a supported skeletal or static mesh component.
 *
 * @param Context Supplies MeshComponent, VariationSet and VariationIdx.
 * @note Unsupported component types produce a false reflected ReturnValue.
 */
SUZIE_FUNCTION(SBZVariationSetUtils, ApplyVariationToMesh)
{
    auto* Component = Context.GetParam<UMeshComponent*>("MeshComponent");
    UObject* Set = Context.GetParam<UObject*>("VariationSet");
    const int32 Index = Context.GetParam<int32>("VariationIdx");
    Context.SetReturn<bool>(Context.IsValid() && SuzieVariation::ApplyToMesh(Context, Component, Set, Index));
}

/**
 * Resolve an actor's mesh component and apply the requested variation.
 *
 * Prefer a character's primary mesh. For other actors, use the first component matching
 * the set's supplied mesh type, or the first mesh component when neither type is assigned.
 *
 * @param Context Supplies Actor, VariationSet and VariationIdx.
 * @note The reflected bool ReturnValue reports whether a compatible component was updated.
 */
SUZIE_FUNCTION(SBZVariationSetUtils, ApplyVariation)
{
    AActor* Actor = Context.GetParam<AActor*>("Actor");
    UObject* Set = Context.GetParam<UObject*>("VariationSet");
    const int32 Index = Context.GetParam<int32>("VariationIdx");
    UMeshComponent* Component = nullptr;
    if (Context.IsValid() && SuzieVariation::IsPreviewActor(Actor))
    {
        Context.SetReturn<bool>(SuzieVariation::BindPreview(Actor, Set, Index));
        return;
    }
    if (SuzieVariation::Ready(Actor) && !Actor->IsTemplate() && SuzieVariation::Ready(Set))
    {
        if (ACharacter* Character = Cast<ACharacter>(Actor)) { Component = Character->GetMesh(); }
        else if (Context.Property("SkeletalMesh", Set).Get<UObject*>()) { Component = Actor->FindComponentByClass<USkeletalMeshComponent>(); }
        else if (Context.Property("StaticMesh", Set).Get<UObject*>()) { Component = Actor->FindComponentByClass<UStaticMeshComponent>(); }
        else { Component = Actor->FindComponentByClass<UMeshComponent>(); }
    }
    Context.SetReturn<bool>(Context.IsValid() && SuzieVariation::ApplyToMesh(Context, Component, Set, Index));
}
