/**
 * @file SBZVariationEditComponent.cpp
 * @brief Applies the variation editor component's selection through Suzie native bodies.
 *
 * The reflected component comes from the JMAP. These callbacks update its assigned preview
 * components when the selection or a watched dependency changes, without editing the data asset.
 */

#include "SBZVariationNative.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"

namespace SuzieVariationEditor
{
    /**
     * Refresh the assigned preview components from VariationSet and CurrentVariationIdx.
     *
     * Rebuild dependency subscriptions before checking readiness so referenced assets can
     * trigger another refresh once loading finishes. An unselected index previews entry zero.
     *
     * @param Context Access to the editor component and its reflected selection properties.
     */
    static void Apply(FSuzieFunctionContext& Context)
    {
        // Preview updates must not change shared templates or run during commandlets.
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
        // Replace subscriptions to the previous selection with the current dependencies.
        Context.ClearDependencies();
        UObject* Set = Context.GetProperty<UObject*>("VariationSet");
        if (IsValid(Set)) { Context.WatchDependency(Set); }
        auto* Skeletal = Context.GetProperty<USkeletalMeshComponent*>("SkeletalMeshComponent");
        auto* Static = Context.GetProperty<UStaticMeshComponent*>("StaticMeshComponent");
        if (IsValid(Skeletal)) { Context.WatchDependency(Skeletal, "SkeletalMesh"); }
        if (IsValid(Static)) { Context.WatchDependency(Static, "StaticMesh"); }
        if (!Context.IsValid() || !SuzieVariation::Ready(Set)) { return; }
        const int32 SelectedIndex = Context.GetProperty<int32>("CurrentVariationIdx");
        const int32 Index = SelectedIndex == INDEX_NONE ? 0 : SelectedIndex;
        // Watch the selected mesh and non-null overrides as well as the variation asset.
        UObject* Mesh = Context.Property("SkeletalMesh", Set).Get<UObject*>();
        if (!Mesh) { Mesh = Context.Property("StaticMesh", Set).Get<UObject*>(); }
        if (IsValid(Mesh)) { Context.WatchDependency(Mesh); }
        const auto Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UObject*>>();
        for (UObject* Material : Materials) { if (IsValid(Material)) { Context.WatchDependency(Material); } }
        if (Context.IsValid())
        {
            // Only update a preview component when the set supplies its mesh type.
            if (Skeletal && Context.Property("SkeletalMesh", Set).Get<UObject*>()) { SuzieVariation::CallUtility(Context, Skeletal, Set, Index); }
            if (Static && Context.Property("StaticMesh", Set).Get<UObject*>()) { SuzieVariation::CallUtility(Context, Static, Set, Index); }
        }
    }
}

/** Apply the initial selection after defaults and required object loading are ready. */
SUZIE_POST_INITIALIZE(SBZVariationEditComponent)
{
    SuzieVariationEditor::Apply(Context);
}

/** Apply the new variation asset and rebuild its dependency subscriptions. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditComponent, VariationSet)
{
    SuzieVariationEditor::Apply(Context);
}

/** Refresh the preview when the selected variation index changes in the editor. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditComponent, CurrentVariationIdx)
{
    SuzieVariationEditor::Apply(Context);
}

/** Apply the current selection to a newly assigned skeletal preview component. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditComponent, SkeletalMeshComponent)
{
    SuzieVariationEditor::Apply(Context);
}

/** Apply the current selection to a newly assigned static preview component. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditComponent, StaticMeshComponent)
{
    SuzieVariationEditor::Apply(Context);
}

/** Reapply the selection after a watched asset or preview mesh changes. */
SUZIE_REFRESH(SBZVariationEditComponent)
{
    SuzieVariationEditor::Apply(Context);
}
