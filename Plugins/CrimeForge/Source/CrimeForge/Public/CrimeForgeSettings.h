#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "CrimeForgeSettings.generated.h"

UENUM()
enum class ECrimeForgeStartGameType : uint8 {
    NONE UMETA(DisplayName = "Do not launch"),
    STEAM UMETA(DisplayName = "Steam"),
};

UCLASS(config=Game)
class CRIMEFORGE_API UCrimeForgeSettings : public UObject {
    GENERATED_BODY()
public:
    /** Retrieves global instance of crimeforge settings */
    static UCrimeForgeSettings* Get();

    /** Saves crimeforge settings to configuration file */
    void SaveSettings();

    UPROPERTY(EditAnywhere, config, Category = Config)
    FDirectoryPath PD3GamePath;

    UPROPERTY(EditAnywhere, config, Category = Config)
    ECrimeForgeStartGameType LaunchGameAfterPacking;

    UPROPERTY(EditAnywhere, config, Category = Config)
    bool bCopyModsToGame = false;

    UPROPERTY(BlueprintReadOnly, config, Category = Config)
    TMap<FString, bool> ModSelection;
};
