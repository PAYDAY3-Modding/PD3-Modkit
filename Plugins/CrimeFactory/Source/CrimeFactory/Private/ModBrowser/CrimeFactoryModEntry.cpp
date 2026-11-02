#include "ModBrowser/CrimeFactoryModEntry.h"
#include "Packaging/CrimeFactoryPALCommandlet.h"
#include "Editor/CrimeFactory.h"
#include "Settings/CrimeFactorySettings.h"
#include "Editor/CrimeFactoryStyle.h"
#include "Packaging/CrimeFactoryPackagingUI.h"
#include "Packaging/CrimeFactoryGameProcess.h"
#include "Metadata/CrimeFactoryMetadataSync.h"
#include "Metadata/CrimeFactoryModIcon.h"
#include "Information/SCrimeFactoryModInspector.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#include "Metadata/ModMetadataObject.h"
#include "SourceControlOperations.h"
#include "Async/Async.h"
#include "IUATHelperModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "CollectionManagerModule.h"
#include "ICollectionManager.h"
#include "PD3PrimaryAssetLabelValidation.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Templates/Atomic.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManagerSettings.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SBox.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryModListEntry"

static TAtomic<bool> GCrimeFactoryForgeActive(false);

struct FCrimeFactoryForgeBatch
{
    TSet<const SCrimeFactoryModEntry*> ApprovedEntries;
    TArray<TSharedPtr<const SCrimeFactoryModEntry>> Entries;
    FString ZipExportDirectory;
    TAtomic<bool> bFinished{false};
    void Finish()
    {
        if (!bFinished.Exchange(true)) GCrimeFactoryForgeActive.Store(false);
    }
    ~FCrimeFactoryForgeBatch() { Finish(); }
};

namespace
{
	struct FCrimeFactoryUATResult
	{
		bool bLoaded = false;
		bool bSuccess = false;

		FString PluginName;
		FString Phase;
		FString Mode;
		FString ChunkId;
		FString Message;
		FString ExceptionType;
		FString Details;
		FString LogFolder;
		FString LogFile;
		FString DurationSeconds;
	};

	FString GetJsonString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName)
	{
		FString Value;

		if (Object.IsValid())
		{
			Object->TryGetStringField(
				FieldName,
				Value);
		}

		return Value;
	}

	FCrimeFactoryUATResult LoadCrimeFactoryUATResult(
		const FString& ResultFile)
	{
		FCrimeFactoryUATResult Result;

		FString JsonText;

		if (ResultFile.IsEmpty() ||
			!FFileHelper::LoadFileToString(
				JsonText,
				*ResultFile))
		{
			return Result;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(JsonText);

		if (!FJsonSerializer::Deserialize(
				Reader,
				JsonObject) ||
			!JsonObject.IsValid())
		{
			return Result;
		}

		Result.bLoaded = true;

		const FString Status =
			GetJsonString(
				JsonObject,
				TEXT("Status"));

		Result.bSuccess =
			Status.Equals(
				TEXT("Success"),
				ESearchCase::IgnoreCase);

		Result.PluginName =
			GetJsonString(
				JsonObject,
				TEXT("PluginName"));

		Result.Phase =
			GetJsonString(
				JsonObject,
				TEXT("Phase"));

		Result.Mode =
			GetJsonString(
				JsonObject,
				TEXT("Mode"));

		Result.ChunkId =
			GetJsonString(
				JsonObject,
				TEXT("ChunkId"));

		Result.Message =
			GetJsonString(
				JsonObject,
				TEXT("Message"));

		Result.ExceptionType =
			GetJsonString(
				JsonObject,
				TEXT("ExceptionType"));

		Result.Details =
			GetJsonString(
				JsonObject,
				TEXT("Details"));

		Result.LogFolder =
			GetJsonString(
				JsonObject,
				TEXT("LogFolder"));

		Result.LogFile =
			GetJsonString(
				JsonObject,
				TEXT("LogFile"));

		Result.DurationSeconds =
			GetJsonString(
				JsonObject,
				TEXT("DurationSeconds"));

		return Result;
	}

	bool DoesResultTypeIndicateFailure(
		const FString& ResultType)
	{
		return
			ResultType.Contains(
				TEXT("Fail"),
				ESearchCase::IgnoreCase) ||
			ResultType.Contains(
				TEXT("Error"),
				ESearchCase::IgnoreCase) ||
			ResultType.Contains(
				TEXT("Cancel"),
				ESearchCase::IgnoreCase);
	}

	FString GetDefaultAutomationToolLogRoot()
	{
		const FString AppData =
			FPlatformMisc::GetEnvironmentVariable(
				TEXT("APPDATA"));

		if (AppData.IsEmpty())
		{
			return FString();
		}

		/*
		 * FPaths::EngineDir() is:
		 *
		 *   G:/Program Files/Epic Games/UE_5.5/Engine/
		 *
		 * AutomationTool keys its log folder from LocalRoot, which is the
		 * parent of Engine:
		 *
		 *   G:/Program Files/Epic Games/UE_5.5
		 *
		 * Do not construct this using EngineDir/..; NormalizeDirectoryName()
		 * does not reliably collapse ".." on every path form and that can
		 * produce a bogus folder such as:
		 *
		 *   G+Program+Files+Epic+Games+UE_5.5+Engine+..
		 */
		FString CurrentEngineDir =
			FPaths::ConvertRelativePathToFull(
				FPaths::EngineDir());

		FPaths::NormalizeDirectoryName(
			CurrentEngineDir);

		const FString EngineRoot =
			FPaths::GetPath(
				CurrentEngineDir);

		FString EngineLogFolderName =
			EngineRoot;

		// AutomationTool folder-safe LocalRoot form:
		// G:/Program Files/Epic Games/UE_5.5
		// -> G+Program+Files+Epic+Games+UE_5.5
		EngineLogFolderName.ReplaceInline(
			TEXT(":"),
			TEXT(""));

		EngineLogFolderName.ReplaceInline(
			TEXT("\\"),
			TEXT("+"));

		EngineLogFolderName.ReplaceInline(
			TEXT("/"),
			TEXT("+"));

		EngineLogFolderName.ReplaceInline(
			TEXT(" "),
			TEXT("+"));

		while (EngineLogFolderName.Contains(
			TEXT("++")))
		{
			EngineLogFolderName.ReplaceInline(
				TEXT("++"),
				TEXT("+"));
		}

		EngineLogFolderName.RemoveFromStart(
			TEXT("+"));

		EngineLogFolderName.RemoveFromEnd(
			TEXT("+"));

		const FString EngineSpecificRoot =
			FPaths::Combine(
				AppData,
				TEXT("Unreal Engine"),
				TEXT("AutomationTool"),
				TEXT("Logs"),
				EngineLogFolderName);

		UE_LOG(
			LogCrimeFactory,
			Verbose,
			TEXT(
				"AutomationTool log fallback for current editor engine: "
				"EngineDir='%s', LocalRoot='%s', LogRoot='%s'"
			),
			*CurrentEngineDir,
			*EngineRoot,
			*EngineSpecificRoot);

		if (IFileManager::Get().DirectoryExists(
				*EngineSpecificRoot))
		{
			return EngineSpecificRoot;
		}

		return FString();
	}

	FString FindNewestAutomationToolTextLog(
		const FString& Root)
	{
		if (Root.IsEmpty() ||
			!IFileManager::Get().DirectoryExists(*Root))
		{
			return FString();
		}

		TArray<FString> Files;

		IFileManager::Get().FindFilesRecursive(
			Files,
			*Root,
			TEXT("*.txt"),
			true,
			false,
			false);

		FString BestFile;
		FDateTime BestTimestamp =
			FDateTime::MinValue();

		for (const FString& File : Files)
		{
			const FDateTime Timestamp =
				IFileManager::Get().GetTimeStamp(*File);

			if (Timestamp > BestTimestamp)
			{
				BestTimestamp = Timestamp;
				BestFile = File;
			}
		}

		return BestFile;
	}

	FString ResolveBestUATLogFile(
		const FCrimeFactoryUATResult& Result)
	{
		if (!Result.LogFile.IsEmpty() &&
			IFileManager::Get().FileExists(
				*Result.LogFile))
		{
			return Result.LogFile;
		}

		FString LogRoot =
			Result.LogFolder;

		if (LogRoot.IsEmpty() ||
			!IFileManager::Get().DirectoryExists(
				*LogRoot))
		{
			LogRoot =
				GetDefaultAutomationToolLogRoot();
		}

		if (LogRoot.IsEmpty())
		{
			return FString();
		}

		/*
		 * Prefer AutomationTool's canonical Log.txt. If it is absent, search
		 * only the current engine's per-user UAT folder. Do not recurse through
		 * Engine/Programs/AutomationTool/Saved because that directory contains
		 * response files such as PakList*.txt and IoStoreCommands.txt which are
		 * not AutomationTool logs.
		 */
		const FString StandardLog =
			FPaths::Combine(
				LogRoot,
				TEXT("Log.txt"));

		if (IFileManager::Get().FileExists(
				*StandardLog))
		{
			return StandardLog;
		}

		return FindNewestAutomationToolTextLog(
			LogRoot);
	}

	FString ExtractRelevantUATLogLines(
		const FString& LogFile)
	{
		if (LogFile.IsEmpty())
		{
			return TEXT(
				"No AutomationTool log file could be resolved.");
		}

		FString LogText;

		if (!FFileHelper::LoadFileToString(
				LogText,
				*LogFile))
		{
			return FString::Printf(
				TEXT(
					"Could not read the AutomationTool log:\n%s"),
				*LogFile);
		}

		TArray<FString> Lines;
		LogText.ParseIntoArrayLines(
			Lines,
			false);

		TArray<FString> RelevantLines;

		const int32 FirstLine =
			FMath::Max(
				0,
				Lines.Num() - 2500);

		for (int32 Index = FirstLine;
			 Index < Lines.Num();
			 ++Index)
		{
			const FString& Line =
				Lines[Index];

			const bool bRelevant =
				Line.Contains(
					TEXT("Error:"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("error CS"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Exception:"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("AutomationException"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("BUILD FAILED"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Cook failed"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Missing "),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("does not exist"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Unable to"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Failed to"),
					ESearchCase::IgnoreCase) ||
				Line.Contains(
					TEXT("Unknown Cook Failure"),
					ESearchCase::IgnoreCase);

			if (!bRelevant)
			{
				continue;
			}

			FString CleanLine = Line;
			CleanLine.TrimStartAndEndInline();

			if (CleanLine.IsEmpty())
			{
				continue;
			}

			if (RelevantLines.Num() == 0 ||
				!RelevantLines.Last().Equals(
					CleanLine,
					ESearchCase::CaseSensitive))
			{
				RelevantLines.Add(
					CleanLine);
			}
		}

		const int32 MaxLines = 14;

		if (RelevantLines.Num() > MaxLines)
		{
			RelevantLines.RemoveAt(
				0,
				RelevantLines.Num() - MaxLines);
		}

		if (RelevantLines.Num() == 0)
		{
			const int32 TailStart =
				FMath::Max(
					0,
					Lines.Num() - 12);

			for (int32 Index = TailStart;
				 Index < Lines.Num();
				 ++Index)
			{
				FString CleanLine =
					Lines[Index];

				CleanLine.TrimStartAndEndInline();

				if (!CleanLine.IsEmpty())
				{
					RelevantLines.Add(
						CleanLine);
				}
			}
		}

		return FString::Join(
			RelevantLines,
			TEXT("\n"));
	}

	FString ClassifyPackagingFailure(
		const FCrimeFactoryUATResult& Result)
	{
        if (Result.Phase == TEXT("Archive mod")) return TEXT("Archiving");
        if (Result.Phase == TEXT("Export ZIP")) return TEXT("ZIP export");
		const FString Combined =
			Result.Phase +
			TEXT("\n") +
			Result.Message +
			TEXT("\n") +
			Result.Details;

		if (Combined.Contains(
				TEXT("Shipping.target"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT("target receipt"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("Shipping target / receipt");
		}

		if (Combined.Contains(
				TEXT("Chunk"),
				ESearchCase::IgnoreCase) &&
			(Combined.Contains(
				TEXT("not found"),
				ESearchCase::IgnoreCase) ||
			 Combined.Contains(
				TEXT("0 entries"),
				ESearchCase::IgnoreCase) ||
			 Combined.Contains(
				TEXT("manifest"),
				ESearchCase::IgnoreCase)))
		{
			return TEXT("PAL chunk / manifest");
		}

		if (Combined.Contains(
				TEXT(".utoc"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT(".ucas"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT("IoStore"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT("container"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("IoStore container merge");
		}

		if (Result.Phase.Contains(
				TEXT("Cook"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("Cook");
		}

		if (Result.Phase.Contains(
				TEXT("Build"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("Build");
		}

		if (Result.Phase.Contains(
				TEXT("Deploy"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT("CopyToGame"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("Deployment");
		}

		if (Combined.Contains(
				TEXT("GameDir"),
				ESearchCase::IgnoreCase) ||
			Combined.Contains(
				TEXT("PAYDAY3.exe"),
				ESearchCase::IgnoreCase))
		{
			return TEXT("PAYDAY 3 path");
		}

		if (Combined.Contains(
				TEXT("package"),
				ESearchCase::IgnoreCase) &&
			(Combined.Contains(
				TEXT("missing"),
				ESearchCase::IgnoreCase) ||
			 Combined.Contains(
				TEXT("does not exist"),
				ESearchCase::IgnoreCase)))
		{
			return TEXT("Missing package / asset");
		}

		return TEXT("AutomationTool");
	}

	FString GetPackagingFailureSuggestion(
		const FString& Category,
		const FCrimeFactoryUATResult& Result)
	{
        if (Category == TEXT("Archiving"))
            return TEXT("Cooking and staging finished, but archiving failed. Check the selected utility and Executable Path in CrimeFactory Project Settings, destination permissions, disk space, and the archive command output. The standard ZIP may still be available in Saved/ArchivedPlugins.");
        if (Category == TEXT("ZIP export"))
            return TEXT("The normal ZIP remains in Saved/ArchivedPlugins. Check the Additional ZIP Export Folder in CrimeFactory settings, available disk space, permissions, and whether the destination ZIP is open in another application.");

		if (Category == TEXT("Automation script compile"))
		{
			return TEXT(
				"AutomationTool failed while compiling/loading CrimeFactory's C# "
				"automation script, before PackagePlugin.ExecuteBuild() could run. "
				"The compiler error from the AutomationTool log is shown below.");
		}

		if (Category == TEXT("Shipping target / receipt"))
		{
			return TEXT(
				"CrimeFactory could not prepare the Win64 Shipping target required "
				"for the project chunk stage. Build PAYDAY3 Win64 Shipping once, "
				"or inspect the UBT error shown below.");
		}

		if (Category == TEXT("PAL chunk / manifest"))
		{
			return TEXT(
				"Check that the PAL is registered with Asset Manager, has the "
				"expected Chunk ID, and that its external /Game assets appear "
				"in the PAL inspector before packaging.");
		}

		if (Category == TEXT("IoStore container merge"))
		{
			return TEXT(
				"PASS 2 did not produce the IoStore files CrimeFactory expected, or "
				"they could not be merged into the PASS 1 plugin shell. Check "
				"the selected chunk and the .utoc/.ucas lines below.");
		}

		if (Category == TEXT("Cook"))
		{
			return TEXT(
				"The Unreal cook failed. The relevant cooker errors are shown "
				"below; missing packages, invalid references, and serialization "
				"errors are the most useful lines to fix first.");
		}

		if (Category == TEXT("Build"))
		{
			return TEXT(
				"The prerequisite game/Shipping build failed. Fix the compiler "
				"or UnrealBuildTool error shown below, then package the mod again.");
		}

		if (Category == TEXT("Deployment"))
		{
			return TEXT(
				"Packaging reached deployment but the built mod could not be "
				"copied to PAYDAY 3. Check the game path and whether files in "
				"the destination mod folder are locked.");
		}

		if (Category == TEXT("PAYDAY 3 path"))
		{
			return TEXT(
				"Check CrimeFactory's PD3Game Path. It must point at the PAYDAY 3 "
				"installation directory containing PAYDAY3.exe.");
		}

		if (Category == TEXT("Missing package / asset"))
		{
			return TEXT(
				"A referenced package is missing from the reconstructed project "
				"or mod. Check the package path in the error and either restore "
				"the placeholder/reference asset or remove the stale reference.");
		}

		return FString::Printf(
			TEXT(
				"AutomationTool failed during '%s'. The exception and most "
				"relevant log lines are shown below."),
			Result.Phase.IsEmpty()
				? TEXT("an unknown phase")
				: *Result.Phase);
	}

	void ShowPackagingFailureWindow(
		const FString& PluginName,
		const FString& ResultType,
		double RunTime,
		const FString& ResultFile,
		const FString& ExplicitUATLogFile,
		int32 RemainingModCount)
	{
		FCrimeFactoryUATResult Result =
			LoadCrimeFactoryUATResult(ResultFile);

		if (!ExplicitUATLogFile.IsEmpty() &&
			IFileManager::Get().FileExists(
				*ExplicitUATLogFile))
		{
			/*
			 * The explicit -log file is authoritative for this exact packaging
			 * invocation, including failures that happen before ExecuteBuild().
			 */
			Result.LogFile =
				ExplicitUATLogFile;

			Result.LogFolder =
				FPaths::GetPath(
					ExplicitUATLogFile);
		}

		if (Result.PluginName.IsEmpty())
		{
			Result.PluginName = PluginName;
		}

		if (!Result.bLoaded)
		{
			Result.Message =
				FString::Printf(
					TEXT(
						"AutomationTool returned '%s', but CrimeFactory's structured "
						"result file was not produced. The UAT process may have "
						"failed before PackagePlugin started."),
					*ResultType);

			Result.Phase =
				TEXT("AutomationTool startup");
		}

		const FString LogFile =
			ResolveBestUATLogFile(Result);

		const FString RelevantLines =
			ExtractRelevantUATLogLines(LogFile);

		FString Category =
			ClassifyPackagingFailure(Result);

		if (RelevantLines.Contains(
				TEXT("error CS"),
				ESearchCase::IgnoreCase) ||
			RelevantLines.Contains(
				TEXT("AutomationScripts.Automation"),
				ESearchCase::IgnoreCase) ||
			RelevantLines.Contains(
				TEXT("AutomationToolScripts"),
				ESearchCase::IgnoreCase))
		{
			Category =
				TEXT("Automation script compile");
		}

		const FString Suggestion =
			GetPackagingFailureSuggestion(
				Category,
				Result);

		FString EffectiveLogFolder =
			Result.LogFolder;

		if (EffectiveLogFolder.IsEmpty() ||
			!IFileManager::Get().DirectoryExists(
				*EffectiveLogFolder))
		{
			EffectiveLogFolder =
				GetDefaultAutomationToolLogRoot();
		}

		FCrimeFactoryPackagingFailure Failure;
		Failure.PluginName = Result.PluginName;
		Failure.ResultType = ResultType;
		Failure.Phase = Result.Phase;
		Failure.Category = Category;
		Failure.Mode = Result.Mode;
		Failure.ChunkId = Result.ChunkId;
		Failure.RunTime = RunTime;
		Failure.Message = Result.Message;
		Failure.Suggestion = Suggestion;
		Failure.ExceptionType = Result.ExceptionType;
		Failure.Details = Result.Details;
		Failure.RelevantLogLines = RelevantLines;
		Failure.LogFile = LogFile;
		Failure.LogFolder = EffectiveLogFolder;
		Failure.ResultFile = ResultFile;
		Failure.RemainingModCount = RemainingModCount;
		CrimeFactoryPackagingUI::ShowFailure(Failure);
	}

	bool DoesSoftReferencePackageExist(
		const FSoftObjectPath& AssetPath)
	{
		if (AssetPath.IsNull())
		{
			return false;
		}

		const FString ObjectPath = AssetPath.ToString();
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(ObjectPath);

		return !PackageName.IsEmpty() &&
			FPackageName::DoesPackageExist(PackageName);
	}

	bool IsPALRegisteredAsSpecificAsset(
		const FPrimaryAssetTypeInfo& PrimaryAssetLabelType,
		const FSoftObjectPath& PALObjectPath)
	{
		const TArray<FSoftObjectPath>& SpecificAssets =
			PrimaryAssetLabelType.GetSpecificAssets();

		return SpecificAssets.ContainsByPredicate(
			[&PALObjectPath](const FSoftObjectPath& ExistingPath)
			{
				return ExistingPath.ToString().Equals(
					PALObjectPath.ToString(),
					ESearchCase::IgnoreCase);
			});
	}

	bool IsMountRegisteredAsPALDirectory(
		const FPrimaryAssetTypeInfo& PrimaryAssetLabelType,
		const FString& PluginMountPath)
	{
		const TArray<FDirectoryPath>& Directories =
			PrimaryAssetLabelType.GetDirectories();

		return Directories.ContainsByPredicate(
			[&PluginMountPath](const FDirectoryPath& Directory)
			{
				return Directory.Path.Equals(
					PluginMountPath,
					ESearchCase::IgnoreCase);
			});
	}

	bool IsOutsidePluginMount(
		const FSoftObjectPath& AssetPath,
		const FString& PluginMountPath)
	{
		if (AssetPath.IsNull())
		{
			return false;
		}

		const FString PathString = AssetPath.ToString();

		return !PathString.StartsWith(
			PluginMountPath,
			ESearchCase::IgnoreCase
		);
	}

    struct FCrimeFactoryPALReferences
    {
        TArray<FSoftObjectPath> Assets;
        TArray<FSoftObjectPath> Blueprints;
        TArray<FSoftObjectPath> Directory;
        TArray<FString> Errors;
        TArray<FString> Warnings;
    };

    // Resolve sources at use time. Never materialize collection members in the PAL.
    FCrimeFactoryPALReferences ResolvePALReferences(const UPrimaryAssetLabel& Label)
    {
        FCrimeFactoryPALReferences Result;
        for (const auto& Asset : Label.ExplicitAssets)
            if (!Asset.IsNull()) Result.Assets.AddUnique(Asset.ToSoftObjectPath());
        for (const auto& Blueprint : Label.ExplicitBlueprints)
        {
            const FSoftObjectPath ClassPath = Blueprint.ToSoftObjectPath();
            if (ClassPath.IsNull()) continue;
            // _C is an object suffix, never part of the owning Blueprint package.
            if (ClassPath.GetLongPackageName().StartsWith(TEXT("/Script/")))
            {
                Result.Warnings.Add(TEXT("PAL native class is not a cookable Blueprint package: ") + ClassPath.ToString());
                continue;
            }
            Result.Blueprints.AddUnique(ClassPath);
        }
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        if (Label.bLabelAssetsInMyDirectory)
        {
            TArray<FAssetData> DirectoryAssets;
            Registry.GetAssetsByPath(FName(*FPackageName::GetLongPackagePath(Label.GetOutermost()->GetName())), DirectoryAssets, true);
            for (const FAssetData& Asset : DirectoryAssets)
                if (Label.bIncludeRedirectors || !Asset.IsRedirector()) Result.Directory.AddUnique(Asset.GetSoftObjectPath());
        }
        const FName CollectionName = Label.AssetCollection.CollectionName;
        if (CollectionName.IsNone()) return Result;
        ICollectionManager& Collections = FCollectionManagerModule::GetModule().Get();
        TArray<FCollectionNameType> Matches;
        // UE5.5 FCollectionReference stores only the name. Match native PAL's CST_All semantics.
        Collections.GetCollections(CollectionName, Matches);
        if (Matches.IsEmpty())
        {
            Result.Errors.Add(FString::Printf(TEXT("PAL %s references missing collection '%s'."), *Label.GetPathName(), *CollectionName.ToString()));
            return Result;
        }
        for (const FCollectionNameType& Match : Matches)
        {
            ECollectionStorageMode::Type StorageMode;
            if (!Collections.GetCollectionStorageMode(Match.Name, Match.Type, StorageMode))
            {
                Result.Errors.Add(TEXT("Cannot resolve collection storage mode: ") + Match.Name.ToString());
                continue;
            }
            if (StorageMode == ECollectionStorageMode::Dynamic)
            {
                // Native PAL GetAssetsInCollection cannot evaluate Content Browser search queries.
                // Fail explicitly rather than accepting an apparently empty, incomplete mod.
                Result.Errors.Add(FString::Printf(TEXT("Collection '%s' (%s) is dynamic. UE5.5 PALs require a static collection; dynamic search results cannot be used as a deterministic native PAL source."), *Match.Name.ToString(), ECollectionShareType::ToString(Match.Type)));
                continue;
            }
            TArray<FSoftObjectPath> Members;
            Collections.GetAssetsInCollection(Match.Name, Match.Type, Members, ECollectionRecursionFlags::Self);
            for (const FSoftObjectPath& Member : Members)
            {
                if (UE::PD3CookedEditor::PrimaryAssetLabels::IsCookedReference(Member))
                {
                    const FString Warning = FString::Printf(TEXT("Collection '%s': cooked asset excluded from forging: %s"), *Match.Name.ToString(), *Member.ToString());
                    Result.Warnings.Add(Warning);
                    UE_LOG(LogCrimeFactory, Warning, TEXT("%s"), *Warning);
                    continue;
                }
                const FString PackageName = Member.GetLongPackageName();
                if (!FPackageName::IsValidLongPackageName(PackageName) || PackageName.StartsWith(TEXT("/Script/"))
                    || !DoesSoftReferencePackageExist(Member))
                {
                    Result.Errors.Add(FString::Printf(TEXT("Collection '%s' member cannot resolve to a cookable package: %s"), *Match.Name.ToString(), *Member.ToString()));
                    continue;
                }
                const FAssetData Asset = Registry.GetAssetByObjectPath(Member);
                if (Asset.IsRedirector() && !Label.bIncludeRedirectors) continue;
                // Package existence alone is not proof that a referenced object is valid.
                if (!Asset.IsValid())
                {
                    Result.Errors.Add(FString::Printf(TEXT("Collection '%s' has an invalid asset reference: %s"), *Match.Name.ToString(), *Member.ToString()));
                    continue;
                }
                Result.Assets.AddUnique(Member);
            }
        }
        return Result;
    }

	struct FCrimeFactoryPALInspectionData
	{
		FString PluginName;
		FString PluginMountPath;
		FString PALPackagePath;
		FString PALObjectPath;

		bool bPALFound = false;
		bool bRuntimeLabel = false;
		bool bLabelAssetsInMyDirectory = false;
		bool bIncludeRedirectors = false;
		bool bApplyRecursively = false;
		bool bAlwaysCook = false;
		bool bTwoStagePackaging = false;
		bool bAssetManagerDirectoryRegistered = false;
		bool bPALSpecificAssetRegistered = false;

		int32 ChunkId = INDEX_NONE;

		TArray<FString> PluginAssets;
		TArray<FString> ExplicitInternalAssets;
		TArray<FString> ExplicitInternalBlueprints;
		TArray<FString> ExternalAssets;
		TArray<FString> ExternalBlueprints;
		TArray<FString> ExternalGameAssets;
		TArray<FString> ManagedChunkCandidates;
		TArray<FString> MissingExplicitReferences;
        TArray<FString> ResolutionErrors;
        TArray<FString> ResolutionWarnings;
	};

	void AddUniqueInspectionPath(
		TArray<FString>& Paths,
		const FString& Path)
	{
		if (!Path.IsEmpty())
		{
			Paths.AddUnique(Path);
		}
	}

	void SortInspectionPaths(
		FCrimeFactoryPALInspectionData& Data)
	{
		Data.PluginAssets.Sort();
		Data.ExplicitInternalAssets.Sort();
		Data.ExplicitInternalBlueprints.Sort();
		Data.ExternalAssets.Sort();
		Data.ExternalBlueprints.Sort();
		Data.ExternalGameAssets.Sort();
		Data.ManagedChunkCandidates.Sort();
		Data.MissingExplicitReferences.Sort();
	}

	FCrimeFactoryPALInspectionData BuildPALInspectionData(
		const IPlugin& Plugin)
	{
		FCrimeFactoryPALInspectionData Data;
		Data.PluginName = Plugin.GetName();

		Data.PluginMountPath =
			Plugin.GetMountedAssetPath();
		Data.PluginMountPath.RemoveFromEnd(TEXT("/"));

		const FString PluginContentPath =
			Data.PluginMountPath + TEXT("/");

		Data.PALPackagePath =
			PluginContentPath +
			FString::Printf(
				TEXT("PAL_%s"),
				*Data.PluginName);

		Data.PALObjectPath =
			Data.PALPackagePath +
			TEXT(".") +
			FString::Printf(
				TEXT("PAL_%s"),
				*Data.PluginName);

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));

		IAssetRegistry& AssetRegistry =
			AssetRegistryModule.Get();

		TArray<FString> PathsToScan;
		PathsToScan.Add(Data.PluginMountPath);

		AssetRegistry.ScanPathsSynchronous(
			PathsToScan,
			true);

		TArray<FAssetData> ModAssets;

		AssetRegistry.GetAssetsByPath(
			FName(*Data.PluginMountPath),
			ModAssets,
			true);

		for (const FAssetData& AssetData : ModAssets)
		{
			AddUniqueInspectionPath(
				Data.PluginAssets,
				AssetData.GetObjectPathString());
		}

		UPrimaryAssetLabel* Label =
			Cast<UPrimaryAssetLabel>(
				UEditorAssetLibrary::LoadAsset(
					Data.PALPackagePath));

		if (Label)
		{
			Data.bPALFound = true;
            const auto Resolved = ResolvePALReferences(*Label);
            Data.ResolutionErrors = Resolved.Errors;
            Data.ResolutionWarnings = Resolved.Warnings;
			Data.bRuntimeLabel =
				Label->bIsRuntimeLabel;
			Data.bLabelAssetsInMyDirectory =
				Label->bLabelAssetsInMyDirectory;
			Data.bIncludeRedirectors =
				Label->bIncludeRedirectors;
			Data.bApplyRecursively =
				Label->Rules.bApplyRecursively;
			Data.bAlwaysCook =
				Label->Rules.CookRule ==
					EPrimaryAssetCookRule::AlwaysCook;
			Data.ChunkId =
				Label->Rules.ChunkId;

			if (Data.bLabelAssetsInMyDirectory)
			{
				for (const FSoftObjectPath& PluginAsset : Resolved.Directory)
				{
					AddUniqueInspectionPath(
						Data.ManagedChunkCandidates,
						PluginAsset.ToString());
				}
			}

			for (const FSoftObjectPath& ExplicitAsset : Resolved.Assets)
			{
				const FSoftObjectPath AssetPath =
					ExplicitAsset;

				if (AssetPath.IsNull())
				{
					continue;
				}

				const FString PathString =
					AssetPath.ToString();

				AddUniqueInspectionPath(
					Data.ManagedChunkCandidates,
					PathString);

				if (!DoesSoftReferencePackageExist(AssetPath))
				{
					AddUniqueInspectionPath(
						Data.MissingExplicitReferences,
						PathString);
				}

				if (IsOutsidePluginMount(
						AssetPath,
						PluginContentPath))
				{
					AddUniqueInspectionPath(
						Data.ExternalAssets,
						PathString);

					if (PathString.StartsWith(
							TEXT("/Game/"),
							ESearchCase::IgnoreCase))
					{
						AddUniqueInspectionPath(
							Data.ExternalGameAssets,
							PathString);
					}
				}
				else
				{
					AddUniqueInspectionPath(
						Data.ExplicitInternalAssets,
						PathString);
				}
			}

			for (const FSoftObjectPath& ExplicitBlueprint : Resolved.Blueprints)
			{
				const FSoftObjectPath AssetPath =
					ExplicitBlueprint;

				if (AssetPath.IsNull())
				{
					continue;
				}

				const FString PathString =
					AssetPath.ToString();

				AddUniqueInspectionPath(
					Data.ManagedChunkCandidates,
					PathString);

				if (!DoesSoftReferencePackageExist(AssetPath))
				{
					AddUniqueInspectionPath(
						Data.MissingExplicitReferences,
						PathString);
				}

				if (IsOutsidePluginMount(
						AssetPath,
						PluginContentPath))
				{
					AddUniqueInspectionPath(
						Data.ExternalBlueprints,
						PathString);

					if (PathString.StartsWith(
							TEXT("/Game/"),
							ESearchCase::IgnoreCase))
					{
						AddUniqueInspectionPath(
							Data.ExternalGameAssets,
							PathString);
					}
				}
				else
				{
					AddUniqueInspectionPath(
						Data.ExplicitInternalBlueprints,
						PathString);
				}
			}

			Data.bTwoStagePackaging =
				Data.ExternalAssets.Num() > 0 ||
				Data.ExternalBlueprints.Num() > 0;
		}

		const UAssetManagerSettings* AssetManagerSettings =
			GetDefault<UAssetManagerSettings>();

		const FPrimaryAssetTypeInfo* PrimaryAssetLabelType =
			AssetManagerSettings
				? AssetManagerSettings
					->PrimaryAssetTypesToScan
					.FindByPredicate(
						[](const FPrimaryAssetTypeInfo& TypeInfo)
						{
							return TypeInfo.PrimaryAssetType ==
								FName(TEXT("PrimaryAssetLabel"));
						})
				: nullptr;

		if (PrimaryAssetLabelType)
		{
			Data.bAssetManagerDirectoryRegistered =
				IsMountRegisteredAsPALDirectory(
					*PrimaryAssetLabelType,
					Data.PluginMountPath);

			Data.bPALSpecificAssetRegistered =
				IsPALRegisteredAsSpecificAsset(
					*PrimaryAssetLabelType,
					FSoftObjectPath(Data.PALObjectPath));
		}

		SortInspectionPaths(Data);
		return Data;
	}

	int32 CountPALInspectionWarnings(
		const FCrimeFactoryPALInspectionData& Data)
	{
		if (!Data.bPALFound)
		{
			return 1;
		}

		int32 WarningCount = 0;
		WarningCount += Data.bRuntimeLabel ? 0 : 1;
		WarningCount += Data.bLabelAssetsInMyDirectory ? 0 : 1;
		WarningCount += Data.bIncludeRedirectors ? 0 : 1;
		WarningCount += Data.bApplyRecursively ? 0 : 1;
		WarningCount += Data.bAlwaysCook ? 0 : 1;
		WarningCount += Data.ChunkId >= 0 ? 0 : 1;
		WarningCount += Data.bAssetManagerDirectoryRegistered ? 0 : 1;
		WarningCount += Data.bPALSpecificAssetRegistered ? 0 : 1;
		WarningCount += Data.MissingExplicitReferences.Num() > 0 ? 1 : 0;
		return WarningCount;
	}

	bool HasAutoFixablePALInspectionWarnings(
		const FCrimeFactoryPALInspectionData& Data)
	{
		return
			!Data.bPALFound ||
			!Data.bRuntimeLabel ||
			!Data.bLabelAssetsInMyDirectory ||
			!Data.bIncludeRedirectors ||
			!Data.bApplyRecursively ||
			!Data.bAlwaysCook ||
			Data.ChunkId < 0 ||
			!Data.bAssetManagerDirectoryRegistered ||
			!Data.bPALSpecificAssetRegistered;
	}

	TArray<FString> GetPALInspectionIssues(const FCrimeFactoryPALInspectionData& Data)
	{
		TArray<FString> Issues;

		if (!Data.bPALFound)
		{
			Issues.Add(
				FString::Printf(
					TEXT("Primary Asset Label is missing: %s"),
					*Data.PALPackagePath));
		}
		else
		{
			if (!Data.bRuntimeLabel)
			{
				Issues.Add(TEXT("PAL is not marked as a Runtime Label."));
			}

			if (!Data.bLabelAssetsInMyDirectory)
			{
				Issues.Add(TEXT("PAL is not labeling assets in its directory."));
			}

			if (!Data.bIncludeRedirectors)
			{
				Issues.Add(TEXT("PAL is not including redirectors."));
			}

			if (!Data.bApplyRecursively)
			{
				Issues.Add(TEXT("PAL rules are not recursive."));
			}

			if (!Data.bAlwaysCook)
			{
				Issues.Add(TEXT("PAL Cook Rule is not Always Cook."));
			}

			if (Data.ChunkId < 0)
			{
				Issues.Add(TEXT("PAL Chunk ID is unset."));
			}
		}

		if (!Data.bAssetManagerDirectoryRegistered)
		{
			Issues.Add(
				FString::Printf(
					TEXT("Asset Manager directory registration is missing for %s."),
					*Data.PluginMountPath));
		}

		if (!Data.bPALSpecificAssetRegistered)
		{
			Issues.Add(TEXT("Asset Manager Specific Asset registration for the PAL is missing."));
		}

		if (Data.MissingExplicitReferences.Num() > 0)
		{
			Issues.Add(
				FString::Printf(
					TEXT("%d explicit PAL reference(s) point to missing packages. These are not removed automatically."),
					Data.MissingExplicitReferences.Num()));
		}

		return Issues;
	}

	void AppendPALInspectionIssues(const FCrimeFactoryPALInspectionData& Data, FString& InOutText)
	{
		const TArray<FString> Issues = GetPALInspectionIssues(Data);
		if (Issues.IsEmpty()) return;
		InOutText += TEXT("\n\nIssues:");
		for (const FString& Issue : Issues) InOutText += TEXT("\n  - ") + Issue;
	}

	bool CreateOrRepairCrimeFactoryPAL(
		IPlugin& Plugin,
		const FCrimeFactoryPALInspectionData& ExistingData,
		FString& OutResult)
	{
		UPrimaryAssetLabel* Label =
			Cast<UPrimaryAssetLabel>(
				UEditorAssetLibrary::LoadAsset(
					ExistingData.PALPackagePath));

		bool bCreatedPAL = false;

		if (!Label)
		{
			UPackage* Package =
				CreatePackage(
					*ExistingData.PALPackagePath);

			if (!Package)
			{
				OutResult =
					FString::Printf(
						TEXT("Unable to create package %s."),
						*ExistingData.PALPackagePath);
				return false;
			}

			const FString PALAssetName =
				FString::Printf(
					TEXT("PAL_%s"),
					*ExistingData.PluginName);

			Label =
				NewObject<UPrimaryAssetLabel>(
					Package,
					*PALAssetName,
					RF_Public |
					RF_Standalone |
					RF_Transactional);

			if (!Label)
			{
				OutResult =
					FString::Printf(
						TEXT("Unable to create %s."),
						*ExistingData.PALObjectPath);
				return false;
			}

			FAssetRegistryModule::AssetCreated(Label);
			bCreatedPAL = true;
		}

		Label->Modify();
		Label->bIsRuntimeLabel = true;
		Label->bLabelAssetsInMyDirectory = true;
		Label->bIncludeRedirectors = true;
		Label->Rules.bApplyRecursively = true;
		Label->Rules.CookRule =
			EPrimaryAssetCookRule::AlwaysCook;

		if (Label->Rules.ChunkId < 0)
		{
			Label->Rules.ChunkId = 333;
		}

		if (bCreatedPAL)
		{
			Label->Rules.Priority = 333;
		}

		const FString DataAssetObjectPath =
			FString::Printf(
				TEXT("/%s/DA_%s.DA_%s"),
				*ExistingData.PluginName,
				*ExistingData.PluginName,
				*ExistingData.PluginName);

		const FSoftObjectPath DataAssetPath(
			DataAssetObjectPath);

		if (DoesSoftReferencePackageExist(DataAssetPath))
		{
			const bool bAlreadyExplicit =
				Label->ExplicitAssets.ContainsByPredicate(
					[&DataAssetPath](
						const TSoftObjectPtr<UObject>& Existing)
					{
						return
							Existing.ToSoftObjectPath() ==
							DataAssetPath;
					});

			if (!bAlreadyExplicit)
			{
				Label->ExplicitAssets.Add(
					TSoftObjectPtr<UObject>(
						DataAssetPath));
			}
		}

		Label->MarkPackageDirty();

		if (!UEditorAssetLibrary::SaveLoadedAsset(
				Label,
				false))
		{
			OutResult =
				FString::Printf(
					TEXT("PAL was changed but could not be saved: %s"),
					*ExistingData.PALPackagePath);
			return false;
		}

		FCrimeFactoryModule& CrimeFactoryModule =
			FModuleManager::LoadModuleChecked<
				FCrimeFactoryModule>(
				TEXT("CrimeFactory"));

		CrimeFactoryModule.EnsureAssetManagerRegistrationForPlugin(
			Plugin);

		OutResult =
			bCreatedPAL
				? FString::Printf(
					TEXT("Created and configured %s."),
					*ExistingData.PALObjectPath)
				: FString::Printf(
					TEXT("Repaired %s."),
					*ExistingData.PALObjectPath);

		return true;
	}


	FString BuildPALAssetReport(
		const FCrimeFactoryPALInspectionData& Data)
	{
		const FString ChunkText =
			Data.ChunkId >= 0
				? FString::FromInt(Data.ChunkId)
				: TEXT("Unset");

		FString Report = FString::Printf(
			TEXT(
				"PAL / Chunk Inspector - %s\n\n"
				"PAL: %s\n"
				"Chunk ID: %s\n"
				"Packaging mode: %s\n"
				"Plugin assets: %d\n"
				"PAL chunk candidates: %d\n"
				"External references: %d\n"
				"External /Game references: %d\n\n"
			),
			*Data.PluginName,
			*Data.PALPackagePath,
			*ChunkText,
			Data.bTwoStagePackaging
				? TEXT("Two-stage")
				: TEXT("Single-stage"),
			Data.PluginAssets.Num(),
			Data.ManagedChunkCandidates.Num(),
			Data.ExternalAssets.Num() +
				Data.ExternalBlueprints.Num(),
			Data.ExternalGameAssets.Num());

		if (!Data.bPALFound)
		{
			Report +=
				TEXT(
					"PAL was not found. There are no PAL-managed assets to display.\n");

			return Report;
		}

		Report +=
			TEXT(
				"PAL-MANAGED CHUNK CANDIDATES\n"
				"----------------------------------------\n");

		if (Data.ManagedChunkCandidates.Num() == 0)
		{
			Report += TEXT("(none)\n");
		}
		else
		{
			for (const FString& AssetPath :
				Data.ManagedChunkCandidates)
			{
				Report += FString::Printf(
					TEXT("%s\n"),
					*AssetPath);
			}
		}

		Report +=
			TEXT(
				"\nEXTERNAL REFERENCES\n"
				"----------------------------------------\n");

		if (Data.ExternalAssets.Num() == 0 &&
			Data.ExternalBlueprints.Num() == 0)
		{
			Report += TEXT("(none)\n");
		}
		else
		{
			for (const FString& AssetPath :
				Data.ExternalAssets)
			{
				Report += FString::Printf(
					TEXT("[Asset] %s\n"),
					*AssetPath);
			}

			for (const FString& AssetPath :
				Data.ExternalBlueprints)
			{
				Report += FString::Printf(
					TEXT("[Blueprint] %s\n"),
					*AssetPath);
			}
		}

		Report +=
			TEXT(
				"\nALL ASSETS UNDER MOD MOUNT\n"
				"----------------------------------------\n");

		if (Data.PluginAssets.Num() == 0)
		{
			Report += TEXT("(none)\n");
		}
		else
		{
			for (const FString& AssetPath :
				Data.PluginAssets)
			{
				Report += FString::Printf(
					TEXT("%s\n"),
					*AssetPath);
			}
		}

		if (Data.MissingExplicitReferences.Num() > 0)
		{
			Report +=
				TEXT(
					"\nMISSING EXPLICIT REFERENCES\n"
					"----------------------------------------\n");

			for (const FString& AssetPath :
				Data.MissingExplicitReferences)
			{
				Report += FString::Printf(
					TEXT("%s\n"),
					*AssetPath);
			}
		}

		return Report;
	}

    TSharedRef<FCrimeFactoryModInspection> MakeInspectionPresentation(const FCrimeFactoryPALInspectionData& Data, const FString& Summary)
    {
        const auto View = MakeShared<FCrimeFactoryModInspection>();
        View->PluginName = Data.PluginName;
        View->PackagingMode = !Data.bPALFound ? TEXT("Packaging label unavailable") : Data.bTwoStagePackaging ? TEXT("Two-stage packaging") : TEXT("Single-stage packaging");
        View->Chunk = Data.ChunkId >= 0 ? FString::FromInt(Data.ChunkId) : TEXT("Unset");
        View->PluginAssetCount = Data.PluginAssets.Num();
        View->ManagedAssetCount = Data.ManagedChunkCandidates.Num();
        View->ExternalAssetCount = Data.ExternalAssets.Num() + Data.ExternalBlueprints.Num();
        View->bCanFix = HasAutoFixablePALInspectionWarnings(Data);
        View->Issues = GetPALInspectionIssues(Data);
        for (const FString& Error : Data.ResolutionErrors) View->Issues.Add(Error);
        for (const FString& Warning : Data.ResolutionWarnings) View->Issues.Add(Warning);
        for (const FString& Path : Data.MissingExplicitReferences) View->MissingPaths.Add(Path);
        View->InformationReport = Summary;
        View->AssetsReport = BuildPALAssetReport(Data);
        const auto Field = [&View](const TCHAR* Name, const FString& Value) { View->Fields.Emplace(FText::FromString(Name), Value); };
        const auto LabelFlag = [&Data](bool bFlag) { return !Data.bPALFound ? TEXT("Unavailable") : bFlag ? TEXT("Yes") : TEXT("No"); };
        Field(TEXT("Primary Asset Label"), Data.PALPackagePath + (Data.bPALFound ? TEXT("") : TEXT(" (missing)")));
        Field(TEXT("Mod content path"), Data.PluginMountPath);
        Field(TEXT("Runtime label"), LabelFlag(Data.bRuntimeLabel));
        Field(TEXT("Labels its directory"), LabelFlag(Data.bLabelAssetsInMyDirectory));
        Field(TEXT("Includes redirectors"), LabelFlag(Data.bIncludeRedirectors));
        Field(TEXT("Recursive rules"), LabelFlag(Data.bApplyRecursively));
        Field(TEXT("Cook rule"), !Data.bPALFound ? TEXT("Unavailable") : Data.bAlwaysCook ? TEXT("Always Cook") : TEXT("Not Always Cook"));
        Field(TEXT("Asset Manager directory"), Data.bAssetManagerDirectoryRegistered ? TEXT("Registered") : TEXT("Missing"));
        Field(TEXT("Specific PAL registration"), Data.bPALSpecificAssetRegistered ? TEXT("Registered") : TEXT("Missing"));
        Field(TEXT("External /Game references"), FString::FromInt(Data.ExternalGameAssets.Num()));
        const auto Group = [&View](const TCHAR* Id, const TCHAR* Name, const TCHAR* Description, const TArray<FString>& Paths)
        {
            auto Item = MakeShared<FCrimeFactoryInspectionAssetGroup>();
            Item->Id = FName(Id);
            Item->Name = FText::FromString(Name);
            Item->Description = FText::FromString(Description);
            Item->Paths = Paths;
            View->Groups.Add(Item);
        };
        Group(TEXT("Managed"), TEXT("PAL-managed chunk candidates"), TEXT("Directory and explicit references considered by the PAL. Final cook output also depends on cooking rules and dependencies."), Data.ManagedChunkCandidates);
        Group(TEXT("ExternalAssets"), TEXT("External asset references"), TEXT("Explicit asset references outside this mod's content mount."), Data.ExternalAssets);
        Group(TEXT("ExternalBlueprints"), TEXT("External Blueprint references"), TEXT("Explicit Blueprint class references outside this mod's content mount."), Data.ExternalBlueprints);
        Group(TEXT("Plugin"), TEXT("All assets in the mod"), TEXT("Assets discovered by the Asset Registry under this mod's content mount, including assets not managed by the PAL."), Data.PluginAssets);
        Group(TEXT("Missing"), TEXT("Missing explicit references"), TEXT("References whose packages could not be found. CrimeFactory never removes these automatically."), Data.MissingExplicitReferences);
        return View;
    }

	FString PALObjectPathToPackageName(
		const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}

		const FString PackageName =
			FPackageName::ObjectPathToPackageName(ObjectPath);

		return FPackageName::IsValidLongPackageName(
				PackageName,
				true)
			? PackageName
			: FString();
	}

	void AddUniquePackageNameFromObjectPath(
		TArray<FString>& PackageNames,
		const FString& ObjectPath)
	{
		const FString PackageName =
			PALObjectPathToPackageName(ObjectPath);

		if (!PackageName.IsEmpty())
		{
			if (!PackageNames.ContainsByPredicate([&PackageName](const FString& Existing) { return Existing.Equals(PackageName, ESearchCase::IgnoreCase); }))
                PackageNames.Add(PackageName);
		}
	}


	struct FCrimeFactoryAdditionalPakFile
	{
		FString Source;
		FString StagedPath;
	};

	bool TryGetWwiseEventShortId(const FString& ObjectPath, uint32& OutShortId)
	{
		OutShortId = 0;

		/*
		 * Prefer the Asset Registry metadata that Wwise itself writes for all
		 * AkAudioType assets. This is both cheaper and more robust than walking
		 * FWwiseEventInfo through reflection, where WwiseShortId is inherited
		 * from FWwiseObjectInfo.
		 */
		const FSoftObjectPath SoftPath(ObjectPath);
		const FString PackageName = SoftPath.GetLongPackageName();
		if (!PackageName.IsEmpty())
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(
				FName(*PackageName),
				Assets,
				false);

			for (const FAssetData& AssetData : Assets)
			{
				if (!AssetData.GetObjectPathString().Equals(
						ObjectPath,
						ESearchCase::IgnoreCase))
				{
					continue;
				}

				const FString ClassPath = AssetData.AssetClassPath.ToString();
				if (!ClassPath.EndsWith(
						TEXT(".AkAudioEvent"),
						ESearchCase::IgnoreCase) &&
					!ClassPath.Equals(
						TEXT("/Script/AkAudio.AkAudioEvent"),
						ESearchCase::IgnoreCase))
				{
					return false;
				}

				auto ShortIdTag =
					AssetData.TagsAndValues.FindTag(FName(TEXT("WwiseShortId")));
				if (ShortIdTag.IsSet())
				{
					OutShortId = static_cast<uint32>(
						FCString::Strtoui64(
							*ShortIdTag.GetValue(),
							nullptr,
							10));
					if (OutShortId != 0)
					{
						return true;
					}
				}
			}
		}

		/* Fallback for an asset whose registry tags have not refreshed yet. */
		UObject* Asset = UEditorAssetLibrary::LoadAsset(ObjectPath);
		if (!Asset || Asset->GetClass()->GetName() != TEXT("AkAudioEvent"))
		{
			return false;
		}

		const FStructProperty* EventInfoProperty =
			FindFProperty<FStructProperty>(Asset->GetClass(), TEXT("EventInfo"));
		if (!EventInfoProperty || !EventInfoProperty->Struct)
		{
			return false;
		}

		const void* EventInfo =
			EventInfoProperty->ContainerPtrToValuePtr<void>(Asset);
		const FUInt32Property* ShortIdProperty =
			FindFProperty<FUInt32Property>(
				EventInfoProperty->Struct,
				TEXT("WwiseShortId"));
		if (!ShortIdProperty)
		{
			return false;
		}

		OutShortId =
			ShortIdProperty->GetPropertyValue_InContainer(EventInfo);
		return OutShortId != 0;
	}

	FString GetConfiguredWwiseRootOutputPath()
	{
		auto ReadRootPath = [](const TCHAR* ClassPath,
			const TCHAR* PropertyName) -> FString
		{
			UClass* SettingsClass = FindObject<UClass>(nullptr, ClassPath);
			if (!SettingsClass)
			{
				return FString();
			}

			UObject* Settings = SettingsClass->GetDefaultObject();
			const FStructProperty* RootProperty =
				FindFProperty<FStructProperty>(SettingsClass, PropertyName);
			if (!RootProperty || !RootProperty->Struct)
			{
				return FString();
			}

			const void* RootValue =
				RootProperty->ContainerPtrToValuePtr<void>(Settings);
			const FStrProperty* PathProperty =
				FindFProperty<FStrProperty>(RootProperty->Struct, TEXT("Path"));
			return PathProperty
				? PathProperty->GetPropertyValue_InContainer(RootValue)
				: FString();
		};

		FString Root = ReadRootPath(
			TEXT("/Script/AkAudio.AkSettingsPerUser"),
			TEXT("RootOutputPathOverride"));

		if (Root.IsEmpty())
		{
			Root = ReadRootPath(
				TEXT("/Script/AkAudio.AkSettings"),
				TEXT("RootOutputPath"));
		}

		if (Root.IsEmpty())
		{
			return FString();
		}

		if (FPaths::IsRelative(Root))
		{
			const FString AbsoluteContentDir =
				FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
			Root = FPaths::ConvertRelativePathToFull(
				AbsoluteContentDir,
				Root);
		}

		FPaths::CollapseRelativeDirectories(Root);
		FPaths::NormalizeDirectoryName(Root);
		return Root;
	}

	bool GatherWwisePakFiles(
		const TArray<FString>& ExplicitPALAssets,
		TArray<FCrimeFactoryAdditionalPakFile>& OutFiles)
	{
		TSet<uint32> EventIds;

		/*
		 * Intentionally inspect ONLY assets explicitly authored into the PAL.
		 * Do not walk PAL-managed dependencies here. Base-game Wwise events and
		 * mod Wwise events both live under /Game/WwiseAudio, so PAL membership is
		 * the opt-in signal for copying generated BNK/WEM data.
		 */
		for (const FString& ObjectPath : ExplicitPALAssets)
		{
			// Loading is necessary: cooked retail registry tags omit editor Wwise identity.
			// Ask the integration whether its decoded snapshot remains valid before
			// staging a same-ID bank from a possibly unrelated authoring project.
			if (UObject* Asset = UEditorAssetLibrary::LoadAsset(ObjectPath))
			{
				if (UFunction* Query = Asset->FindFunction(TEXT("CanPreservePD3WwiseData")))
				{
					struct { bool ReturnValue = false; } Result;
					Asset->ProcessEvent(Query, &Result);
					if (Result.ReturnValue)
					{
						UE_LOG(LogCrimeFactory, Display, TEXT("Wwise retail replacement %s: preserving decoded dependencies in the installed game."), *ObjectPath);
						continue;
					}
				}
			}
			uint32 ShortId = 0;
			if (TryGetWwiseEventShortId(ObjectPath, ShortId))
			{
				EventIds.Add(ShortId);
				UE_LOG(
					LogCrimeFactory,
					Display,
					TEXT("Wwise PAL opt-in: %s [ShortId=%u]"),
					*ObjectPath,
					ShortId);
			}
		}

		if (EventIds.IsEmpty())
		{
			return true;
		}

		const FString RootOutput = GetConfiguredWwiseRootOutputPath();
		if (RootOutput.IsEmpty() ||
			!IFileManager::Get().DirectoryExists(*RootOutput))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"PAL explicitly references %d Wwise event(s), but no usable "
					"Wwise Root Output Path is configured. Generate Windows SoundBanks with JSON metadata before packaging."),
				EventIds.Num());
			return false;
		}

		const FString PlatformRoot =
			FPaths::Combine(RootOutput, TEXT("Windows"));
		if (!IFileManager::Get().DirectoryExists(*PlatformRoot))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"PAL explicitly references %d Wwise event(s), but the generated "
					"Windows SoundBank directory does not exist: %s"),
				EventIds.Num(),
				*PlatformRoot);
			return false;
		}

		/*
		 * Wwise does not always emit a global SoundbanksInfo.json. With per-bank
		 * metadata enabled, each generated bank/event has its own JSON file, for
		 * example GeneratedSoundBanks/Windows/Event/Test.json. Those files still
		 * contain the normal SoundBanksInfo -> SoundBanks schema, including the
		 * bank Path, Events/Ids, and loose Media paths.
		 *
		 * Build the lookup from those generated JSON files instead of requiring
		 * SoundbanksInfo.json. Match by the Wwise Short ID from the explicit PAL
		 * AkAudioEvent so Unreal asset names/locations do not have to match Wwise.
		 */
		TArray<FString> MetadataFiles;
		IFileManager::Get().FindFilesRecursive(
			MetadataFiles,
			*PlatformRoot,
			TEXT("*.json"),
			true,
			false,
			false);

		if (MetadataFiles.IsEmpty())
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"PAL explicitly references %d Wwise event(s), but no generated "
					"Wwise JSON metadata was found under %s. Generate Windows "
					"SoundBanks with JSON metadata before packaging."),
				EventIds.Num(),
				*PlatformRoot);
			return false;
		}

		bool bResourcesValid = true;
		TSet<FString> AddedStagedPaths;
		TSet<uint32> MatchedEventIds;

		auto AddGeneratedFile = [&](const FString& RelativePath)
		{
			if (RelativePath.IsEmpty())
			{
				return;
			}

			FString CleanRelative = RelativePath;
			CleanRelative.ReplaceInline(TEXT("\\"), TEXT("/"));
			while (CleanRelative.StartsWith(TEXT("/")))
			{
				CleanRelative.RightChopInline(1, EAllowShrinking::No);
			}

			const FString Source = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(PlatformRoot, CleanRelative));
			const FString StagedPath =
				FString(TEXT("PAYDAY3/Content/WwiseAudio/")) + CleanRelative;

			if (!IFileManager::Get().FileExists(*Source))
			{
				UE_LOG(
					LogCrimeFactory,
					Error,
					TEXT("Wwise generated resource is missing: %s"),
					*Source);
				bResourcesValid = false;
				return;
			}

			if (!AddedStagedPaths.Contains(StagedPath))
			{
				AddedStagedPaths.Add(StagedPath);
				OutFiles.Add({Source, StagedPath});
			}
		};

		auto ParseWwiseId = [](const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		uint32& OutId) -> bool
		{
			OutId = 0;
			if (!Object.IsValid())
			{
				return false;
			}

			FString IdString;
			if (Object->TryGetStringField(FieldName, IdString))
			{
				OutId = static_cast<uint32>(
					FCString::Strtoui64(*IdString, nullptr, 10));
				return OutId != 0;
			}

			double IdNumber = 0.0;
			if (Object->TryGetNumberField(FieldName, IdNumber) && IdNumber > 0.0)
			{
				OutId = static_cast<uint32>(IdNumber);
				return OutId != 0;
			}

			return false;
		};

		for (const FString& MetadataFile : MetadataFiles)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *MetadataFile))
			{
				continue;
			}

			TSharedPtr<FJsonObject> RootJson;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, RootJson) ||
				!RootJson.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* SoundBanksInfo = nullptr;
			if (!RootJson->TryGetObjectField(
					TEXT("SoundBanksInfo"),
					SoundBanksInfo) ||
				!SoundBanksInfo ||
				!SoundBanksInfo->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Banks = nullptr;
			if (!(*SoundBanksInfo)->TryGetArrayField(
					TEXT("SoundBanks"),
					Banks) ||
				!Banks)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& BankValue : *Banks)
			{
				const TSharedPtr<FJsonObject> Bank =
					BankValue.IsValid() ? BankValue->AsObject() : nullptr;
				if (!Bank.IsValid())
				{
					continue;
				}

				bool bSelectedEventBank = false;
				const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
				if (Bank->TryGetArrayField(TEXT("Events"), Events) && Events)
				{
					for (const TSharedPtr<FJsonValue>& EventValue : *Events)
					{
						const TSharedPtr<FJsonObject> Event =
							EventValue.IsValid() ? EventValue->AsObject() : nullptr;
						uint32 EventId = 0;
						if (ParseWwiseId(Event, TEXT("Id"), EventId) &&
							EventIds.Contains(EventId))
						{
							bSelectedEventBank = true;
							MatchedEventIds.Add(EventId);
						}
					}
				}

				/*
				 * Auto-defined Event banks normally use the Event Short ID as the
				 * bank ID as well. Keep this fallback for metadata variants that do
				 * not emit an Events array.
				 */
				if (!bSelectedEventBank)
				{
					uint32 BankId = 0;
					if (ParseWwiseId(Bank, TEXT("Id"), BankId) &&
						EventIds.Contains(BankId))
					{
						bSelectedEventBank = true;
						MatchedEventIds.Add(BankId);
					}
				}

				if (!bSelectedEventBank)
				{
					continue;
				}

				FString BankPath;
				if (Bank->TryGetStringField(TEXT("Path"), BankPath))
				{
					AddGeneratedFile(BankPath);
				}

				const TArray<TSharedPtr<FJsonValue>>* Media = nullptr;
				if (Bank->TryGetArrayField(TEXT("Media"), Media) && Media)
				{
					for (const TSharedPtr<FJsonValue>& MediaValue : *Media)
					{
						const TSharedPtr<FJsonObject> MediaObject =
							MediaValue.IsValid() ? MediaValue->AsObject() : nullptr;
						FString MediaPath;
						if (MediaObject.IsValid() &&
							MediaObject->TryGetStringField(TEXT("Path"), MediaPath))
						{
							AddGeneratedFile(MediaPath);
						}
					}
				}
			}

			if (MatchedEventIds.Num() == EventIds.Num())
			{
				break;
			}
		}

		for (const uint32 EventId : EventIds)
		{
			if (!MatchedEventIds.Contains(EventId))
			{
				UE_LOG(
					LogCrimeFactory,
					Error,
					TEXT(
						"Wwise Event ShortId %u is explicitly included in the PAL, "
						"but no matching generated bank metadata was found under %s."),
					EventId,
					*PlatformRoot);
			}
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"Wwise PAL scan found %d/%d explicitly selected event(s) in generated "
				"per-bank JSON metadata and %d BNK/WEM file(s) to stage into the main mod pak."),
			MatchedEventIds.Num(),
			EventIds.Num(),
			OutFiles.Num());
		return bResourcesValid && MatchedEventIds.Num() == EventIds.Num();
	}

	bool WritePALPackagingManifest(
		const IPlugin& Plugin,
		const int32 ChunkId,
		const FString& ManifestFile)
	{
		const FCrimeFactoryPALInspectionData Data =
			BuildPALInspectionData(Plugin);

        if (!Data.ResolutionErrors.IsEmpty())
        {
            for (const FString& Error : Data.ResolutionErrors) UE_LOG(LogCrimeFactory, Error, TEXT("%s"), *Error);
            return false;
        }

		if (!Data.bPALFound)
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Cannot write PAL packaging manifest for %s: "
					"PAL was not found at %s."
				),
				*Plugin.GetName(),
				*Data.PALPackagePath);

			return false;
		}

		if (Data.ChunkId != ChunkId)
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Cannot write PAL packaging manifest for %s: "
					"requested Chunk %d but PAL currently uses Chunk %d."
				),
				*Plugin.GetName(),
				ChunkId,
				Data.ChunkId);

			return false;
		}

		TArray<FString> ExternalCookPackages;
		TArray<FString> ExpectedChunkPackages;

		for (const FString& ObjectPath : Data.ExternalAssets)
		{
			AddUniquePackageNameFromObjectPath(
				ExternalCookPackages,
				ObjectPath);
		}

		for (const FString& ObjectPath : Data.ExternalBlueprints)
		{
			AddUniquePackageNameFromObjectPath(
				ExternalCookPackages,
				ObjectPath);
		}

		for (const FString& ObjectPath : Data.ManagedChunkCandidates)
		{
			AddUniquePackageNameFromObjectPath(
				ExpectedChunkPackages,
				ObjectPath);
		}

		ExternalCookPackages.Sort();
		ExpectedChunkPackages.Sort();

		TArray<FString> ExplicitWwiseCandidates =
			Data.ExplicitInternalAssets;
		for (const FString& ObjectPath : Data.ExternalAssets)
		{
			ExplicitWwiseCandidates.AddUnique(ObjectPath);
		}

		TArray<FCrimeFactoryAdditionalPakFile> AdditionalPakFiles;
		if (!GatherWwisePakFiles(
			ExplicitWwiseCandidates,
			AdditionalPakFiles)) return false;

		if (ExternalCookPackages.Num() == 0)
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Two-stage packaging was selected for %s, but no "
					"external package names could be generated from its PAL."
				),
				*Plugin.GetName());

			return false;
		}

		TSharedRef<FJsonObject> RootObject =
			MakeShared<FJsonObject>();

		RootObject->SetStringField(
			TEXT("PluginName"),
			Plugin.GetName());

		RootObject->SetNumberField(
			TEXT("ChunkId"),
			ChunkId);

		RootObject->SetStringField(
			TEXT("PAL"),
			Data.PALPackagePath);

		TArray<TSharedPtr<FJsonValue>> ExternalValues;
		for (const FString& PackageName : ExternalCookPackages)
		{
			ExternalValues.Add(
				MakeShared<FJsonValueString>(
					PackageName));
		}

		TArray<TSharedPtr<FJsonValue>> ExpectedValues;
		for (const FString& PackageName : ExpectedChunkPackages)
		{
			ExpectedValues.Add(
				MakeShared<FJsonValueString>(
					PackageName));
		}

		RootObject->SetArrayField(
			TEXT("ExternalCookPackages"),
			ExternalValues);

		RootObject->SetArrayField(
			TEXT("ExpectedChunkPackages"),
			ExpectedValues);

		TArray<TSharedPtr<FJsonValue>> AdditionalPakValues;
		for (const FCrimeFactoryAdditionalPakFile& File : AdditionalPakFiles)
		{
			TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("Source"), File.Source);
			FileObject->SetStringField(TEXT("StagedPath"), File.StagedPath);
			AdditionalPakValues.Add(
				MakeShared<FJsonValueObject>(FileObject));
		}
		RootObject->SetArrayField(
			TEXT("AdditionalPakFiles"),
			AdditionalPakValues);

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&JsonText);

		if (!FJsonSerializer::Serialize(
				RootObject,
				Writer))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Failed to serialize PAL packaging manifest for %s."
				),
				*Plugin.GetName());

			return false;
		}

		if (!FFileHelper::SaveStringToFile(
				JsonText,
				*ManifestFile))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"Failed to write PAL packaging manifest for %s: %s"
				),
				*Plugin.GetName(),
				*ManifestFile);

			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"Wrote deterministic PAL packaging manifest for %s: "
				"%d external cook package(s), %d expected Chunk %d package(s)."
			),
			*Plugin.GetName(),
			ExternalCookPackages.Num(),
			ExpectedChunkPackages.Num(),
			ChunkId);

		for (const FString& PackageName : ExternalCookPackages)
		{
			UE_LOG(
				LogCrimeFactory,
				Display,
				TEXT("  Force cook external package: %s"),
				*PackageName);
		}

		return true;
	}

	int32 GetModChunkIdIfExternalContentExists(const IPlugin& Plugin)
	{
		FString PluginMountPath = Plugin.GetMountedAssetPath();
		if (!PluginMountPath.EndsWith(TEXT("/")))
		{
			PluginMountPath += TEXT("/");
		}

		const FString PluginName = Plugin.GetName();
		const FString PALPath =
			PluginMountPath + FString::Printf(TEXT("PAL_%s"), *PluginName);

		UPrimaryAssetLabel* Label =
			Cast<UPrimaryAssetLabel>(UEditorAssetLibrary::LoadAsset(PALPath));

		if (!Label)
		{
			UE_LOG(
				LogCrimeFactory,
				Display,
				TEXT(
					"No PAL found at %s. No external-content check is "
					"needed; using the normal plugin DLC cook."
				),
				*PALPath
			);
			return INDEX_NONE;
		}

        const auto Data = BuildPALInspectionData(Plugin);
        const bool bHasExternalContent = !Data.ExternalAssets.IsEmpty() || !Data.ExternalBlueprints.IsEmpty();

		if (!bHasExternalContent)
		{
			UE_LOG(
				LogCrimeFactory,
				Display,
				TEXT(
					"PAL %s contains no resolved references outside %s. Skipping PASS 2 and using "
					"the normal plugin DLC cook only."
				),
				*PALPath,
				*PluginMountPath
			);

			return INDEX_NONE;
		}

		const int32 ChunkId = Label->Rules.ChunkId;
		if (ChunkId < 0)
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"PAL %s contains external content but has no valid "
					"Chunk ID. External /Game content cannot be included "
					"by the two-stage cook."
				),
				*PALPath
			);

			return INDEX_NONE;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"Two-stage packaging enabled for %s: %s -> Chunk %d"
			),
			*PluginName,
			*PALPath,
			ChunkId
		);

		return ChunkId;
	}

	FString ResolveCrimeFactoryAutomationScriptDir()
	{
		if (const TSharedPtr<IPlugin> CrimeFactoryPlugin =
			IPluginManager::Get().FindPlugin(TEXT("CrimeFactory"));
			CrimeFactoryPlugin.IsValid())
		{
			return FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					CrimeFactoryPlugin->GetBaseDir(),
					TEXT("Source/CrimeFactory.Automation")));
		}

		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectPluginsDir(),
				TEXT("CrimeFactory/Source/CrimeFactory.Automation")));
	}

	FString GetCrimeFactoryAutomationDllPath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
				TEXT("Binaries/DotNET/AutomationTool/AutomationScripts/CrimeFactory/net8.0/CrimeFactory.Automation.dll")));
	}

	FString GetCrimeFactoryAutomationBootstrapMarkerPath()
	{
		FString EngineRoot =
			FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
		FPaths::NormalizeDirectoryName(EngineRoot);

		const uint32 EngineHash = GetTypeHash(EngineRoot.ToLower());

		return FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("CrimeFactory"),
			TEXT("Setup"),
			FString::Printf(
				TEXT("Automation_%08X.ready"),
				EngineHash));
	}

	FDateTime GetNewestCrimeFactoryAutomationSourceTimestamp(
		const FString& ScriptDir)
	{
		IFileManager& FileManager = IFileManager::Get();
		FDateTime Newest = FDateTime::MinValue();

		const TCHAR* SourceFiles[] =
		{
			TEXT("CrimeFactory.Automation.csproj"),
			TEXT("CrimeFactory.Automation.csproj.props"),
			TEXT("Packaging/PackagePlugin.cs"),
            TEXT("Packaging/CrimeFactoryZipExport.cs"),
            TEXT("Packaging/CrimeFactoryArchive.cs")
		};

		for (const TCHAR* SourceFile : SourceFiles)
		{
			const FString FullPath =
				FPaths::Combine(ScriptDir, SourceFile);

			if (FileManager.FileExists(*FullPath))
			{
				const FDateTime Timestamp =
					FileManager.GetTimeStamp(*FullPath);

				if (Timestamp > Newest)
				{
					Newest = Timestamp;
				}
			}
		}

		return Newest;
	}

	FString GetCrimeFactoryAutomationSourceFingerprint(
		const FString& ScriptDir)
	{
		/*
		 * Do not rely on timestamps alone. Plugin files may be copied from an
		 * archive or Git checkout with timestamps older than a previously built
		 * AutomationTool DLL. Fingerprint the actual source contents so an updated
		 * PackagePlugin.cs always invalidates the installed automation assembly.
		 */
		const TCHAR* SourceFiles[] =
		{
			TEXT("CrimeFactory.Automation.csproj"),
			TEXT("CrimeFactory.Automation.csproj.props"),
			TEXT("Packaging/PackagePlugin.cs"),
            TEXT("Packaging/CrimeFactoryZipExport.cs"),
            TEXT("Packaging/CrimeFactoryArchive.cs")
		};

		uint32 Fingerprint = 2166136261u;

		for (const TCHAR* SourceFile : SourceFiles)
		{
			const FString FullPath =
				FPaths::Combine(ScriptDir, SourceFile);

			FString Contents;
			if (!FFileHelper::LoadFileToString(Contents, *FullPath))
			{
				continue;
			}

			Fingerprint =
				(Fingerprint ^ GetTypeHash(FString(SourceFile))) * 16777619u;
			Fingerprint =
				(Fingerprint ^ GetTypeHash(Contents)) * 16777619u;
		}

		return FString::Printf(TEXT("%08X"), Fingerprint);
	}

	FString GetProcessOutputTail(const FString& Output)
	{
		constexpr int32 MaxDialogChars = 6000;
		if (Output.Len() <= MaxDialogChars)
		{
			return Output;
		}

		return TEXT("...\n") + Output.Right(MaxDialogChars);
	}

	bool EnsureCrimeFactoryAutomationReady(
		const FString& ProjectPath,
		const FString& ScriptDir)
	{
		IFileManager& FileManager = IFileManager::Get();

		if (!FileManager.DirectoryExists(*ScriptDir))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("CrimeFactory Automation script directory does not exist: %s"),
				*ScriptDir);

			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory could not find its AutomationTool scripts:\n\n%s\n\n"
							"Packaging cannot continue."),
						*ScriptDir)));

			return false;
		}

		const FString PackagePluginSource =
			FPaths::Combine(ScriptDir, TEXT("Packaging/PackagePlugin.cs"));

		if (!FileManager.FileExists(*PackagePluginSource))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT("CrimeFactory PackagePlugin source does not exist: %s"),
				*PackagePluginSource);

			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory could not find PackagePlugin.cs:\n\n%s\n\n"
							"Packaging cannot continue."),
						*PackagePluginSource)));

			return false;
		}

		const FString AutomationDll =
			GetCrimeFactoryAutomationDllPath();
		const FString MarkerFile =
			GetCrimeFactoryAutomationBootstrapMarkerPath();

		const FDateTime NewestSourceTimestamp =
			GetNewestCrimeFactoryAutomationSourceTimestamp(ScriptDir);
		const FString SourceFingerprint =
			GetCrimeFactoryAutomationSourceFingerprint(ScriptDir);

		const FDateTime DllTimestamp =
			FileManager.FileExists(*AutomationDll)
				? FileManager.GetTimeStamp(*AutomationDll)
				: FDateTime::MinValue();
		const FDateTime MarkerTimestamp =
			FileManager.FileExists(*MarkerFile)
				? FileManager.GetTimeStamp(*MarkerFile)
				: FDateTime::MinValue();

		FString MarkerContents;
		const bool bMarkerLoaded =
			FileManager.FileExists(*MarkerFile) &&
			FFileHelper::LoadFileToString(MarkerContents, *MarkerFile);
		const bool bFingerprintMatches =
			bMarkerLoaded &&
			MarkerContents.Contains(
				TEXT("SourceFingerprint=") + SourceFingerprint,
				ESearchCase::CaseSensitive);

		const bool bBootstrapRequired =
			!FileManager.FileExists(*AutomationDll) ||
			!FileManager.FileExists(*MarkerFile) ||
			!bFingerprintMatches ||
			DllTimestamp < NewestSourceTimestamp ||
			MarkerTimestamp < NewestSourceTimestamp;

		if (!bBootstrapRequired)
		{
			UE_LOG(
				LogCrimeFactory,
				Verbose,
				TEXT("CrimeFactory AutomationTool bootstrap is already valid for this engine."));
			return true;
		}

		const FString RunUATBat =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::EngineDir(),
					TEXT("Build/BatchFiles/RunUAT.bat")));

		if (!FileManager.FileExists(*RunUATBat))
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory first-time setup could not find RunUAT.bat:\n\n%s"),
						*RunUATBat)));
			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"CrimeFactory AutomationTool bootstrap is missing or stale. "
				"Running one-time setup for the current engine..."));

		/*
		 * Remove stale outputs before invoking UAT. This prevents MSBuild/UAT from
		 * reusing an old CrimeFactory.Automation assembly or obj cache after a plugin
		 * update, especially when archive timestamps make the old DLL look newer.
		 */
		const FString AutomationOutputDir = FPaths::GetPath(AutomationDll);
		FileManager.Delete(*AutomationDll, false, true, true);
		FileManager.Delete(
			*FPaths::Combine(AutomationOutputDir, TEXT("CrimeFactory.Automation.pdb")),
			false, true, true);
		FileManager.Delete(
			*FPaths::Combine(AutomationOutputDir, TEXT("CrimeFactory.Automation.deps.json")),
			false, true, true);

		FileManager.DeleteDirectory(
			*FPaths::Combine(ScriptDir, TEXT("obj")),
			false,
			true);

		FileManager.Delete(
			*FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("ScriptModules/CrimeFactory.Automation.json")),
			false, true, true);

		const FString BootstrapArguments =
			FString::Printf(
				TEXT(
					"-ScriptsForProject=\"%s\" -ScriptDir=\"%s\" -List"),
				*ProjectPath,
				*ScriptDir);

		int32 ReturnCode = INDEX_NONE;
		FString Output;

		const bool bStarted = CrimeFactoryPackagingUI::RunSetup(
			RunUATBat,
			BootstrapArguments,
			ReturnCode,
			Output,
			TEXT("Preparing AutomationTool integration..."),
			TEXT("Compiling and registering CrimeFactory PackagePlugin for this engine."));

		const int32 CrimeFactorySection =
			Output.Find(
				TEXT("CrimeFactory.Automation:"),
				ESearchCase::CaseSensitive);

		const bool bPackagePluginRegistered =
			CrimeFactorySection != INDEX_NONE &&
			Output.Mid(CrimeFactorySection, 512).Contains(
				TEXT("PackagePlugin"),
				ESearchCase::CaseSensitive);

		if (!bStarted ||
			ReturnCode != 0 ||
			!bPackagePluginRegistered ||
			!FileManager.FileExists(*AutomationDll))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"CrimeFactory AutomationTool bootstrap failed. "
					"Started=%s ReturnCode=%d PackagePlugin=%s DLL=%s"),
				bStarted ? TEXT("true") : TEXT("false"),
				ReturnCode,
				bPackagePluginRegistered ? TEXT("found") : TEXT("missing"),
				FileManager.FileExists(*AutomationDll)
					? TEXT("present")
					: TEXT("missing"));

			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory could not complete its one-time AutomationTool setup.\n\n"
							"Exit code: %d\nPackagePlugin: %s\nAutomation DLL: %s\n\n%s"),
						ReturnCode,
						bPackagePluginRegistered ? TEXT("Found") : TEXT("Not found"),
						FileManager.FileExists(*AutomationDll) ? TEXT("Found") : TEXT("Not found"),
						*GetProcessOutputTail(Output))));

			return false;
		}

		FileManager.MakeDirectory(
			*FPaths::GetPath(MarkerFile),
			true);

		const FString NewMarkerContents =
			FString::Printf(
				TEXT(
					"Engine=%s\nScriptDir=%s\nAutomationDll=%s\nSourceFingerprint=%s\n"),
				*FPaths::ConvertRelativePathToFull(FPaths::EngineDir()),
				*ScriptDir,
				*AutomationDll,
				*SourceFingerprint);

		if (!FFileHelper::SaveStringToFile(
				NewMarkerContents,
				*MarkerFile))
		{
			UE_LOG(
				LogCrimeFactory,
				Warning,
				TEXT(
					"CrimeFactory AutomationTool bootstrap succeeded, but its ready marker "
					"could not be written: %s"),
				*MarkerFile);
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT("CrimeFactory AutomationTool bootstrap completed. PackagePlugin is registered."));

		return true;
	}

	bool EnsureUnrealPakReady()
	{
#if PLATFORM_WINDOWS
		IFileManager& FileManager = IFileManager::Get();

		const FString UnrealPakExe =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::EngineDir(),
					TEXT("Binaries/Win64/UnrealPak.exe")));

		if (FileManager.FileExists(*UnrealPakExe))
		{
			return true;
		}

		const FString BuildBat =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::EngineDir(),
					TEXT("Build/BatchFiles/Build.bat")));

		if (!FileManager.FileExists(*BuildBat))
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory requires UnrealPak.exe, but it is missing and this engine "
							"does not contain Build.bat.\n\nExpected UnrealPak:\n%s"),
						*UnrealPakExe)));
			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT(
				"UnrealPak.exe is missing from the current source engine. "
				"Building the UnrealPak target once..."));

		int32 ReturnCode = INDEX_NONE;
		FString Output;

		const bool bStarted = CrimeFactoryPackagingUI::RunSetup(
			BuildBat,
			TEXT("UnrealPak Win64 Development -WaitMutex"),
			ReturnCode,
			Output,
			TEXT("Preparing UnrealPak..."),
			TEXT("UnrealPak is missing from this source build, so CrimeFactory is building it now."));

		if (!bStarted ||
			ReturnCode != 0 ||
			!FileManager.FileExists(*UnrealPakExe))
		{
			UE_LOG(
				LogCrimeFactory,
				Error,
				TEXT(
					"CrimeFactory could not build UnrealPak. Started=%s ReturnCode=%d Path=%s"),
				bStarted ? TEXT("true") : TEXT("false"),
				ReturnCode,
				*UnrealPakExe);

			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::FromString(
					FString::Printf(
						TEXT(
							"CrimeFactory requires UnrealPak.exe for staging, but the automatic "
							"source-engine build failed.\n\nExit code: %d\n\n%s"),
						ReturnCode,
						*GetProcessOutputTail(Output))));

			return false;
		}

		UE_LOG(
			LogCrimeFactory,
			Display,
			TEXT("UnrealPak first-time setup completed: %s"),
			*UnrealPakExe);
#endif

		return true;
	}

}


void SCrimeFactoryModEntry::Construct(
	const FArguments& Args,
	TSharedRef<IPlugin> InMod,
	TSharedRef<SCrimeFactoryModEntryList> InOwner)
{
	Mod = InMod;
	Owner = InOwner;

	UCrimeFactorySettings* Settings =
		UCrimeFactorySettings::Get();

	const FString PluginName =
		Mod->GetName();

	Checkbox =
		SNew(SCheckBox)
		.OnCheckStateChanged(
			this,
			&SCrimeFactoryModEntry::OnEnableCheckboxChanged)
		.IsChecked(
			Settings->ModSelection.FindOrAdd(
				PluginName,
				false));

    RefreshModIcon();
    RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrimeFactoryModEntry::PollModIcon));

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(190.0f)
		[
			SNew(SBorder)
			.Padding(FMargin(6.0f))
			[
				SNew(SVerticalBox)

				// Card header: selection, mod name, delete.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						Checkbox.ToSharedRef()
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]
                        {
                            return FText::FromString(Mod->GetDescriptor().FriendlyName.IsEmpty() ? Mod->GetName() : Mod->GetDescriptor().FriendlyName);
                        })
						.Justification(ETextJustify::Center)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
						.HighlightText_Lambda(
							[WeakOwner = TWeakPtr<SCrimeFactoryModEntryList>(InOwner)]()
							{
								const auto Pinned = WeakOwner.Pin();
								return Pinned ? FText::FromString(Pinned->GetLastFilter()) : FText::GetEmpty();
							})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(
							FAppStyle::Get(),
							TEXT("SimpleButton"))
						.ContentPadding(FMargin(2.0f))
						.OnClicked(
							this,
							&SCrimeFactoryModEntry::OnDeleteMod)
						.Visibility_Lambda(
							[this]()
							{
								if (!Mod.IsValid())
								{
									return EVisibility::Collapsed;
								}

								FString ModBaseDir =
									FPaths::ConvertRelativePathToFull(
										Mod->GetBaseDir());

								FString ProjectModsDir =
									FPaths::ConvertRelativePathToFull(
										FPaths::Combine(
											FPaths::ProjectDir(),
											TEXT("Mods")));

								FPaths::NormalizeDirectoryName(
									ModBaseDir);
								FPaths::NormalizeDirectoryName(
									ProjectModsDir);

								const FString ProjectModsPrefix =
									ProjectModsDir.EndsWith(TEXT("/"))
										? ProjectModsDir
										: ProjectModsDir + TEXT("/");

								return
									ModBaseDir.StartsWith(
										ProjectModsPrefix,
										ESearchCase::IgnoreCase) &&
									ModBaseDir.Len() >
										ProjectModsPrefix.Len()
										? EVisibility::Visible
										: EVisibility::Collapsed;
							})
						.ToolTipText_Lambda(
							[this]()
							{
								return Mod.IsValid()
									? FText::FromString(
										FString::Printf(
											TEXT("Permanently delete %s"),
											*Mod->GetName()))
									: FText::GetEmpty();
							})
						[
							SNew(SImage)
							.Image(
								FAppStyle::GetBrush(
									TEXT("Icons.Delete")))
							.ColorAndOpacity(
								FLinearColor(
									1.0f,
									0.20f,
									0.20f,
									1.0f))
						]
					]
				]

				// The icon itself is the "open this mod in Content Browser" button.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(SButton)
					.ButtonStyle(&FCrimeFactoryStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("CrimeFactory.ModCardIconButton")))
					.ContentPadding(FMargin(0.0f))
					.OnClicked(
						this,
						&SCrimeFactoryModEntry::OnOpenModContent)
					.ToolTipText_Lambda(
						[this]()
						{
							return Mod.IsValid()
								? FText::FromString(
									FString::Printf(
										TEXT("Open /%s in the Content Browser"),
										*Mod->GetName()))
								: FText::GetEmpty();
						})
					[
						SNew(SBox)
						.WidthOverride(128.0f)
						.HeightOverride(128.0f)
						[
							SAssignNew(ModIconImage, SImage)
							.Image(ModIconBrush ? ModIconBrush.Get() : FAppStyle::GetBrush("Icons.Plugin"))
						]
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 1.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(
						LOCTEXT(
							"ModCardInformation",
							"Information"))
					.ButtonColorAndOpacity(
						this,
						&SCrimeFactoryModEntry::GetInformationButtonColor)
					.OnClicked(
						this,
						&SCrimeFactoryModEntry::OnOpenInformation)
					.ToolTipText(
						this,
						&SCrimeFactoryModEntry::GetInformationButtonTooltip)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 1.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(
						LOCTEXT(
							"ModCardConfig",
							"CONFIG"))
					.OnClicked_Lambda(
						[this]()
						{
							OnEditMod();
							return FReply::Handled();
						})
					.ToolTipText_Lambda(
						[this]()
						{
							return Mod.IsValid()
								? FText::FromString(
									FString::Printf(
										TEXT("Edit %s configuration"),
										*Mod->GetName()))
								: FText::GetEmpty();
						})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 1.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(
						LOCTEXT(
							"ModCardForge",
							"FORGE"))
					.OnClicked_Lambda(
						[this]()
						{
							PackageMod(
								TArray<
									TSharedPtr<
										SCrimeFactoryModEntry>>());

							return FReply::Handled();
						})
					.ToolTipText_Lambda(
						[this]()
						{
							return Mod.IsValid()
								? FText::FromString(
									FString::Printf(
										TEXT("Forge %s"),
										*Mod->GetName()))
								: FText::GetEmpty();
						})
				]
			]
		]
	];
}

FReply SCrimeFactoryModEntry::OnOpenModContent()
{
	if (!Mod.IsValid())
	{
		return FReply::Handled();
	}

	TArray<FString> FolderPaths;
	FolderPaths.Add(
		FString::Printf(
			TEXT("/%s"),
			*Mod->GetName()));

	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(
			TEXT("ContentBrowser"));

	ContentBrowserModule.Get().SyncBrowserToFolders(
		FolderPaths,
		true,
		true);

	return FReply::Handled();
}

FReply SCrimeFactoryModEntry::OnOpenInformation()
{
    if (!Mod.IsValid()) return FReply::Handled();
    OnRefreshPALInspector();
    const auto Window = SNew(SWindow).Title(FText::FromString(Mod->GetName() + TEXT(" - Mod Information")))
        .ClientSize(FVector2D(840, 740)).MinWidth(680).MinHeight(480).SupportsMaximize(true).SupportsMinimize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    const auto View = SNew(SCrimeFactoryModInspector).Data(PALInformation)
        .OnRefresh(FSimpleDelegate::CreateSPLambda(SharedThis(this), [this] { OnRefreshPALInspector(); }))
        .OnFix(FSimpleDelegate::CreateSPLambda(SharedThis(this), [this] { OnFixInformationIssues(); }))
        .OnViewAssets(FSimpleDelegate::CreateSPLambda(SharedThis(this), [this] { OnViewPALAssets(); }))
        .OnClose(FSimpleDelegate::CreateLambda([WeakWindow] { if (const auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow(); }));
    InspectorViews.Add(View);
    Window->SetContent(View);
    FSlateApplication::Get().AddWindow(Window);
    return FReply::Handled();
}

FText SCrimeFactoryModEntry::GetPALInspectorSummaryText() const
{
	if (!Mod.IsValid())
	{
		return LOCTEXT(
			"PALInspectorInvalidMod",
			"Mod is no longer valid.");
	}

	if (PALInspectorSummary.IsEmpty())
	{
		return LOCTEXT(
			"PALInspectorNotLoaded",
			"PAL inspector data has not been loaded yet.");
	}

	return PALInspectorSummary;
}

void SCrimeFactoryModEntry::RefreshPALInspectorState()
{
	if (!Mod.IsValid())
	{
		bInformationHasWarnings = true;
		bInformationHasAutoFixableWarnings = false;
		PALInspectorSummary =
			LOCTEXT(
				"PALInspectorInvalidModRefresh",
				"Mod is no longer valid.");
		return;
	}

	const FCrimeFactoryPALInspectionData Data =
		BuildPALInspectionData(*Mod);

	const int32 WarningCount =
		CountPALInspectionWarnings(Data);

	bInformationHasWarnings =
		WarningCount > 0;

	bInformationHasAutoFixableWarnings =
		HasAutoFixablePALInspectionWarnings(Data);

	FString Summary;

	if (!Data.bPALFound)
	{
		Summary =
			FString::Printf(
				TEXT(
					"Status: WARNING (%d issue%s)\n"
					"PAL: MISSING (%s)\n"
					"Plugin assets: %d\n"
					"Packaging: Single-stage fallback"
				),
				WarningCount,
				WarningCount == 1 ? TEXT("") : TEXT("s"),
				*Data.PALPackagePath,
				Data.PluginAssets.Num());
	}
	else
	{
		const int32 ExternalReferenceCount =
			Data.ExternalAssets.Num() +
			Data.ExternalBlueprints.Num();

		const FString ChunkText =
			Data.ChunkId >= 0
				? FString::FromInt(Data.ChunkId)
				: TEXT("Unset");

		const FString PackagingText =
			Data.bTwoStagePackaging
				? TEXT("Two-stage")
				: TEXT("Single-stage");

		const FString AssetManagerText =
			FString::Printf(
				TEXT(
					"Directory: %s | Specific PAL: %s"
				),
				Data.bAssetManagerDirectoryRegistered
					? TEXT("Registered")
					: TEXT("Missing"),
				Data.bPALSpecificAssetRegistered
					? TEXT("Registered")
					: TEXT("Missing"));

		const FString StatusText =
			WarningCount > 0
				? FString::Printf(
					TEXT("WARNING (%d issue%s)"),
					WarningCount,
					WarningCount == 1
						? TEXT("")
						: TEXT("s"))
				: TEXT("OK");

		Summary =
			FString::Printf(
				TEXT(
					"Status: %s\n"
					"PAL: %s\n"
					"Chunk: %s | Packaging: %s | Runtime Label: %s\n"
					"Directory Label: %s | Include Redirectors: %s\n"
					"Recursive: %s | Cook Rule: %s\n"
					"Plugin assets: %d | PAL chunk candidates: %d\n"
					"External refs: %d | /Game replacements: %d\n"
					"Asset Manager: %s"
				),
				*StatusText,
				*Data.PALPackagePath,
				*ChunkText,
				*PackagingText,
				Data.bRuntimeLabel ? TEXT("Yes") : TEXT("No"),
				Data.bLabelAssetsInMyDirectory ? TEXT("Yes") : TEXT("No"),
				Data.bIncludeRedirectors ? TEXT("Yes") : TEXT("No"),
				Data.bApplyRecursively ? TEXT("Yes") : TEXT("No"),
				Data.bAlwaysCook ? TEXT("Always Cook") : TEXT("Not Always Cook"),
				Data.PluginAssets.Num(),
				Data.ManagedChunkCandidates.Num(),
				ExternalReferenceCount,
				Data.ExternalGameAssets.Num(),
				*AssetManagerText);
	}

	AppendPALInspectionIssues(
		Data,
		Summary);

	PALInspectorSummary =
		FText::FromString(
			Summary);
    PALInformation = MakeInspectionPresentation(Data, Summary);
    InspectorViews.RemoveAll([](const auto& View) { return !View.IsValid(); });
    for (const auto& View : InspectorViews) if (const auto Pinned = View.Pin()) Pinned->SetData(PALInformation);

}

FReply SCrimeFactoryModEntry::OnRefreshPALInspector()
{
	RefreshPALInspectorState();
	return FReply::Handled();
}

FSlateColor SCrimeFactoryModEntry::GetInformationButtonColor() const
{
	return
		bInformationHasWarnings
			? FSlateColor(
				FLinearColor(
					1.0f,
					0.35f,
					0.03f,
					1.0f))
			: FSlateColor::UseForeground();
}

FText SCrimeFactoryModEntry::GetInformationButtonTooltip() const
{
	return
		bInformationHasWarnings
			? LOCTEXT(
				"ModCardInformationWarningTooltip",
				"CrimeFactory found one or more issues. Open Information for details and available automatic fixes.")
			: LOCTEXT(
				"ModCardInformationTooltip",
				"Show PAL, chunk, packaging mode, and managed asset information");
}

EVisibility SCrimeFactoryModEntry::GetFixInformationVisibility() const
{
	return
		bInformationHasWarnings
			? EVisibility::Visible
			: EVisibility::Collapsed;
}

bool SCrimeFactoryModEntry::CanAutoFixInformationIssues() const
{
	return bInformationHasAutoFixableWarnings;
}

FReply SCrimeFactoryModEntry::OnFixInformationIssues()
{
	if (!Mod.IsValid())
	{
		return FReply::Handled();
	}

	const FCrimeFactoryPALInspectionData Before =
		BuildPALInspectionData(*Mod);

	FString FixResult;

	const bool bFixed =
		CreateOrRepairCrimeFactoryPAL(
			*Mod,
			Before,
			FixResult);

	RefreshPALInspectorState();

	if (!bFixed)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(
				FString::Printf(
					TEXT(
						"CrimeFactory could not complete the automatic repair.\n\n%s"
					),
					*FixResult)));
	}
	else if (
		bInformationHasWarnings &&
		!bInformationHasAutoFixableWarnings)
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(
				FString::Printf(
					TEXT(
						"%s\n\nCrimeFactory fixed the safe configuration issues. "
						"One or more references still require manual repair; "
						"see Information for details."
					),
					*FixResult)));
	}

	return FReply::Handled();
}

FReply SCrimeFactoryModEntry::OnViewPALAssets()
{
    if (!Mod.IsValid()) return FReply::Handled();
    OnRefreshPALInspector();
    const auto Window = SNew(SWindow).Title(FText::FromString(Mod->GetName() + TEXT(" - Mod Assets")))
        .ClientSize(FVector2D(960, 760)).MinWidth(680).MinHeight(480).SupportsMaximize(true).SupportsMinimize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    const auto View = SNew(SCrimeFactoryModInspector).Data(PALInformation).ShowAssets(true)
        .OnRefresh(FSimpleDelegate::CreateSPLambda(SharedThis(this), [this] { OnRefreshPALInspector(); }))
        .OnFix(FSimpleDelegate::CreateSPLambda(SharedThis(this), [this] { OnFixInformationIssues(); }))
        .OnLocateAsset(TDelegate<bool(const FString&)>::CreateLambda([](const FString& Path)
        {
            // Generated class references resolve to their Blueprint asset without loading it.
            auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
            FAssetData Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
            if (!Asset.IsValid() && Path.EndsWith(TEXT("_C")))
                Asset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path.LeftChop(2)));
            if (!Asset.IsValid()) return false;
            FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{Asset});
            return true;
        }))
        .OnClose(FSimpleDelegate::CreateLambda([WeakWindow] { if (const auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow(); }));
    InspectorViews.Add(View);
    Window->SetContent(View);
    FSlateApplication::Get().AddWindow(Window);
    return FReply::Handled();
}

void SCrimeFactoryModEntry::OnEditMod()
{
	// Construct the plugin metadata object using the descriptor for this plugin
	UModMetadataObject* MetadataObject = NewObject<UModMetadataObject>();
	MetadataObject->TargetIconPath = Mod->GetBaseDir() / TEXT("Resources/Icon128.png");
	MetadataObject->PopulateFromDescriptor(Mod->GetDescriptor());
    CrimeFactoryMetadataSync::PopulateProperties(*Mod, *MetadataObject);
    MetadataObject->OnIconChanged = FSimpleDelegate::CreateSP(this, &SCrimeFactoryModEntry::RefreshModIcon);
	MetadataObject->AddToRoot();
    ON_SCOPE_EXIT { if (MetadataObject->IsRooted()) MetadataObject->RemoveFromRoot(); };

	// Create a property view
	FPropertyEditorModule& EditModule = FModuleManager::Get().GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	EditModule.RegisterCustomClassLayout(UModMetadataObject::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FModMetadataCustomization::MakeInstance));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bUpdatesFromSelection = false;
	DetailsViewArgs.bLockable = false;
	DetailsViewArgs.bAllowSearch = false;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::ActorsUseNameArea;
	DetailsViewArgs.bHideSelectionTip = true;

	TSharedRef<IDetailsView> PropertyView =
		EditModule.CreateDetailView(DetailsViewArgs);
	PropertyView->SetObject(MetadataObject, true);

	// Create the window
	PropertiesWindow = SNew(SWindow)
		.ClientSize(FVector2D(800.0f, 700.0f))
		.Title(LOCTEXT("ModMetadata", "Mod Properties"))
		.Content()
		[
			SNew(SBorder)
			.Padding(FMargin(8.0f, 8.0f))
			[
				SNew(SVerticalBox)

				+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(5.0f, 10.0f, 5.0f, 5.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Mod->GetName()))
				]

				+ SVerticalBox::Slot()
				.Padding(5)
				[
					PropertyView
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(5)
				.HAlign(HAlign_Right)
				[
					SNew(SButton)
					.ContentPadding(FMargin(20.0f, 2.0f))
					.Text(LOCTEXT("OkButtonLabel", "Ok"))
					.OnClicked(this, &SCrimeFactoryModEntry::OnEditModFinished, MetadataObject)
				]
			]
		];

	FSlateApplication::Get().AddModalWindow(PropertiesWindow.ToSharedRef(), Owner.Pin());
}

FReply SCrimeFactoryModEntry::OnEditModFinished(UModMetadataObject* MetadataObject)
{
	FPluginDescriptor OldDescriptor = Mod->GetDescriptor();

	// Update the descriptor with the new metadata
	FPluginDescriptor NewDescriptor = OldDescriptor;
	MetadataObject->CopyIntoDescriptor(NewDescriptor);

	// Close the properties window
	PropertiesWindow->RequestDestroyWindow();

	// Write both to strings
	FString OldText;
	OldDescriptor.Write(OldText);
	FString NewText;
	NewDescriptor.Write(NewText);
	if(OldText.Compare(NewText, ESearchCase::CaseSensitive) != 0)
	{
		FString DescriptorFileName = Mod->GetDescriptorFileName();

		// First attempt to check out the file if SCC is enabled
		ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
		if(SourceControlModule.IsEnabled())
		{
			ISourceControlProvider& SourceControlProvider = SourceControlModule.GetProvider();
			TSharedPtr<ISourceControlState, ESPMode::ThreadSafe> SourceControlState = SourceControlProvider.GetState(DescriptorFileName, EStateCacheUsage::ForceUpdate);
			if(SourceControlState.IsValid() && SourceControlState->CanCheckout())
			{
				SourceControlProvider.Execute(ISourceControlOperation::Create<FCheckOut>(), DescriptorFileName);
			}
		}

		// Write to the file and update the in-memory metadata
		FText FailReason;
		if(!Mod->UpdateDescriptor(NewDescriptor, FailReason))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FailReason);
		}
	}
    RefreshModIcon();
    FString SyncError;
    if (!CrimeFactoryMetadataSync::Synchronize(*Mod, true, SyncError))
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(SyncError));
	return FReply::Handled();
}

void SCrimeFactoryModEntry::RefreshModIcon()
{
    FString Filename = FPaths::ConvertRelativePathToFull(Mod->GetBaseDir() / TEXT("Resources/Icon128.png"));
    if (!FPaths::FileExists(Filename))
        if (const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("CrimeFactory")))
            Filename = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/CrimeFactory128.png"));
    auto Brush = CrimeFactoryModIcon::Load(Filename);
    if (!Brush)
        if (const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("CrimeFactory")))
            Brush = CrimeFactoryModIcon::Load(Plugin->GetBaseDir() / TEXT("Resources/CrimeFactory128.png"));
    // Update the widget before releasing the previous brush/resource.
    if (ModIconImage) ModIconImage->SetImage(Brush ? Brush.Get() : FAppStyle::GetBrush("Icons.Plugin"));
    ModIconBrush = MoveTemp(Brush);
    ModIconFilename = Filename;
    ModIconTimestamp = IFileManager::Get().GetTimeStamp(*Filename);
    ModIconFileSize = IFileManager::Get().FileSize(*Filename);
}

EActiveTimerReturnType SCrimeFactoryModEntry::PollModIcon(double Time, float Delta)
{
    const FString Filename = FPaths::ConvertRelativePathToFull(Mod->GetBaseDir() / TEXT("Resources/Icon128.png"));
    if ((FPaths::FileExists(Filename) && Filename != ModIconFilename)
        || (Filename == ModIconFilename && (IFileManager::Get().GetTimeStamp(*Filename) != ModIconTimestamp
            || IFileManager::Get().FileSize(*Filename) != ModIconFileSize))) RefreshModIcon();
    return EActiveTimerReturnType::Continue;
}

FString GetArgumentForLaunchType(
    ECrimeFactoryStartGameType LaunchMode)
{
    switch (LaunchMode)
    {
    case ECrimeFactoryStartGameType::STEAM:
        return TEXT("-LaunchGameType=Steam");

    case ECrimeFactoryStartGameType::MICROSOFT_STORE:
        return TEXT("-LaunchGameType=MicrosoftStore");

    case ECrimeFactoryStartGameType::EPIC_GAMES:
        return TEXT("-LaunchGameType=Epic");

    case ECrimeFactoryStartGameType::NONE:
    default:
        return TEXT("");
    }
}

FText GetCurrentPlatformName() {
#if PLATFORM_WINDOWS
    return LOCTEXT("PlatformName_Windows", "Windows");
#elif PLATFORM_MAC
    return LOCTEXT("PlatformName_Mac", "Mac");
#elif PLATFORM_LINUX
    return LOCTEXT("PlatformName_Linux", "Linux");
#else
    return LOCTEXT("PlatformName_Other", "Other OS");
#endif
}

FCrimeFactoryPreflightResult SCrimeFactoryModEntry::CollectPreflightResult() const
{
    FCrimeFactoryPreflightResult Result;
    if (!Mod.IsValid())
    {
        Result.Error(TEXT("The selected mod is no longer available. Refresh the mod list and try again."));
        return Result;
    }

	const FString PluginName = Mod->GetName();

	FString PluginMountPath = Mod->GetMountedAssetPath();
	PluginMountPath.RemoveFromEnd(TEXT("/"));

	const FString PluginContentPath =
		PluginMountPath + TEXT("/");

	const FString PALPackagePath =
		PluginContentPath +
		FString::Printf(TEXT("PAL_%s"), *PluginName);

	const FString PALObjectPathString =
		PALPackagePath +
		TEXT(".") +
		FString::Printf(TEXT("PAL_%s"), *PluginName);

	const FSoftObjectPath PALObjectPath(PALObjectPathString);


	// ------------------------------------------------------------------
	// Basic plugin/filesystem sanity.
	// ------------------------------------------------------------------
	if (IFileManager::Get().FileExists(*Mod->GetDescriptorFileName()))
	{
		Result.Pass(TEXT("Plugin descriptor exists."));
	}
	else
	{
		Result.Error(
			FString::Printf(
				TEXT("Plugin descriptor is missing: %s"),
				*Mod->GetDescriptorFileName()));
	}

	if (IFileManager::Get().DirectoryExists(*Mod->GetContentDir()))
	{
		Result.Pass(TEXT("Plugin Content directory exists."));
	}
	else
	{
		Result.Error(
			FString::Printf(
				TEXT("Plugin Content directory is missing: %s"),
				*Mod->GetContentDir()));
	}

	// ------------------------------------------------------------------
	// PDML dependency.
	// ------------------------------------------------------------------
	if (PluginName.Equals(TEXT("PDML"), ESearchCase::IgnoreCase))
	{
		Result.Pass(TEXT("PDML is the framework plugin; self-dependency is not required."));
	}
	else
	{
		const FPluginReferenceDescriptor* PDMLDependency =
			Mod->GetDescriptor().Plugins.FindByPredicate(
				[](const FPluginReferenceDescriptor& Reference)
				{
					return Reference.Name.Equals(
						TEXT("PDML"),
						ESearchCase::IgnoreCase);
				});

		if (PDMLDependency && PDMLDependency->bEnabled)
		{
			Result.Pass(TEXT("PDML dependency is present and enabled."));
		}
		else
		{
			Result.Error(
				TEXT(
					"PDML dependency is missing or disabled in the mod's .uplugin."));
		}
	}

	// ------------------------------------------------------------------
	// Asset Registry scan of the mod.
	// ------------------------------------------------------------------
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry"));

	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FString> PathsToScan;
	PathsToScan.Add(PluginMountPath);
	AssetRegistry.ScanPathsSynchronous(PathsToScan, true);

	TArray<FAssetData> ModAssets;
	AssetRegistry.GetAssetsByPath(
		FName(*PluginMountPath),
		ModAssets,
		true);

	if (ModAssets.Num() > 0)
	{
		Result.Pass(
			FString::Printf(
				TEXT("Asset Registry found %d asset(s) under %s."),
				ModAssets.Num(),
				*PluginMountPath));
	}
	else
	{
		Result.Warning(
			FString::Printf(
				TEXT("Asset Registry found no assets under %s."),
				*PluginMountPath));
	}

	// Detect template leftovers in both asset names and package dependencies.
	TSet<FName> MissingDependencyPackages;
	TSet<FName> PlaceholderDependencyPackages;

	for (const FAssetData& AssetData : ModAssets)
	{
		const FString AssetName = AssetData.AssetName.ToString();

		if (AssetName.Contains(
				TEXT("PLACEHOLDER_"),
				ESearchCase::IgnoreCase))
		{
			Result.Error(
				FString::Printf(
					TEXT("Template placeholder asset was not renamed: %s"),
					*AssetData.GetObjectPathString()));
		}

		const FString PackageName = AssetData.PackageName.ToString();

		if (!FPackageName::DoesPackageExist(PackageName))
		{
			Result.Error(
				FString::Printf(
					TEXT("Registered mod package is missing on disk: %s"),
					*PackageName));
		}

		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(
			AssetData.PackageName,
			Dependencies,
			UE::AssetRegistry::EDependencyCategory::Package);

		for (const FName DependencyName : Dependencies)
		{
			const FString DependencyString =
				DependencyName.ToString();

			// Only treat PLACEHOLDER_* dependencies inside this mod's own
			// plugin mount as stale CrimeFactory template leftovers.
			//
			// PAYDAY 3 legitimately contains base-game /Game assets whose
			// real names include PLACEHOLDER_ (for example the Wwise
			// PLACEHOLDER_UI_confirm / PLACEHOLDER_UI_highlight events).
			// Those are valid external dependencies and must not block forge.
			const bool bDependencyIsInsideThisPlugin =
				DependencyString.StartsWith(
					PluginContentPath,
					ESearchCase::IgnoreCase);

			if (bDependencyIsInsideThisPlugin &&
				DependencyString.Contains(
					TEXT("PLACEHOLDER_"),
					ESearchCase::IgnoreCase))
			{
				PlaceholderDependencyPackages.Add(
					DependencyName);
			}
		}
	}

	if (PlaceholderDependencyPackages.Num() == 0)
	{
		Result.Pass(TEXT("No stale PLACEHOLDER_* package references were found inside the mod plugin."));
	}
	else
	{
		for (const FName DependencyName : PlaceholderDependencyPackages)
		{
			Result.Error(
				FString::Printf(
					TEXT("Stale template dependency found: %s"),
					*DependencyName.ToString()));
		}
	}

	// ------------------------------------------------------------------
	// PAL.
	// ------------------------------------------------------------------
	UPrimaryAssetLabel* Label =
		Cast<UPrimaryAssetLabel>(
			UEditorAssetLibrary::LoadAsset(PALPackagePath));

	if (!Label)
	{
		Result.Error(
			FString::Printf(
				TEXT("Required Primary Asset Label is missing: %s"),
				*PALPackagePath));
	}
	else
	{
		Result.Pass(
			FString::Printf(
				TEXT("Primary Asset Label found: %s"),
				*PALPackagePath));

		if (Label->bIsRuntimeLabel)
		{
			Result.Pass(TEXT("PAL is marked as a runtime label."));
		}
		else
		{
			Result.Warning(TEXT("PAL is not marked as a runtime label."));
		}

        const auto Resolved = ResolvePALReferences(*Label);
        for (const FString& Error : Resolved.Errors) Result.Error(Error);
        for (const FString& Warning : Resolved.Warnings) Result.Warning(Warning);
		int32 ExternalAssetCount = 0;
		int32 ExternalBlueprintCount = 0;
		int32 MissingExplicitReferenceCount = 0;

		for (const FSoftObjectPath& ExplicitAsset : Resolved.Assets)
		{
			const FSoftObjectPath AssetPath =
				ExplicitAsset;

			if (AssetPath.IsNull())
			{
				Result.Warning(TEXT("PAL contains an empty Explicit Asset entry."));
				continue;
			}

			if (!DoesSoftReferencePackageExist(AssetPath))
			{
				++MissingExplicitReferenceCount;

				Result.Error(
					FString::Printf(
						TEXT("PAL Explicit Asset package does not exist: %s"),
						*AssetPath.ToString()));
			}

			if (IsOutsidePluginMount(
					AssetPath,
					PluginContentPath))
			{
				++ExternalAssetCount;
			}
		}

		for (const FSoftObjectPath& ExplicitBlueprint : Resolved.Blueprints)
		{
			const FSoftObjectPath AssetPath =
				ExplicitBlueprint;

			if (AssetPath.IsNull())
			{
				Result.Warning(TEXT("PAL contains an empty Explicit Blueprint entry."));
				continue;
			}

			if (!DoesSoftReferencePackageExist(AssetPath))
			{
				++MissingExplicitReferenceCount;

				Result.Error(
					FString::Printf(
						TEXT("PAL Explicit Blueprint package does not exist: %s"),
						*AssetPath.ToString()));
			}

			if (IsOutsidePluginMount(
					AssetPath,
					PluginContentPath))
			{
				++ExternalBlueprintCount;
			}
		}

		if (MissingExplicitReferenceCount == 0)
		{
			Result.Pass(TEXT("All PAL explicit references resolve to packages on disk."));
		}

		const int32 TotalExternalReferences =
			ExternalAssetCount + ExternalBlueprintCount;

		if (TotalExternalReferences > 0)
		{
			Result.Pass(
				FString::Printf(
					TEXT(
						"Detected %d external replacement reference(s) "
						"(%d asset(s), %d blueprint(s)); two-stage packaging is required."
					),
					TotalExternalReferences,
					ExternalAssetCount,
					ExternalBlueprintCount));

			if (Label->Rules.ChunkId >= 0)
			{
				Result.Pass(
					FString::Printf(
						TEXT("PAL has valid Chunk ID %d."),
						Label->Rules.ChunkId));
			}
			else
			{
				Result.Error(
					TEXT(
						"PAL manages external content but its Chunk ID is invalid (< 0)."));
			}
		}
		else
		{
			Result.Pass(
				TEXT(
					"No external /Game-style replacement references were detected; "
					"single-stage plugin packaging is sufficient."));

			if (Label->Rules.ChunkId < 0)
			{
				Result.Warning(
					TEXT(
						"PAL Chunk ID is unset. This is fine for plugin-only content, "
						"but external replacement assets would require a valid Chunk ID."));
			}
		}
	}

	// ------------------------------------------------------------------
	// Asset Manager registration.
	// ------------------------------------------------------------------
	const UAssetManagerSettings* AssetManagerSettings =
		GetDefault<UAssetManagerSettings>();

	const FPrimaryAssetTypeInfo* PrimaryAssetLabelType =
		AssetManagerSettings
			? AssetManagerSettings->PrimaryAssetTypesToScan.FindByPredicate(
				[](const FPrimaryAssetTypeInfo& TypeInfo)
				{
					return TypeInfo.PrimaryAssetType ==
						FName(TEXT("PrimaryAssetLabel"));
				})
			: nullptr;

	if (!PrimaryAssetLabelType)
	{
		Result.Error(
			TEXT(
				"Asset Manager has no PrimaryAssetLabel entry in Primary Asset Types to Scan."));
	}
	else
	{
		if (IsMountRegisteredAsPALDirectory(
				*PrimaryAssetLabelType,
				PluginMountPath))
		{
			Result.Pass(
				FString::Printf(
					TEXT("Asset Manager Directories contains %s."),
					*PluginMountPath));
		}
		else
		{
			Result.Error(
				FString::Printf(
					TEXT("Asset Manager Directories is missing %s."),
					*PluginMountPath));
		}

		if (IsPALRegisteredAsSpecificAsset(
				*PrimaryAssetLabelType,
				PALObjectPath))
		{
			Result.Pass(TEXT("PAL is registered as an Asset Manager Specific Asset."));
		}
		else
		{
			Result.Warning(
				TEXT(
					"PAL is not listed as an Asset Manager Specific Asset. "
					"The directory scan can still discover it, but CrimeFactory normally registers both."));
		}
	}

	// ------------------------------------------------------------------
	// Present results and decide whether packaging may continue.
	// ------------------------------------------------------------------
	FString Report = Result.BuildReport(PluginName);

	UE_LOG(
		LogCrimeFactory,
		Display,
		TEXT("%s"),
		*Report);

	return Result;
}


bool SCrimeFactoryModEntry::RunPreflightValidation() const
{
    return CrimeFactoryPackagingUI::ConfirmPreflight(Mod.IsValid() ? Mod->GetName() : TEXT("Unavailable mod"), CollectPreflightResult());
}

void SCrimeFactoryModEntry::PackageMod(const TArray<TSharedPtr<SCrimeFactoryModEntry>>& NextEntries) const
{
    if (GCrimeFactoryForgeActive.Exchange(true))
    {
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ForgeAlreadyActive", "CrimeFactory is already forging a mod batch. Wait for it to finish before starting another."));
        return;
    }
    const auto Batch = MakeShared<FCrimeFactoryForgeBatch>();
    Batch->Entries.Add(SharedThis(this));
    TArray<TSharedPtr<SCrimeFactoryModEntry>> Queue;
    for (const auto& Entry : NextEntries)
        if (Entry.IsValid()) { Batch->Entries.Add(Entry); Queue.Add(Entry); }
    FCrimeFactoryPreflightResult Combined;
    for (const auto& Entry : Batch->Entries)
    {
        FString MetadataError;
        if (Entry->Mod.IsValid() && !CrimeFactoryMetadataSync::Synchronize(*Entry->Mod, true, MetadataError))
            Combined.Error(Entry->Mod->GetName() + TEXT(" - ") + MetadataError);
        if (Entry->Mod.IsValid())
            FModuleManager::LoadModuleChecked<FCrimeFactoryModule>(TEXT("CrimeFactory")).EnsureAssetManagerRegistrationForPlugin(*Entry->Mod);
        const auto Result = Entry->CollectPreflightResult();
        const FString Name = Entry->Mod.IsValid() ? Entry->Mod->GetName() : TEXT("Unavailable mod");
        for (const auto& Item : Result.Items)
            Combined.Add(Item.Severity, Batch->Entries.Num() > 1 ? Name + TEXT(" - ") + Item.Message : Item.Message);
    }
    const UCrimeFactorySettings* ArchiveSettings = UCrimeFactorySettings::Get();
    if (ArchiveSettings->bCreateUtilityArchive)
    {
        const bool bSevenZip = ArchiveSettings->ArchiveUtility == ECrimeFactoryArchiveUtility::SevenZip;
        const FString Executable = (bSevenZip ? ArchiveSettings->SevenZipExecutablePath : ArchiveSettings->WinRARExecutablePath).FilePath.TrimStartAndEnd();
        if (Executable.IsEmpty() || Executable.Contains(TEXT("\"")) || Executable.Contains(TEXT("\r")) || Executable.Contains(TEXT("\n"))
            || !FPaths::GetExtension(Executable).Equals(TEXT("exe"), ESearchCase::IgnoreCase)
            || !IFileManager::Get().FileExists(*Executable))
            Combined.Error(FString::Printf(TEXT("%s executable is missing or invalid: %s. Configure its Executable Path in CrimeFactory Project Settings."),
                bSevenZip ? TEXT("7-Zip") : TEXT("WinRAR"), *Executable));
    }
    const FString Export = UCrimeFactorySettings::Get()->ZipExportDirectory.Path.TrimStartAndEnd();
    if (!Export.IsEmpty())
    {
        if (Export.Contains(TEXT("\"")) || Export.Contains(TEXT("\r")) || Export.Contains(TEXT("\n")))
            Combined.Error(TEXT("The ZIP export folder contains an invalid quote or line break. Choose a valid folder in CrimeFactory settings."));
        else
        {
            Batch->ZipExportDirectory = FPaths::IsRelative(Export) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Export) : Export;
            FPaths::NormalizeDirectoryName(Batch->ZipExportDirectory);
            const FString Probe = FPaths::Combine(Batch->ZipExportDirectory, TEXT(".CrimeFactoryWriteTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
            if (!IFileManager::Get().MakeDirectory(*Batch->ZipExportDirectory, true) || !FFileHelper::SaveStringToFile(TEXT(""), *Probe))
                Combined.Error(TEXT("CrimeFactory cannot write to the ZIP export folder: ") + Batch->ZipExportDirectory);
            else IFileManager::Get().Delete(*Probe);
        }
    }
    const FString Title = Batch->Entries.Num() == 1 && Mod.IsValid() ? Mod->GetName() : FString::Printf(TEXT("%d selected mods"), Batch->Entries.Num());
    if (!CrimeFactoryPackagingUI::ConfirmPreflight(Title, Combined)) return;
    for (const auto& Entry : Batch->Entries) Batch->ApprovedEntries.Add(Entry.Get());
    PackageMod(Queue, Batch);
}


void SCrimeFactoryModEntry::PackageMod(const TArray<TSharedPtr<SCrimeFactoryModEntry>>& NextEntries,
    TSharedRef<FCrimeFactoryForgeBatch> Batch) const {
    bool bTaskScheduled = false;
    ON_SCOPE_EXIT { if (!bTaskScheduled) Batch->Finish(); };
    if (!Mod.IsValid() || !Batch->ApprovedEntries.Contains(this)) return;
    UCrimeFactorySettings* Settings = UCrimeFactorySettings::Get();
    const FString PluginName = Mod->GetName();
    const FString GamePath = Settings->PD3GamePath.Path;

    // Make the game path persistence independent of when the details view
    // decides that editing has finished.
    Settings->SaveSettings();

    const FString ProjectPath = FPaths::IsProjectFilePathSet()
        ? FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
        : FPaths::RootDir() / FApp::GetProjectName() / FApp::GetProjectName() + TEXT(".uproject");

    const FString CrimeFactoryAutomationScriptDir =
        ResolveCrimeFactoryAutomationScriptDir();

    if (!EnsureCrimeFactoryAutomationReady(
            ProjectPath,
            CrimeFactoryAutomationScriptDir))
    {
        return;
    }

    if (!EnsureUnrealPakReady())
    {
        return;
    }

    FString AdditionalUATArguments;
    if (Settings->bCreateUtilityArchive)
    {
        const bool bSevenZip = Settings->ArchiveUtility == ECrimeFactoryArchiveUtility::SevenZip;
        const FString Executable = (bSevenZip ? Settings->SevenZipExecutablePath : Settings->WinRARExecutablePath).FilePath.TrimStartAndEnd();
        // Validate again: settings may have changed while the batch was cooking.
        if (Executable.IsEmpty() || Executable.Contains(TEXT("\"")) || Executable.Contains(TEXT("\r")) || Executable.Contains(TEXT("\n"))
            || !FPaths::GetExtension(Executable).Equals(TEXT("exe"), ESearchCase::IgnoreCase)
            || !IFileManager::Get().FileExists(*Executable))
        {
            FCrimeFactoryPackagingFailure Failure;
            Failure.PluginName = PluginName;
            Failure.Phase = TEXT("Archive configuration");
            Failure.Message = FString::Printf(TEXT("%s executable is missing or invalid: %s"), bSevenZip ? TEXT("7-Zip") : TEXT("WinRAR"), *Executable);
            Failure.Suggestion = TEXT("Configure the selected utility's Executable Path in CrimeFactory Project Settings.");
            CrimeFactoryPackagingUI::ShowFailure(Failure);
            return;
        }
        AdditionalUATArguments += FString::Printf(TEXT("-CreateUtilityArchive -ArchiveUtility=%s -ArchiveExecutable=\"%s\" "),
            bSevenZip ? TEXT("SevenZip") : TEXT("WinRAR"), *FPaths::ConvertRelativePathToFull(Executable));
    }

    if (!Batch->ZipExportDirectory.IsEmpty())
        AdditionalUATArguments += FString::Printf(TEXT("-ZipExportDir=\"%s\" "), *Batch->ZipExportDirectory);

    if (Settings->bCopyModsToGame) {
        AdditionalUATArguments.Append(TEXT("-CopyToGameDir "));
    }
    if (Settings->LaunchGameAfterPacking != ECrimeFactoryStartGameType::NONE && NextEntries.Num() == 0) {
        AdditionalUATArguments.Append(TEXT("-LaunchGame "));
        AdditionalUATArguments.Append(GetArgumentForLaunchType(Settings->LaunchGameAfterPacking)).Append(TEXT(" "));
    }

    /*
     * Repair/ensure Asset Manager registration immediately before UAT starts.
     * This also fixes already-created mods: /<ModName> is persisted into the
     * PrimaryAssetLabel Directories list and the PAL is added to SpecificAssets.
     */
    FCrimeFactoryModule& CrimeFactoryModule =
        FModuleManager::LoadModuleChecked<FCrimeFactoryModule>(TEXT("CrimeFactory"));

    CrimeFactoryModule.EnsureAssetManagerRegistrationForPlugin(*Mod);

    // Revalidate without another prompt in case assets changed while earlier mods cooked.
    FString MetadataError;
    if (!CrimeFactoryMetadataSync::Synchronize(*Mod, true, MetadataError))
    {
        FCrimeFactoryPackagingFailure Failure;
        Failure.PluginName = PluginName;
        Failure.Phase = TEXT("Metadata synchronization");
        Failure.Message = MetadataError;
        Failure.Suggestion = TEXT("Repair and save the mod properties and metadata DA, then forge again.");
        CrimeFactoryPackagingUI::ShowFailure(Failure);
        return;
    }
    const auto CurrentPreflight = CollectPreflightResult();
    if (CurrentPreflight.HasErrors())
    {
        FCrimeFactoryPackagingFailure Failure;
        Failure.PluginName = PluginName;
        Failure.Phase = TEXT("Preflight revalidation");
        Failure.Message = TEXT("This mod changed after the batch was approved and now has blocking errors.");
        Failure.Details = CurrentPreflight.BuildReport(PluginName);
        Failure.Suggestion = TEXT("Fix the reported issues and restart Forge Selected! for the remaining mods.");
        Failure.RemainingModCount = NextEntries.Num();
        CrimeFactoryPackagingUI::ShowFailure(Failure);
        return;
    }

    // Only request PASS 2 when the PAL explicitly manages content outside
    // the plugin mount. Mods containing only plugin assets stay on the fast
    // original single-stage DLC path.
    const int32 ChunkId = GetModChunkIdIfExternalContentExists(*Mod);

    // UAT runs with a different working directory from the editor. Resolve
    // this directory here so the manifest, result JSON, and log use the same
    // absolute paths in both processes.
    const FString ResultDirectory =
        FPaths::ConvertRelativePathToFull(
            FPaths::Combine(
                FPaths::ProjectSavedDir(),
                TEXT("CrimeFactory"),
                TEXT("Results")));

    IFileManager::Get().MakeDirectory(
        *ResultDirectory,
        true);

    if (ChunkId >= 0)
    {
        AdditionalUATArguments.Append(
            FString::Printf(TEXT("-ChunkId=%d "), ChunkId)
        );

        const FString PALPackagingManifestFile =
            FPaths::Combine(
                ResultDirectory,
                FPaths::MakeValidFileName(
                    PluginName +
                    TEXT("_PALPackaging.json")));

        IFileManager::Get().Delete(
            *PALPackagingManifestFile,
            false,
            true,
            true);

        if (!WritePALPackagingManifest(
                *Mod,
                ChunkId,
                PALPackagingManifestFile))
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::FromString(
                    FString::Printf(
                        TEXT(
                            "CrimeFactory could not build the deterministic PAL "
                            "package list for '%s'. Packaging has been stopped."
                        ),
                        *PluginName)));

            return;
        }

        AdditionalUATArguments.Append(
            FString::Printf(
                TEXT("-CrimeFactoryPALManifestFile=\"%s\" "),
                *PALPackagingManifestFile));
    }

    UE_LOG(LogCrimeFactory, Display, TEXT("Packaging plugin \"%s\". %d remaining"), *PluginName, NextEntries.Num());

    const FString ResultFile =
        FPaths::Combine(
            ResultDirectory,
            FPaths::MakeValidFileName(
                PluginName + TEXT("_PackageResult.json")));

    // Never let a stale result from a previous packaging run be displayed.
    IFileManager::Get().Delete(
        *ResultFile,
        false,
        true,
        true);

    AdditionalUATArguments.Append(
        FString::Printf(
            TEXT("-CrimeFactoryResultFile=\"%s\" "),
            *ResultFile));

    const FString UATLogFile =
        FPaths::Combine(
            ResultDirectory,
            FPaths::MakeValidFileName(
                PluginName + TEXT("_UAT.log")));

    // Ensure the failure window can never show a stale log from an older run.
    IFileManager::Get().Delete(
        *UATLogFile,
        false,
        true,
        true);

    /*
     * Give AutomationTool an explicit log path that belongs to THIS mod run.
     * This is much more reliable than trying to infer the correct file after a
     * very-early script/command failure.
     */
    AdditionalUATArguments.Append(
        FString::Printf(
            TEXT("-log=\"%s\" "),
            *UATLogFile));

    /*
     * PackagePlugin lives in CrimeFactory.Automation. The one-time bootstrap
     * above compiles/registers it for this engine; CreateUatTask remains the
     * normal packaging path and can safely use its automatic -nocompile flags.
     */
    UE_LOG(
        LogCrimeFactory,
        Display,
        TEXT("Using CrimeFactory Automation ScriptDir: %s"),
        *CrimeFactoryAutomationScriptDir);

    const FString CommandLine = FString::Printf(
        TEXT("-ScriptsForProject=\"%s\" -ScriptDir=\"%s\" PackagePlugin -Project=\"%s\" -PluginName=\"%s\" -GameDir=\"%s\" %s"),
        *ProjectPath,
        *CrimeFactoryAutomationScriptDir,
        *ProjectPath,
        *PluginName,
        *GamePath,
        *AdditionalUATArguments
    );

    const FText PlatformName = GetCurrentPlatformName();
    FString CloseError;
    if (!CrimeFactoryGameProcess::CloseBeforeForge(CloseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(CloseError));
        return;
    }

    bTaskScheduled = true;
    IUATHelperModule::Get().CreateUatTask(
        CommandLine,
        PlatformName,
        LOCTEXT("PackageModTaskName", "Packaging Mod"),
        LOCTEXT("PackageModTaskShortName", "Package Mod Task"),
        FCrimeFactoryStyle::Get().GetBrush("CrimeFactory.OpenPluginWindow"),
        nullptr,
        [PluginName, ResultFile, UATLogFile, NextEntries, Batch](
            FString resultType,
            double runTime)
        {
            AsyncTask(
                ENamedThreads::GameThread,
                [PluginName,
                 ResultFile,
                 UATLogFile,
                 NextEntries,
                 Batch,
                 resultType,
                 runTime]()
                {
                    const FCrimeFactoryUATResult ParsedResult =
                        LoadCrimeFactoryUATResult(ResultFile);

                    const bool bFailed =
                        ParsedResult.bLoaded
                            ? !ParsedResult.bSuccess
                            : DoesResultTypeIndicateFailure(
                                resultType);

                    if (bFailed)
                    {
                        Batch->Finish();
                        ShowPackagingFailureWindow(
                            PluginName,
                            resultType,
                            runTime,
                            ResultFile,
                            UATLogFile,
                            NextEntries.Num());

                        return;
                    }

                    // Continue a selected-mod batch only after a successful
                    // package. A failure stops the batch so later results do
                    // not bury the actual first error.
                    if (NextEntries.Num() > 0)
                    {
                        TSharedPtr<SCrimeFactoryModEntry> NextMod =
                            NextEntries[0];

                        TArray<TSharedPtr<SCrimeFactoryModEntry>>
                            RemainingEntries =
                                NextEntries.FilterByPredicate(
                                    [NextMod](
                                        const TSharedPtr<
                                            SCrimeFactoryModEntry>& X)
                                    {
                                        return X != NextMod;
                                    });

                        NextMod->PackageMod(
                            RemainingEntries, Batch);
                    }
                    else Batch->Finish();
                });
        }
    );
}

FReply SCrimeFactoryModEntry::OnDeleteMod()
{
    if (!Mod.IsValid())
    {
        return FReply::Handled();
    }

    const FString PluginName = Mod->GetName();
    const FString PluginMountPath = Mod->GetMountedAssetPath();
    const FString DescriptorFileName = Mod->GetDescriptorFileName();

    // Capture the mount's physical content directory before deleting it.
    FString PluginContentDir =
        FPaths::ConvertRelativePathToFull(Mod->GetContentDir());
    FPaths::NormalizeDirectoryName(PluginContentDir);

    FString ModBaseDir =
        FPaths::ConvertRelativePathToFull(Mod->GetBaseDir());

    FString ProjectModsDir =
        FPaths::ConvertRelativePathToFull(
            FPaths::Combine(
                FPaths::ProjectDir(),
                TEXT("Mods")));

    FPaths::NormalizeDirectoryName(ModBaseDir);
    FPaths::NormalizeDirectoryName(ProjectModsDir);

    const FString ProjectModsPrefix =
        ProjectModsDir.EndsWith(TEXT("/"))
            ? ProjectModsDir
            : ProjectModsDir + TEXT("/");

    /*
     * Never allow this button to recursively delete anything outside the
     * project's Mods directory, even if an unexpected plugin descriptor is
     * classified as EPluginType::Mod.
     */
    if (!ModBaseDir.StartsWith(
            ProjectModsPrefix,
            ESearchCase::IgnoreCase) ||
        ModBaseDir.Len() <= ProjectModsPrefix.Len())
    {
        const FText ErrorText = FText::FromString(
            FString::Printf(
                TEXT(
                    "CrimeFactory refused to delete '%s' because its directory is "
                    "outside the project's Mods folder.\n\n%s"
                ),
                *PluginName,
                *ModBaseDir));

        FMessageDialog::Open(
            EAppMsgType::Ok,
            ErrorText);

        return FReply::Handled();
    }

    const FText ConfirmationText = FText::FromString(
        FString::Printf(
            TEXT(
                "Permanently delete mod '%s'?\n\n"
                "This will permanently delete the entire mod folder:\n"
                "%s\n\n"
                "CrimeFactory will also remove the mod from the Asset Manager "
                "PrimaryAssetLabel directories and specific assets.\n\n"
                "THIS CANNOT BE UNDONE.\n\n"
                "Are you sure you want to continue?"
            ),
            *PluginName,
            *ModBaseDir));

    if (FMessageDialog::Open(
            EAppMsgType::YesNo,
            ConfirmationText) != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    /*
     * Delete the folder first. If Windows has a loaded DLL or another file
     * locked, leave the Asset Manager configuration untouched and report the
     * failure instead of creating a half-deleted registration state.
     */
    if (!IFileManager::Get().DeleteDirectory(
            *ModBaseDir,
            false, // RequireExists
            true)) // Tree
    {
        const FText ErrorText = FText::FromString(
            FString::Printf(
                TEXT(
                    "Failed to delete mod '%s'.\n\n"
                    "The directory could not be removed:\n%s\n\n"
                    "A file may still be in use. The mod has NOT been removed "
                    "from the Asset Manager."
                ),
                *PluginName,
                *ModBaseDir));

        FMessageDialog::Open(
            EAppMsgType::Ok,
            ErrorText);

        return FReply::Handled();
    }

    /*
     * Refresh the Asset Registry against the now-deleted directory while the
     * package mount still exists. A force rescan drops the old on-disk asset
     * entries and finds nothing to replace them with.
     */
    FString RegistryMountPath = PluginMountPath;
    RegistryMountPath.RemoveFromEnd(TEXT("/"));

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry"));

    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> DeletedScanPaths;
    DeletedScanPaths.Add(RegistryMountPath);

    AssetRegistry.ScanPathsSynchronous(
        DeletedScanPaths,
        true // bForceRescan
    );

    /*
     * Remove any now-empty cached subpaths as well. Do deepest paths first so
     * parents can be removed cleanly afterwards.
     */
    TArray<FString> CachedSubPaths;
    AssetRegistry.GetSubPaths(
        RegistryMountPath,
        CachedSubPaths,
        true);

    CachedSubPaths.Sort(
        [](const FString& A, const FString& B)
        {
            return A.Len() > B.Len();
        });

    for (const FString& CachedSubPath : CachedSubPaths)
    {
        AssetRegistry.RemovePath(CachedSubPath);
    }

    AssetRegistry.RemovePath(RegistryMountPath);

    // Drop the saved and live Asset Manager references while the mount is
    // still valid, before the cook configuration can reference an unmounted root.
    FCrimeFactoryModule& CrimeFactoryModule =
        FModuleManager::LoadModuleChecked<FCrimeFactoryModule>(TEXT("CrimeFactory"));
    CrimeFactoryModule.RemoveAssetManagerRegistrationForPlugin(PluginName, PluginMountPath);

    /*
     * Remove the package mount itself. Content Browser listens to mount/path
     * changes, so this removes the deleted mod root from the Sources panel
     * instead of leaving an empty /<ModName> folder until editor restart.
     */
    FString RootMountPath = RegistryMountPath;
    if (!RootMountPath.EndsWith(TEXT("/")))
    {
        RootMountPath += TEXT("/");
    }

    FPackageName::UnRegisterMountPoint(
        RootMountPath,
        PluginContentDir);

    // Default-enabled mods normally have no .uproject entry. Remove an
    // explicit override if one exists, so it cannot require this deleted mod
    // when a separate cook/UAT process or a later editor session starts.
    IProjectManager& ProjectManager = IProjectManager::Get();
    const FProjectDescriptor* Project = ProjectManager.GetCurrentProject();
    if (Project && Project->Plugins.ContainsByPredicate(
        [&PluginName](const FPluginReferenceDescriptor& Reference)
        {
            return Reference.Name.Equals(PluginName, ESearchCase::IgnoreCase);
        }))
    {
        FText ProjectSaveFailure;
        if (!ProjectManager.RemovePluginReference(PluginName, ProjectSaveFailure) ||
            !ProjectManager.SaveCurrentProjectToDisk(ProjectSaveFailure))
        {
            UE_LOG(LogCrimeFactory, Error,
                TEXT("Deleted mod %s, but could not save removal of its project plugin reference: %s. Make the project descriptor writable and save the project before packaging."),
                *PluginName, *ProjectSaveFailure.ToString());
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
                TEXT("Mod '%s' was deleted, but its project plugin reference could not be saved. Make the project descriptor writable and save the project before packaging.\n\n%s"),
                *PluginName, *ProjectSaveFailure.ToString())));
        }
    }

    // Remove the saved checkbox selection for the deleted mod.
    if (UCrimeFactorySettings* Settings = UCrimeFactorySettings::Get())
    {
        Settings->ModSelection.Remove(PluginName);
        Settings->SaveSettings();
    }

    /*
     * Remove the deleted descriptor from PluginManager's discovered list and
     * refresh discovery. UE refuses removal of enabled plugins, and refresh
     * intentionally retains them. Do not force-unload assets/modules here.
     * Asset Manager registration is already gone; packaging independently
     * validates loaded receipt dependencies before UAT staging.
     */
    FText RemovePluginFailReason;
    if (!IPluginManager::Get().RemoveFromPluginsList(
            DescriptorFileName,
            &RemovePluginFailReason))
    {
        UE_LOG(
            LogCrimeFactory,
            Warning,
            TEXT(
                "Mod %s was deleted from disk, but PluginManager could not "
                "remove its descriptor from the current in-memory list: %s. "
                "CrimeFactory will validate receipt references before staging; no restart is required for that cleanup."
            ),
            *PluginName,
            *RemovePluginFailReason.ToString());
    }

    IPluginManager::Get().RefreshPluginsList();

    UE_LOG(
        LogCrimeFactory,
        Display,
        TEXT("Permanently deleted mod %s from %s"),
        *PluginName,
        *ModBaseDir);

    /*
     * Refresh the list after this Slate click handler returns, so this widget
     * is not destroyed while one of its own member functions is executing.
     */
    TWeakPtr<SCrimeFactoryModEntryList> WeakOwner = Owner;

    AsyncTask(
        ENamedThreads::GameThread,
        [WeakOwner]()
        {
            if (TSharedPtr<SCrimeFactoryModEntryList> PinnedOwner =
                    WeakOwner.Pin())
            {
                PinnedOwner->LoadMods();
            }
        });

    return FReply::Handled();
}

void SCrimeFactoryModEntry::OnEnableCheckboxChanged(ECheckBoxState NewState) {
    // Save new checked state for the mod at the
    UCrimeFactorySettings* Settings = UCrimeFactorySettings::Get();
    const FString PluginName = Mod->GetName();

    Settings->ModSelection.Add(PluginName, NewState == ECheckBoxState::Checked);

    Settings->SaveSettings();
}

#undef LOCTEXT_NAMESPACE

UCrimeFactoryPALCommandlet::UCrimeFactoryPALCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UCrimeFactoryPALCommandlet::Main(const FString& Params)
{
	if (FParse::Param(*Params, TEXT("CheckClassRedirects")))
	{
		const struct { const TCHAR* Path; UClass* Expected; } Cases[] = {
			{TEXT("/Script/CrimeForge.CrimeForgeSettings"), UCrimeFactorySettings::StaticClass()},
			{TEXT("/Script/CrimeForge.ModMetadataObject"), UModMetadataObject::StaticClass()}
		};
		for (const auto& Case : Cases)
		{
			if (FSoftObjectPath(Case.Path).TryLoad() != Case.Expected)
			{
				UE_LOG(LogCrimeFactory, Error, TEXT("CrimeFactory class redirect failed: %s -> %s."),
					Case.Path, *Case.Expected->GetPathName());
				return 1;
			}
		}
		UE_LOG(LogCrimeFactory, Display, TEXT("CrimeFactory legacy class redirects validated through soft reference loading."));
	}
	FString PluginName, Output;
	int32 ChunkId = INDEX_NONE;
	if (!FParse::Value(*Params, TEXT("PluginName="), PluginName)
		|| !FParse::Value(*Params, TEXT("Output="), Output)
		|| !FParse::Value(*Params, TEXT("ChunkId="), ChunkId))
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("Usage: -run=CrimeFactoryPAL -PluginName=<mod> -ChunkId=<id> -Output=<manifest.json>"));
		return 1;
	}
	const auto Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin)
	{
		UE_LOG(LogCrimeFactory, Error, TEXT("CrimeFactory PAL validation: Plugin %s was not discovered."), *PluginName);
		return 1;
	}
	return WritePALPackagingManifest(*Plugin, ChunkId, Output) ? 0 : 1;
}

