/**
 * @file SBZStaticVehicle.cpp
 * @brief Restores static vehicle variation previews through Suzie lifecycle callbacks.
 *
 * This class derives from StaticMeshActor, not SBZWheeledVehicle. Its VariationData
 * therefore needs a separate caller targeting the inherited static mesh component.
 */

#include "SBZVariationNative.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

namespace SuzieStaticVehicleVariation
{
    /** Apply the retail seeded selection without overwriting the authored Seed. */
    static void Apply(FSuzieFunctionContext& Context)
    {
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
        auto* Actor = Cast<AStaticMeshActor>(Context.GetSelf());
        if (!Actor) { return; }

        Context.ClearDependencies();
        UObject* Set = Context.GetProperty<UObject*>("VariationData");
        if (IsValid(Set)) { Context.WatchDependency(Set); }
        UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
        if (IsValid(Mesh)) { Context.WatchDependency(Mesh, "StaticMesh"); }
        Context.WatchDependency(Actor->GetArchetype(), "VariationData");
        Context.WatchDependency(Actor->GetArchetype(), "Seed");

        if (!Context.IsValid() || !SuzieVariation::Ready(Set) || !SuzieVariation::Ready(Mesh)) { return; }
        if (Context.Property("Variations", Set).Num() == 0) { return; }

        UObject* SelectedMesh = Context.Property("StaticMesh", Set).Get<UObject*>();
        if (IsValid(SelectedMesh)) { Context.WatchDependency(SelectedMesh); }
        const auto Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UObject*>>();
        for (UObject* Material : Materials) { if (IsValid(Material)) { Context.WatchDependency(Material); } }

        const int32 Index = SuzieVariation::GetVehicleVariationIndex(Context, Set, true);
        if (Context.IsValid() && Index != INDEX_NONE) { SuzieVariation::CallUtility(Context, Mesh, Set, Index); }
    }
}

/** Apply the variation once saved defaults and required loading are ready. */
SUZIE_POST_INITIALIZE(SBZStaticVehicle)
{
    SuzieStaticVehicleVariation::Apply(Context);
}

/** Rebind dependencies and apply a newly assigned variation asset. */
SUZIE_PROPERTY_CHANGED(SBZStaticVehicle, VariationData)
{
    SuzieStaticVehicleVariation::Apply(Context);
}

/** Recompute the deterministic selection after editing the authored seed. */
SUZIE_PROPERTY_CHANGED(SBZStaticVehicle, Seed)
{
    SuzieVariation::PreserveVehicleSeed(Cast<AActor>(Context.GetSelf()));
    SuzieStaticVehicleVariation::Apply(Context);
}

/** Reapply the variation when the inherited component reference changes. */
SUZIE_PROPERTY_CHANGED(SBZStaticVehicle, StaticMeshComponent)
{
    SuzieStaticVehicleVariation::Apply(Context);
}

/** Refresh after a watched mesh, material, variation asset or archetype changes. */
SUZIE_REFRESH(SBZStaticVehicle)
{
    SuzieStaticVehicleVariation::Apply(Context);
}
