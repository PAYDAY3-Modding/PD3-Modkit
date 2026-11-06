/**
 * @file SBZWheeledVehicle.cpp
 * @brief Applies variation CPD to Suzie-generated vehicle instances in the editor.
 *
 * Vehicles store their set in VariationSetData rather than the AI character's VariationData.
 * Retail selects one entry using a local FRandomStream seeded by the actor and mission.
 * Editor callbacks apply the same selection to the inherited Mesh after loading and
 * construction have finished, without overwriting the saved seed.
 */

#include "SBZVariationNative.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

namespace SuzieWheeledVehicleVariation
{
    /**
     * Apply the vehicle's deterministic variation and rebuild dependency subscriptions.
     *
     * Replication supplies an already-mixed seed. Deferred editor previews mix authored
     * seeds locally; client-world refreshes also retain their replicated effective seed.
     *
     * @param Context Access to the vehicle's reflected properties and deferred refresh API.
     * @param bSeedAlreadyMixed True for the native OnRep_Seed call.
     */
    static void Apply(FSuzieFunctionContext& Context, bool bSeedAlreadyMixed = false)
    {
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }

        Context.ClearDependencies();
        UObject* Set = Context.GetProperty<UObject*>("VariationSetData");
        if (IsValid(Set)) { Context.WatchDependency(Set); }

        auto* Mesh = Context.GetProperty<USkeletalMeshComponent*>("Mesh");
        if (IsValid(Mesh)) { Context.WatchDependency(Mesh, "SkeletalMesh"); }

        // Instance previews also need to follow inherited Blueprint default changes.
        Context.WatchDependency(Context.GetSelf()->GetArchetype(), "VariationSetData");
        Context.WatchDependency(Context.GetSelf()->GetArchetype(), "Seed");

        if (!Context.IsValid() || !SuzieVariation::Ready(Set) || !SuzieVariation::Ready(Mesh)) { return; }
        if (Context.Property("Variations", Set).Num() == 0) { return; }

        UObject* SelectedMesh = Context.Property("SkeletalMesh", Set).Get<UObject*>();
        if (!SelectedMesh) { SelectedMesh = Context.Property("Mesh", Set).Get<UObject*>(); }
        if (IsValid(SelectedMesh)) { Context.WatchDependency(SelectedMesh); }

        const auto Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UObject*>>();
        for (UObject* Material : Materials)
        {
            if (IsValid(Material)) { Context.WatchDependency(Material); }
        }

        const auto* Actor = Cast<AActor>(Context.GetSelf());
        const bool bMixMissionSeed = !bSeedAlreadyMixed && Actor && Actor->GetNetMode() != NM_Client;
        const int32 Index = SuzieVariation::GetVehicleVariationIndex(Context, Set, bMixMissionSeed);
        if (Context.IsValid() && Index != INDEX_NONE) { SuzieVariation::CallUtility(Context, Mesh, Set, Index); }
    }
}

/** Apply the preview after the vehicle's defaults and required loading are ready. */
SUZIE_POST_INITIALIZE(SBZWheeledVehicle)
{
    SuzieWheeledVehicleVariation::Apply(Context);
}

/** Apply a newly assigned variation set and update its dependency subscriptions. */
SUZIE_PROPERTY_CHANGED(SBZWheeledVehicle, VariationSetData)
{
    SuzieWheeledVehicleVariation::Apply(Context);
}

/** Recompute the deterministic selection after editing the authored seed. */
SUZIE_PROPERTY_CHANGED(SBZWheeledVehicle, Seed)
{
    SuzieVariation::PreserveVehicleSeed(Cast<AActor>(Context.GetSelf()));
    SuzieWheeledVehicleVariation::Apply(Context);
}

/** Apply the already-mixed seed received through the retail replication callback. */
SUZIE_FUNCTION(SBZWheeledVehicle, OnRep_Seed)
{
    SuzieWheeledVehicleVariation::Apply(Context, true);
}

/** Apply the current variation when the vehicle mesh component is replaced. */
SUZIE_PROPERTY_CHANGED(SBZWheeledVehicle, Mesh)
{
    SuzieWheeledVehicleVariation::Apply(Context);
}

/** Refresh the preview after a watched variation asset, mesh or material changes. */
SUZIE_REFRESH(SBZWheeledVehicle)
{
    SuzieWheeledVehicleVariation::Apply(Context);
}
