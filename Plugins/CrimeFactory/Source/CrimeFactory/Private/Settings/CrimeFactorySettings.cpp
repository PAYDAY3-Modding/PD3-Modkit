#include "Settings/CrimeFactorySettings.h"

#include "Misc/ConfigCacheIni.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#if __has_include("PD3CookedEditorSettings.h")
#include "PD3CookedEditorSettings.h"
#define CRIMEFACTORY_HAS_COOKED_EDITOR_SETTINGS 1
#else
#define CRIMEFACTORY_HAS_COOKED_EDITOR_SETTINGS 0
#endif

namespace
{
    constexpr const TCHAR* UserConfigSection = TEXT("CrimeFactory.PD3");
    constexpr const TCHAR* SettingsConfigSection = TEXT("/Script/CrimeFactory.CrimeFactorySettings");
    // Read-only migration sources. All subsequent writes use CrimeFactory sections.
    constexpr const TCHAR* LegacyUserConfigSection = TEXT("CrimeForge.PD3");
    constexpr const TCHAR* LegacySettingsConfigSection = TEXT("/Script/CrimeForge.CrimeForgeSettings");

    FString GetCookedEditorGamePath()
    {
#if CRIMEFACTORY_HAS_COOKED_EDITOR_SETTINGS
        return GetDefault<UPD3CookedEditorSettings>()->GameInstallDirectory.Path.TrimStartAndEnd();
#else
        return {};
#endif
    }

    bool ImportSetting(UCrimeFactorySettings* Settings, const TCHAR* Section,
        const TCHAR* Name, const FString& Filename)
    {
        FString Value;
        FProperty* Property = FindFProperty<FProperty>(Settings->GetClass(), Name);
        return Property && GConfig->GetString(Section, Name, Value, Filename)
            && Property->ImportText_InContainer(*Value, Settings, Settings, PPF_None) != nullptr;
    }
}

UCrimeFactorySettings::UCrimeFactorySettings()
{
    // Config loading follows construction, so saved paths always override detection.
    auto DetectExecutable = [](const TCHAR* RelativePath, const TCHAR* DefaultPath)
    {
        for (const TCHAR* Variable : {TEXT("ProgramW6432"), TEXT("ProgramFiles"), TEXT("ProgramFiles(x86)")})
        {
            const FString Root = FPlatformMisc::GetEnvironmentVariable(Variable);
            if (Root.IsEmpty()) continue;
            const FString Candidate = FPaths::Combine(Root, RelativePath);
            if (IFileManager::Get().FileExists(*Candidate)) return Candidate;
        }
        return FString(DefaultPath);
    };
    WinRARExecutablePath.FilePath = DetectExecutable(TEXT("WinRAR/WinRAR.exe"), TEXT("C:/Program Files/WinRAR/WinRAR.exe"));
    SevenZipExecutablePath.FilePath = DetectExecutable(TEXT("7-Zip/7z.exe"), TEXT("C:/Program Files/7-Zip/7z.exe"));
}

UCrimeFactorySettings* UCrimeFactorySettings::Get()
{
    UCrimeFactorySettings* Settings = GetMutableDefault<UCrimeFactorySettings>();
    static bool bLoadedUserSettings = false;
    bool bSaveSettings = false;
    if (!bLoadedUserSettings && GConfig)
    {
        bLoadedUserSettings = true;
        const bool bHasCurrentSettings = GConfig->DoesSectionExist(SettingsConfigSection, GEditorPerProjectIni);
        if (!bHasCurrentSettings)
        {
            for (const TCHAR* Name : {TEXT("PD3GamePath"), TEXT("bCopyModsToGame"),
                TEXT("LaunchGameAfterPacking"), TEXT("ModSelection")})
            {
                ImportSetting(Settings, LegacySettingsConfigSection, Name, GEditorPerProjectIni);
            }
        }

        const TCHAR* CustomSection = GConfig->DoesSectionExist(UserConfigSection, GEditorPerProjectIni)
            ? UserConfigSection : (!bHasCurrentSettings ? LegacyUserConfigSection : nullptr);
        if (CustomSection)
        {
            FString SavedGamePath;
            if (GConfig->GetString(CustomSection, TEXT("PD3GamePath"), SavedGamePath, GEditorPerProjectIni))
            {
                Settings->PD3GamePath.Path = SavedGamePath;
            }
            GConfig->GetBool(CustomSection, TEXT("CopyModsToGame"), Settings->bCopyModsToGame, GEditorPerProjectIni);
            int32 LaunchType = static_cast<int32>(Settings->LaunchGameAfterPacking);
            if (GConfig->GetInt(CustomSection, TEXT("LaunchGameAfterPacking"), LaunchType, GEditorPerProjectIni)
                && LaunchType >= 0 && LaunchType <= static_cast<int32>(ECrimeFactoryStartGameType::EPIC_GAMES))
            {
                Settings->LaunchGameAfterPacking = static_cast<ECrimeFactoryStartGameType>(LaunchType);
            }
        }

        if (!Settings->bMigratedLegacyCrimeFactoryGameSettings)
        {
            // Very old versions stored mod selections in Game.ini. Prefer per-user data.
            FString ExistingSelections;
            if (!bHasCurrentSettings && !GConfig->GetString(LegacySettingsConfigSection,
                TEXT("ModSelection"), ExistingSelections, GEditorPerProjectIni))
            {
                if (!ImportSetting(Settings, SettingsConfigSection, TEXT("ModSelection"), GGameIni))
                {
                    ImportSetting(Settings, LegacySettingsConfigSection, TEXT("ModSelection"), GGameIni);
                }
            }
            Settings->bMigratedLegacyCrimeFactoryGameSettings = true;
            bSaveSettings = true;
        }
    }

    // Use the live settings object, independently of retail-mount caching or the AES key.
    bSaveSettings |= Settings->UpdateGamePathFromCookedEditor();
    if (bSaveSettings && GConfig) Settings->SaveSettings();
    return Settings;
}

bool UCrimeFactorySettings::UpdateGamePathFromCookedEditor()
{
    const FString GamePath = GetCookedEditorGamePath();
    if (GamePath.IsEmpty() || PD3GamePath.Path == GamePath) return false;
    PD3GamePath.Path = GamePath;
    return true;
}

void UCrimeFactorySettings::OnCookedEditorSettingsChanged(UObject* Object, FPropertyChangedEvent& Event)
{
#if CRIMEFACTORY_HAS_COOKED_EDITOR_SETTINGS
    if (Object && Object->IsA<UPD3CookedEditorSettings>()
        && (!Event.MemberProperty || Event.MemberProperty->GetFName()
            == GET_MEMBER_NAME_CHECKED(UPD3CookedEditorSettings, GameInstallDirectory)))
    {
        Get();
    }
#endif
}

bool UCrimeFactorySettings::CanEditChange(const FProperty* Property) const
{
    if (Property && Property->GetFName() == GET_MEMBER_NAME_CHECKED(UCrimeFactorySettings, PD3GamePath)
        && !GetCookedEditorGamePath().IsEmpty()) return false;
    return Super::CanEditChange(Property);
}

void UCrimeFactorySettings::SaveSettings()
{
    UpdateGamePathFromCookedEditor();
    SaveConfig();
    if (GConfig)
    {
        GConfig->SetString(UserConfigSection, TEXT("PD3GamePath"), *PD3GamePath.Path, GEditorPerProjectIni);
        GConfig->SetBool(UserConfigSection, TEXT("CopyModsToGame"), bCopyModsToGame, GEditorPerProjectIni);
        GConfig->SetInt(UserConfigSection, TEXT("LaunchGameAfterPacking"),
            static_cast<int32>(LaunchGameAfterPacking), GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrimeFactoryArchiveSettingsTest,
    "CrimeFactory.Archiving.SettingsPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCrimeFactoryArchiveSettingsTest::RunTest(const FString& Parameters)
{
    // Use a private config file; never modify the editor user's preferences.
    const FString Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
        / TEXT("CrimeFactory") / (TEXT("ArchiveSettings_") + FGuid::NewGuid().ToString() + TEXT(".ini")));
    // Unlike the registered user ini, a new private filename must be added first.
    GConfig->Add(Filename, FConfigFile());
    UCrimeFactorySettings* Settings = NewObject<UCrimeFactorySettings>();
    Settings->WinRARExecutablePath.FilePath = TEXT("C:/Custom WinRAR/WinRAR.exe");
    Settings->SevenZipExecutablePath.FilePath = TEXT("D:/Custom 7-Zip/7z.exe");
    Settings->bCreateUtilityArchive = true;
    Settings->ArchiveUtility = ECrimeFactoryArchiveUtility::SevenZip;
    Settings->SaveConfig(CPF_Config, *Filename, GConfig, false);
    GConfig->UnloadFile(Filename);
    GConfig->LoadFile(Filename);

    UCrimeFactorySettings* Reloaded = NewObject<UCrimeFactorySettings>();
    Reloaded->LoadConfig(UCrimeFactorySettings::StaticClass(), *Filename);
    TestEqual(TEXT("7-Zip selection reloads"), Reloaded->ArchiveUtility, ECrimeFactoryArchiveUtility::SevenZip);
    TestTrue(TEXT("Archive opt-in reloads"), Reloaded->bCreateUtilityArchive);
    TestEqual(TEXT("Custom WinRAR path reloads"), Reloaded->WinRARExecutablePath.FilePath, Settings->WinRARExecutablePath.FilePath);
    TestEqual(TEXT("Custom 7-Zip path reloads"), Reloaded->SevenZipExecutablePath.FilePath, Settings->SevenZipExecutablePath.FilePath);

    Reloaded->ArchiveUtility = ECrimeFactoryArchiveUtility::WinRAR;
    Reloaded->SaveConfig(CPF_Config, *Filename, GConfig, false);
    GConfig->UnloadFile(Filename);
    GConfig->LoadFile(Filename);
    // A fresh object models editor restart: config omits values equal to defaults.
    UCrimeFactorySettings* Switched = NewObject<UCrimeFactorySettings>();
    Switched->LoadConfig(UCrimeFactorySettings::StaticClass(), *Filename);
    TestEqual(TEXT("Switch back to WinRAR reloads"), Switched->ArchiveUtility, ECrimeFactoryArchiveUtility::WinRAR);
    TestEqual(TEXT("Switch retains 7-Zip path"), Switched->SevenZipExecutablePath.FilePath, Reloaded->SevenZipExecutablePath.FilePath);
    GConfig->UnloadFile(Filename);
    IFileManager::Get().Delete(*Filename);
    return true;
}
#endif
