#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "CrimeFactorySettings.generated.h"

UENUM()
enum class ECrimeFactoryArchiveUtility : uint8
{
    WinRAR = 0 UMETA(DisplayName = "WinRAR"),
    SevenZip = 1 UMETA(DisplayName = "7-Zip"),
};

UENUM()
enum class ECrimeFactoryStartGameType : uint8 {
    NONE = 0 UMETA(DisplayName = "Do not launch"),
    STEAM = 1 UMETA(DisplayName = "Steam"),
    MICROSOFT_STORE = 2 UMETA(DisplayName = "Microsoft Store / Xbox"),
    EPIC_GAMES = 3 UMETA(DisplayName = "Epic Games Store"),
};

/**
 * Per-user CrimeFactory packaging preferences.
 *
 * Registered under Project Settings -> CrimeFactory -> General, alongside Asset Palette.
 */
UCLASS(config = EditorPerProjectUserSettings)
class CRIMEFACTORY_API UCrimeFactorySettings : public UObject
{
    GENERATED_BODY()

public:
    UCrimeFactorySettings();
    static UCrimeFactorySettings* Get();
    void SaveSettings();
    static void OnCookedEditorSettingsChanged(UObject* Object, FPropertyChangedEvent& Event);

    virtual bool CanEditChange(const FProperty* Property) const override;

    /* General ------------------------------------------------------------------------------ */

    UPROPERTY(EditAnywhere, Config, Category = "General|PAYDAY 3", meta = (DisplayName = "PAYDAY 3 Game Path", ToolTip = "Uses the PAYDAY 3 Cooked Editor installation directory when configured. Otherwise, choose a game directory here."))
    FDirectoryPath PD3GamePath;

    UPROPERTY(EditAnywhere, Config, Category = "General|PAYDAY 3", meta = (DisplayName = "Copy Mods To Game"))
    bool bCopyModsToGame = false;

    UPROPERTY(EditAnywhere, Config, Category = "General|PAYDAY 3", meta = (DisplayName = "Launch Game After Packing"))
    ECrimeFactoryStartGameType LaunchGameAfterPacking = ECrimeFactoryStartGameType::NONE;

    UPROPERTY(EditAnywhere, Config, Category = "General|Export", meta = (DisplayName = "Additional ZIP Export Folder", ToolTip = "Optional extra ZIP destination, with a subfolder per platform. The standard Saved/ArchivedPlugins archive is always retained. Relative paths are resolved from the project directory."))
    FDirectoryPath ZipExportDirectory;

    UPROPERTY(EditAnywhere, Config, Category = "Archiving", meta = (DisplayName = "Create RAR / 7z Archive", ToolTip = "Create an additional distributable archive beside the standard ZIP in Saved/ArchivedPlugins. Requires only the selected utility."))
    bool bCreateUtilityArchive = false;

    UPROPERTY(EditAnywhere, Config, Category = "Archiving", meta = (DisplayName = "Archive Utility"))
    ECrimeFactoryArchiveUtility ArchiveUtility = ECrimeFactoryArchiveUtility::WinRAR;

    UPROPERTY(EditAnywhere, Config, Category = "Archiving", meta = (DisplayName = "WinRAR Executable Path", FilePathFilter = "exe"))
    FFilePath WinRARExecutablePath;

    UPROPERTY(EditAnywhere, Config, Category = "Archiving", meta = (DisplayName = "7-Zip Executable Path", FilePathFilter = "exe"))
    FFilePath SevenZipExecutablePath;

    /* Internal state ----------------------------------------------------------------------- */

    UPROPERTY(Config)
    TMap<FString, bool> ModSelection;

    UPROPERTY(Config)
    bool bMigratedLegacyCrimeFactoryGameSettings = false;

private:
    bool UpdateGamePathFromCookedEditor();
};
