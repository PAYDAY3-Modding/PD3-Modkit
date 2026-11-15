/**
 * @file SBZVariationNative.h
 * @brief Shares variation helpers between the Suzie native bodies and editor callbacks.
 *
 * The game classes and variation structures remain defined by the JMAP. These helpers
 * access their reflected properties without declaring replacement Starbreeze classes.
 */

#pragma once

#include "SuzieFunctionContext.h"

class UMeshComponent;
class AActor;

namespace SuzieVariation
{
    /**
     * Check that an object is ready before reading its properties or changing its component state.
     *
     * @param Object Object or asset to check. May be null.
     * @return True when the object is valid and initialization and loading have finished.
     */
    bool Ready(const UObject* Object);

    /** Record a vehicle seed edit so a pending placement/drop event cannot replace it. */
    void PreserveVehicleSeed(AActor* Actor);

    /** Preserve an authored AI seed against subsequent placement/drop notifications. */
    void PreserveCharacterSeed(AActor* Actor);

    /** Preserve a manually authored pawn-spawner seed during a pending placement. */
    void PreserveSpawnerSeed(AActor* Actor);

    /** Apply the proven AI appearance path for a seed edit or inherited seed rep-notify. */
    void ApplyCharacterSeed(FSuzieFunctionContext& Context);

    /**
     * Select the retail vehicle variation using the first draw of a local random stream.
     *
     * Initialization XORs the authored seed with the world's SBZMissionState.RandomSeed.
     * OnRep_Seed receives the effective seed and must skip that mix. Editor previews keep
     * the authored property intact so refresh, duplication and reload cannot mix it twice.
     * An editor world without a mission state uses the authored seed directly.
     *
     * @param Context Vehicle properties and dependency subscriptions for editor refresh.
     * @param Set Ready variation asset whose Variations array defines the selection range.
     * @param bMixMissionSeed Whether Seed is authored and needs the mission-seed XOR.
     * @return Selected index, or INDEX_NONE when required data is unavailable or empty.
     */
    int32 GetVehicleVariationIndex(FSuzieFunctionContext& Context, UObject* Set, bool bMixMissionSeed);

    /**
     * Apply a variation entry to a skeletal or static mesh component.
     *
     * Assign the available mesh and material overrides, replace runtime and default CPD,
     * and apply matching skeletal morph weights. Leave templates and collision settings alone.
     *
     * @param Context Invocation-scoped access to the reflected variation data and diagnostics.
     * @param Component Component to update. Must be a ready, non-template instance.
     * @param Set SBZVariationSetData asset containing the mesh references and variation entries.
     * @param Index Zero-based index into the asset's Variations array.
     * @return True when the variation was applied; false for unsupported or unavailable data.
     */
    bool ApplyToMesh(FSuzieFunctionContext& Context, UMeshComponent* Component, UObject* Set, int32 Index);

    /**
     * Invoke the reflected ApplyVariationToMesh function from an editor callback.
     *
     * @param Context Caller context used to set up the reflected function call.
     * @param Component Target mesh component.
     * @param Set Variation data asset passed to the utility.
     * @param Index Zero-based variation entry to apply.
     * @return True when the utility is available and reports successful application.
     */
    bool CallUtility(FSuzieFunctionContext& Context, UMeshComponent* Component, UObject* Set, int32 Index);

    /** Return whether the actor is a reflected variation viewer or menu armor preview. */
    bool IsPreviewActor(const AActor* Actor);

    /**
     * Remember a caller-supplied selection for a preview actor and apply its matching components.
     *
     * @param Actor VariationViewActor or MenuArmor instance; templates are rejected.
     * @param Set Variation data supplied by the editor tool or customization manager.
     * @param Index Explicit zero-based variation index.
     * @return True when at least one matching component received the variation.
     * @note Selection is held weakly for this editor session, not added to the reflected asset layout.
     */
    bool BindPreview(AActor* Actor, UObject* Set, int32 Index);

    /** Reapply a preview's caller-supplied selection after its components or dependencies change. */
    bool RefreshPreview(FSuzieFunctionContext& Context);
}
