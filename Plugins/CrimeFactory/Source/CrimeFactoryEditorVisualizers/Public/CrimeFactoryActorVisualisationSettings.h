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

USTRUCT()
struct CRIMEFACTORYEDITORVISUALIZERS_API FCrimeFactoryPawnSpawnerDefinition
{
    GENERATED_BODY()

    /** Matches this actor class and its descendants. A closer configured class overrides it. */
    UPROPERTY(EditAnywhere, Category = "Pawn Spawn Visualisation", meta = (AllowAbstract = "true"))
    TSoftClassPtr<AActor> ActorClass;

    /** Selected from supported reflected properties on Actor Class. */
    UPROPERTY(EditAnywhere, Category = "Pawn Spawn Visualisation", meta = (GetOptions = "GetPawnRequestPropertyOptions"))
    FName PawnRequestProperty = TEXT("PawnRequest");
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

    /** Mesh-only, reference-pose previews. No pawn actors or gameplay systems are created. */
    UPROPERTY(EditAnywhere, Config, Category = "Pawn Spawn Visualisation")
    bool bEnablePawnPreviews = true;

    UPROPERTY(EditAnywhere, Config, Category = "Pawn Spawn Visualisation", meta = (ClampMin = "0.01", UIMin = "0.01"))
    float PawnPreviewScale = 1.0f;

    /** Choose a spawner class and request property. Previews use the first pawn/spawn definition and the default appearance category's first eligible variant, in reference pose. Runtime equipment and animation are not executed. */
    UPROPERTY(EditAnywhere, Config, Category = "Pawn Spawn Visualisation", meta = (TitleProperty = "ActorClass"))
    TArray<FCrimeFactoryPawnSpawnerDefinition> PawnSpawnerDefinitions;

    UFUNCTION()
    TArray<FString> GetPawnRequestPropertyOptions() const;

    virtual bool SupportsAutoRegistration() const override { return false; }
    virtual FName GetContainerName() const override { return TEXT("Project"); }
    virtual FName GetCategoryName() const override { return TEXT("CrimeFactory"); }
    virtual FName GetSectionName() const override { return TEXT("ActorVisualisation"); }
};
