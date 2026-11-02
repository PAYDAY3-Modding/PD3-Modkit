#include "CrimeFactoryBillboardComponent.h"

#include "Components/LightComponent.h"
#include "Engine/Engine.h"
#include "Engine/Light.h"
#include "Engine/Texture2D.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "TextureResource.h"

namespace
{
    // Keep the engine's billboard sizing, category visibility, selection and hit-proxy behaviour.
    // Only the drawing anchor differs: each view raises the sprite by its rendered half-height.
    class FCrimeFactoryBillboardSceneProxy final : public FPrimitiveSceneProxy
    {
    public:
        explicit FCrimeFactoryBillboardSceneProxy(const UCrimeFactoryBillboardComponent& Component)
            : FPrimitiveSceneProxy(&Component)
            , Texture(Component.Sprite)
            , ScreenSize(Component.ScreenSize)
            , U(Component.U), V(Component.V), UL(Component.UL), VL(Component.VL)
            , OpacityMask(Component.OpacityMaskRefVal)
            , bScreenScaled(Component.bIsScreenSizeScaled)
            , EditorScale(Component.bUseInEditorScaling ? UBillboardComponent::EditorScale : 1.0f)
            , Category(GEngine->GetSpriteCategoryIndex(Component.SpriteInfo.Category))
        {
            bWillEverBeLit = false;
            if (AActor* Owner = Component.GetOwner())
            {
                SpriteScale = Owner->SpriteScale;
                bActorLocked = Component.bShowLockedLocation && Owner->IsLockLocation();
                if (ALight* Light = Cast<ALight>(Owner))
                {
                    if (ULightComponent* LightComponent = Light->GetLightComponent())
                    {
                        Color = LightComponent->LightColor.ReinterpretAsLinear();
                    }
                }
            }
        }

        virtual SIZE_T GetTypeHash() const override
        {
            static uint8 Identity;
            return reinterpret_cast<SIZE_T>(&Identity);
        }

        virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
            const FSceneViewFamily&, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
        {
            const FTexture* Resource = Texture ? Texture->GetResource() : nullptr;
            if (!Resource) { return; }
            const float Width = UL == 0.0f ? Resource->GetSizeX() : UL;
            const float Height = VL == 0.0f ? Resource->GetSizeY() : VL;
            const FVector Pivot = GetLocalToWorld().GetOrigin();
            const float Scale = GetLocalToWorld().GetMaximumAxisScale() * SpriteScale * EditorScale * 0.25f;
            for (int32 Index = 0; Index < Views.Num(); ++Index)
            {
                if (!(VisibilityMap & (1u << Index))) { continue; }
                const FSceneView& View = *Views[Index];
                float ViewScale = Scale;
                const FMatrix& Projection = View.ViewMatrices.GetProjectionMatrix();
                if (bScreenScaled && Projection.M[3][3] != 1.0f)
                {
                    const float Zoom = FMath::Min<float>(Projection.M[0][0], Projection.M[1][1]);
                    if (Zoom != 0.0f)
                    {
                        ViewScale *= FMath::Min(1.0f, static_cast<float>(View.WorldToScreen(Pivot).W) * ScreenSize / Zoom);
                    }
                }
                FLinearColor DrawColor = Color;
                if (View.bHasSelectedComponents && !IsIndividuallySelected())
                {
                    DrawColor = FLinearColor::White + GEngine->GetSubduedSelectionOutlineColor()
                        * GEngine->SelectionHighlightIntensityBillboards * 10;
                }
                else if (IsSelected())
                {
                    DrawColor = FLinearColor::White + GEngine->GetSelectedMaterialColor()
                        * GEngine->SelectionHighlightIntensityBillboards * 10;
                }
                else if (IsHovered())
                {
                    DrawColor = FLinearColor::White + GEngine->GetHoveredMaterialColor()
                        * GEngine->SelectionHighlightIntensityBillboards * 10;
                }
                if (bActorLocked) { DrawColor = FColor::Red; }
                DrawColor.A = 1.0f;
                const FLinearColor ColorationTint = IsSelected() ? DrawColor : FLinearColor(GetPrimitiveColor());
                if (View.Family->EngineShowFlags.ActorColoration) { DrawColor = ColorationTint; }

                const float HalfHeight = ViewScale * Height;
                const FVector Centre = Pivot + View.GetViewUp() * HalfHeight;
                Collector.GetPDI(Index)->DrawSprite(Centre, ViewScale * Width, HalfHeight, Resource,
                    DrawColor, GetDepthPriorityGroup(&View), U, Width, V, Height, SE_BLEND_Masked, OpacityMask);
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
                RenderBounds(Collector.GetPDI(Index), View.Family->EngineShowFlags, GetBounds(), IsSelected());
#endif
            }
        }

        virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
        {
            bool bVisible = View->Family->EngineShowFlags.BillboardSprites;
            if (bVisible && Category != INDEX_NONE && View->SpriteCategoryVisibility.IsValidIndex(Category))
            {
                bVisible = View->SpriteCategoryVisibility[Category];
            }
            FPrimitiveViewRelevance Result;
            Result.bDrawRelevance = IsShown(View) && bVisible;
            Result.bOpaque = true;
            Result.bDynamicRelevance = true;
            Result.bShadowRelevance = IsShadowCast(View);
            Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
            return Result;
        }

        virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

    private:
        const UTexture2D* Texture;
        float ScreenSize, U, V, UL, VL, OpacityMask;
        bool bScreenScaled;
        float EditorScale;
        int32 Category;
        float SpriteScale = 1.0f;
        bool bActorLocked = false;
        FLinearColor Color = FLinearColor::White;
    };
}

FPrimitiveSceneProxy* UCrimeFactoryBillboardComponent::CreateSceneProxy()
{
    return new FCrimeFactoryBillboardSceneProxy(*this);
}

FBoxSphereBounds UCrimeFactoryBillboardComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const float Width = UL == 0.0f ? (Sprite ? Sprite->GetSizeX() : 1) : UL;
    const float Height = VL == 0.0f ? (Sprite ? Sprite->GetSizeY() : 1) : VL;
    const float OwnerScale = GetOwner() ? GetOwner()->SpriteScale : 1.0f;
    const float Scale = FMath::Abs(OwnerScale * (bUseInEditorScaling ? EditorScale : 1.0f))
        * LocalToWorld.GetMaximumAxisScale() * 0.25f;
    // The camera-facing sprite can extend two half-heights from its pivot in any viewing direction.
    const double Radius = Scale * FMath::Sqrt(Width * Width + 4.0f * Height * Height);
    return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(Radius), Radius);
}
