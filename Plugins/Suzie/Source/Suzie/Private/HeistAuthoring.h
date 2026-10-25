#pragma once

#include "CoreMinimal.h"

class AActor;
class UObject;

namespace SuzieHeistAuthoring
{
    // Configuration only: never copy tick registration, Target or prerequisites.
    void CopyActorTickConfiguration(AActor& Destination, const AActor& Source);

    // Positive-weight squads in the first progression entry, per static difficulty slot.
    TArray<TArray<int32>> GetOpeningSquadSizes(const UObject& Settings);

    // Editor authoring only. Existing custom definitions are never adopted implicitly.
    bool UpdateSpawnAuthoringData(AActor& Actor, bool bFollowGeneratedTransform);
    void RegisterEditorHooks();
    void UnregisterEditorHooks();
}
