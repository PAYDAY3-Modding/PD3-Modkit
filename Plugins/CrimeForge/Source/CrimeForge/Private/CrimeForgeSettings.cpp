#include "CrimeForgeSettings.h"

#include "Misc/ConfigCacheIni.h"

namespace
{
	static const TCHAR* CrimeForgeUserConfigSection = TEXT("CrimeForge.PD3");

	static const TCHAR* PD3GamePathConfigKey = TEXT("PD3GamePath");
	static const TCHAR* CopyModsToGameConfigKey = TEXT("CopyModsToGame");
	static const TCHAR* LaunchGameAfterPackingConfigKey = TEXT("LaunchGameAfterPacking");
}

UCrimeForgeSettings* UCrimeForgeSettings::Get()
{
	UCrimeForgeSettings* Settings = GetMutableDefault<UCrimeForgeSettings>();

	/*
	 * Persist these CrimeForge user settings explicitly in GEditorPerProjectIni.
	 *
	 * This avoids relying on the individual properties being declared
	 * UPROPERTY(Config) in CrimeForgeSettings.h and keeps them per-user/per-project.
	 */
	static bool bLoadedUserSettings = false;
	if (!bLoadedUserSettings)
	{
		bLoadedUserSettings = true;

		if (GConfig)
		{
			FString SavedGamePath;
			if (GConfig->GetString(
				CrimeForgeUserConfigSection,
				PD3GamePathConfigKey,
				SavedGamePath,
				GEditorPerProjectIni))
			{
				Settings->PD3GamePath.Path = SavedGamePath;
			}

			bool bSavedCopyModsToGame = Settings->bCopyModsToGame;
			if (GConfig->GetBool(
				CrimeForgeUserConfigSection,
				CopyModsToGameConfigKey,
				bSavedCopyModsToGame,
				GEditorPerProjectIni))
			{
				Settings->bCopyModsToGame = bSavedCopyModsToGame;
			}

			int32 SavedLaunchGameAfterPacking =
				static_cast<int32>(Settings->LaunchGameAfterPacking);

			if (GConfig->GetInt(
				CrimeForgeUserConfigSection,
				LaunchGameAfterPackingConfigKey,
				SavedLaunchGameAfterPacking,
				GEditorPerProjectIni))
			{
				Settings->LaunchGameAfterPacking =
					static_cast<ECrimeForgeStartGameType>(
						SavedLaunchGameAfterPacking
					);
			}
		}
	}

	return Settings;
}

void UCrimeForgeSettings::SaveSettings()
{
	// Preserve the existing behaviour for any normal Config properties.
	SaveConfig();

	if (GConfig)
	{
		GConfig->SetString(
			CrimeForgeUserConfigSection,
			PD3GamePathConfigKey,
			*PD3GamePath.Path,
			GEditorPerProjectIni);

		GConfig->SetBool(
			CrimeForgeUserConfigSection,
			CopyModsToGameConfigKey,
			bCopyModsToGame,
			GEditorPerProjectIni);

		GConfig->SetInt(
			CrimeForgeUserConfigSection,
			LaunchGameAfterPackingConfigKey,
			static_cast<int32>(LaunchGameAfterPacking),
			GEditorPerProjectIni);

		GConfig->Flush(false, GEditorPerProjectIni);
	}
}
