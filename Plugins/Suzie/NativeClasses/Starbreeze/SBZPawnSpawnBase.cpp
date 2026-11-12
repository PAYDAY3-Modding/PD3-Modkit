/**
 * @file SBZPawnSpawnBase.cpp
 * @brief Preserves authored pawn-spawner seeds through editor placement events.
 *
 * Seed is declared on SBZPawnSpawnBase and inherited by SBZPawnSpawnPredefined.
 * The existing visualizer handles property edits, undo, loading and dependency
 * refreshes. Retail spawn requests and mission random state remain untouched.
 */

#include "SBZVariationNative.h"
#include "GameFramework/Actor.h"

/** Keep a Details-panel seed edit when the matching placement notification follows. */
SUZIE_PROPERTY_CHANGED(SBZPawnSpawnBase, Seed)
{
    SuzieVariation::PreserveSpawnerSeed(Cast<AActor>(Context.GetSelf()));
}
