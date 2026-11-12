/**
 * @file SBZVariationEditActor.cpp
 * @brief Preserves variation editor component wiring for newly created or replaced previews.
 */

#include "SBZVariationNative.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"

namespace SuzieVariationEditActor
{
    /** Fill only missing component links, preserving the JMAP's existing or explicitly assigned wiring. */
    static void Wire(FSuzieFunctionContext& Context)
    {
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }
        Context.ClearDependencies();
        UObject* Component = Context.GetProperty<UObject*>("VariationComponent");
        if (IsValid(Component)) { Context.WatchDependency(Component); }
        if (!SuzieVariation::Ready(Component) || Component->IsTemplate()) { return; }

        bool bChanged = false;
        if (!Context.Property("SkeletalMeshComponent", Component).Get<USkeletalMeshComponent*>())
        {
            auto* Mesh = Context.GetProperty<USkeletalMeshComponent*>("SkeletalMeshComponent");
            if (SuzieVariation::Ready(Mesh)) { bChanged |= Context.Property("SkeletalMeshComponent", Component).Set<USkeletalMeshComponent*>(Mesh); }
        }
        if (!Context.Property("StaticMeshComponent", Component).Get<UStaticMeshComponent*>())
        {
            auto* Mesh = Context.GetProperty<UStaticMeshComponent*>("StaticMeshComponent");
            if (SuzieVariation::Ready(Mesh)) { bChanged |= Context.Property("StaticMeshComponent", Component).Set<UStaticMeshComponent*>(Mesh); }
        }
        if (Context.IsValid() && bChanged)
        {
            FSuzieFunctionContext Linked(Component);
            Linked.RequestRefresh();
        }
    }
}

/** Preserve existing JMAP wiring and repair missing links after initialization. */
SUZIE_POST_INITIALIZE(SBZVariationEditActor)
{
    SuzieVariationEditActor::Wire(Context);
}

/** Wire a newly assigned variation editor component. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditActor, VariationComponent)
{
    SuzieVariationEditActor::Wire(Context);
}

/** Fill missing links after the actor's skeletal component changes. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditActor, SkeletalMeshComponent)
{
    SuzieVariationEditActor::Wire(Context);
}

/** Fill missing links after the actor's static component changes. */
SUZIE_PROPERTY_CHANGED(SBZVariationEditActor, StaticMeshComponent)
{
    SuzieVariationEditActor::Wire(Context);
}

/** Repair missing links after a watched component becomes available or is replaced. */
SUZIE_REFRESH(SBZVariationEditActor)
{
    SuzieVariationEditActor::Wire(Context);
}
