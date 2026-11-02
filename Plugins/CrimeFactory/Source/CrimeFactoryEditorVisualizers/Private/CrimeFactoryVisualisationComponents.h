#pragma once

#include "Components/PrimitiveComponent.h"

namespace CrimeFactoryVisualisation
{
    inline constexpr EObjectFlags PreviewFlags = RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;

    inline bool IsManaged(const UPrimitiveComponent* Component, FName Tag)
    {
        return IsValid(Component) && Component->HasAllFlags(PreviewFlags)
            && Component->IsVisualizationComponent() && Component->ComponentHasTag(Tag);
    }

    inline void Configure(UPrimitiveComponent& Component, FName Tag)
    {
        Component.SetIsVisualizationComponent(true);
        // Instance provenance prevents construction-script fixup from adopting a preview as actor root.
        // Never add previews to the serialized InstanceComponents array.
        Component.CreationMethod = EComponentCreationMethod::Instance;
        Component.ComponentTags.AddUnique(Tag);
        Component.SetHiddenInGame(true);
        Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component.SetGenerateOverlapEvents(false);
        Component.SetCanEverAffectNavigation(false);
        Component.PrimaryComponentTick.bCanEverTick = false;
        Component.PrimaryComponentTick.bStartWithTickEnabled = false;
        Component.AlwaysLoadOnClient = false;
        Component.AlwaysLoadOnServer = false;
    }

    inline void Destroy(UPrimitiveComponent& Component)
    {
        Component.DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
        Component.DestroyComponent();
    }
}
