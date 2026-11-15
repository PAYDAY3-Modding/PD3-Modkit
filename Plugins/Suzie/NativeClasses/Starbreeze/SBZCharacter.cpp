/**
 * @file SBZCharacter.cpp
 * @brief Connects inherited character seeds to the reflected AI appearance path.
 *
 * Character selection uses the saved effective seed. Editor refreshes do not invoke
 * the mission's order-dependent spawn allocator or change shared gameplay defaults.
 */

#include "SBZVariationNative.h"
#include "GameFramework/Actor.h"

/** Return the reflected seed without changing its value or random state. */
SUZIE_FUNCTION(SBZCharacter, GetSeed)
{
    Context.SetReturn<int32>(Context.GetProperty<int32>("Seed"));
}

/** Apply the inherited effective seed after a character seed rep-notify. */
SUZIE_FUNCTION(SBZCharacter, OnRep_Seed)
{
    SuzieVariation::ApplyCharacterSeed(Context);
}

/** Keep manual seeds, including negative values, and refresh the AI appearance. */
SUZIE_PROPERTY_CHANGED(SBZCharacter, Seed)
{
    SuzieVariation::PreserveCharacterSeed(Cast<AActor>(Context.GetSelf()));
    SuzieVariation::ApplyCharacterSeed(Context);
}
