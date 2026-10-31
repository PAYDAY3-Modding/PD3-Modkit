#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "CrimeFactoryActorVisualisationSettings.generated.h"

USTRUCT()
struct CRIMEFACTORYEDITORVISUALIZERS_API FCrimeFactoryActorBillboardDefinition
{
    GENERATED_BODY()

    /** Matches this class and its descendants, unless a closer ancestor is configured. */
    UPROPERTY(EditAnywhere, Category = "Actor Visualisation", meta = (AllowAbstract = "true"))
    TSoftClassPtr<AActor> ActorClass;

    /** Editor preview only; this reference does not add the texture to forged mods. */
    UPROPERTY(EditAnywhere, Category = "Actor Visualisation", meta = (DisplayThumbnail = "true"))
    TSoftObjectPtr<UTexture2D> BillboardTexture;
};

/** Shared project configuration, independent of CrimeFactory's per-user forging preferences. */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Actor Visualisation"))
class CRIMEFACTORYEDITORVISUALIZERS_API UCrimeFactoryActorVisualisationSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Config, Category = "Actor Visualisation", meta = (ClampMin = "0.01", UIMin = "0.01"))
    float BillboardScale = 1.0f;

    UPROPERTY(EditAnywhere, Config, Category = "Actor Visualisation", meta = (TitleProperty = "ActorClass"))
    TArray<FCrimeFactoryActorBillboardDefinition> ActorBillboardDefinitions;

    virtual bool SupportsAutoRegistration() const override { return false; }
    virtual FName GetContainerName() const override { return TEXT("Project"); }
    virtual FName GetCategoryName() const override { return TEXT("CrimeFactory"); }
    virtual FName GetSectionName() const override { return TEXT("ActorVisualisation"); }
};
