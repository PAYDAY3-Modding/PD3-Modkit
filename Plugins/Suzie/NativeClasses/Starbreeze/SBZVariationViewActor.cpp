/**
 * @file SBZVariationViewActor.cpp
 * @brief Refreshes variation viewers using the selection supplied to ApplyVariation.
 *
 * The JMAP declares no set or index on this actor. The utility remembers a weak selection
 * for this editor session; tools must supply it again after reopening their preview.
 */

#include "SBZVariationNative.h"

/** Restore a supplied selection after the viewer's required loading is ready. */
SUZIE_POST_INITIALIZE(SBZVariationViewActor)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Apply the supplied selection when the skeletal preview component changes. */
SUZIE_PROPERTY_CHANGED(SBZVariationViewActor, SkeletalMeshComponent)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Apply the supplied selection when the static preview component changes. */
SUZIE_PROPERTY_CHANGED(SBZVariationViewActor, StaticMeshComponent)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Refresh the supplied selection after a watched dependency changes. */
SUZIE_REFRESH(SBZVariationViewActor)
{
    SuzieVariation::RefreshPreview(Context);
}
