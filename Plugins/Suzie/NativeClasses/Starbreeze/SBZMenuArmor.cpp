/**
 * @file SBZMenuArmor.cpp
 * @brief Refreshes menu armor components from their customization manager's supplied selection.
 */

#include "SBZVariationNative.h"

/** Apply a supplied armor selection after component initialization. */
SUZIE_POST_INITIALIZE(SBZMenuArmor)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Reapply armor data when the skeletal preview component is replaced. */
SUZIE_PROPERTY_CHANGED(SBZMenuArmor, SkeletalMeshComponent)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Reapply armor data when the static preview component is replaced. */
SUZIE_PROPERTY_CHANGED(SBZMenuArmor, StaticMeshComponent)
{
    SuzieVariation::RefreshPreview(Context);
}

/** Refresh armor CPD and materials after a watched dependency changes. */
SUZIE_REFRESH(SBZMenuArmor)
{
    SuzieVariation::RefreshPreview(Context);
}
