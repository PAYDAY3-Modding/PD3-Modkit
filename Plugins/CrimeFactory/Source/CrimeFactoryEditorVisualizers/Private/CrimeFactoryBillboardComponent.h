#pragma once

#include "Components/BillboardComponent.h"
#include "CrimeFactoryBillboardComponent.generated.h"

/** Editor preview whose sprite's bottom centre is anchored at the component origin. */
UCLASS(Transient, NotBlueprintable, HideDropdown)
class UCrimeFactoryBillboardComponent : public UBillboardComponent
{
    GENERATED_BODY()

public:
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};
