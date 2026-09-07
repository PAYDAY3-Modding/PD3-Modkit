#include "CrimeForgeModEntry.h"
#include "CrimeForge.h"
#include "CrimeForgeSettings.h"
#include "CrimeForgeStyle.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlOperation.h"
#include "ModMetadataObject.h"
#include "SourceControlOperations.h"
#include "Async/Async.h"
#include "IUATHelperModule.h"
#include "Engine/PrimaryAssetLabel.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManagerSettings.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
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
#include "Brushes/SlateDynamicImageBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "CrimeForgeModListEntry"

namespace
{
	struct FCrimeForgeUATResult
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

	FCrimeForgeUATResult LoadCrimeForgeUATResult(
		const FString& ResultFile)
	{
		FCrimeForgeUATResult Result;

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
			LogCrimeForge,
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
		const FCrimeForgeUATResult& Result)
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
		const FCrimeForgeUATResult& Result)
	{
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
		const FCrimeForgeUATResult& Result)
	{
		if (Category == TEXT("Automation script compile"))
		{
			return TEXT(
				"AutomationTool failed while compiling/loading CrimeForge's C# "
				"automation script, before PackagePlugin.ExecuteBuild() could run. "
				"The compiler error from the AutomationTool log is shown below.");
		}

		if (Category == TEXT("Shipping target / receipt"))
		{
			return TEXT(
				"CrimeForge could not prepare the Win64 Shipping target required "
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
				"PASS 2 did not produce the IoStore files CrimeForge expected, or "
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
				"Check CrimeForge's PD3Game Path. It must point at the PAYDAY 3 "
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
		FCrimeForgeUATResult Result =
			LoadCrimeForgeUATResult(ResultFile);

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
						"AutomationTool returned '%s', but CrimeForge's structured "
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

		const FString RuntimeText =
			FString::Printf(
				TEXT("%.1f seconds"),
				RunTime);

		FString Summary =
			FString::Printf(
				TEXT(
					"Mod: %s\n"
					"Result: %s\n"
					"Phase: %s\n"
					"Category: %s\n"
					"Mode: %s\n"
					"Chunk: %s\n"
					"Runtime: %s\n\n"
					"%s\n\n"
					"Error:\n%s"
				),
				*Result.PluginName,
				*ResultType,
				Result.Phase.IsEmpty()
					? TEXT("Unknown")
					: *Result.Phase,
				*Category,
				Result.Mode.IsEmpty()
					? TEXT("Unknown")
					: *Result.Mode,
				Result.ChunkId.IsEmpty()
					? TEXT("-")
					: *Result.ChunkId,
				*RuntimeText,
				*Suggestion,
				Result.Message.IsEmpty()
					? TEXT("No exception message was recorded.")
					: *Result.Message);

		if (RemainingModCount > 0)
		{
			Summary += FString::Printf(
				TEXT(
					"\n\nBatch stopped: %d remaining selected mod(s) were not packaged."),
				RemainingModCount);
		}

		const FString ClipboardReport =
			Summary +
			TEXT("\n\nRelevant log lines:\n") +
			RelevantLines +
			(LogFile.IsEmpty()
				? FString()
				: TEXT("\n\nLog file:\n") + LogFile);

		TSharedRef<SWindow> FailureWindow =
			SNew(SWindow)
				.Title(
					FText::FromString(
						FString::Printf(
							TEXT(
								"CrimeForge Packaging Failed - %s"),
							*PluginName)))
				.ClientSize(
					FVector2D(
						900.0f,
						700.0f))
				.SupportsMaximize(true)
				.SupportsMinimize(false);

		FailureWindow->SetContent(
			SNew(SBorder)
				.Padding(FMargin(10.0f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
							.Text(
								FText::FromString(
									Summary))
							.AutoWrapText(true)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(
						0.0f,
						10.0f,
						0.0f,
						4.0f)
					[
						SNew(STextBlock)
							.Text(
								LOCTEXT(
									"CrimeForgeRelevantUATLines",
									"Relevant log lines"))
					]

					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SMultiLineEditableTextBox)
							.IsReadOnly(true)
							.Text(
								FText::FromString(
									RelevantLines))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(
						0.0f,
						8.0f,
						0.0f,
						0.0f)
					[
						SNew(STextBlock)
							.Text(
								FText::FromString(
									LogFile.IsEmpty()
										? TEXT(
											"Full UAT log: unavailable")
										: FString::Printf(
											TEXT(
												"Full UAT log: %s"),
											*LogFile)))
							.AutoWrapText(true)
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(
						0.0f,
						10.0f,
						0.0f,
						0.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(
							0.0f,
							0.0f,
							5.0f,
							0.0f)
						[
							SNew(SButton)
								.Text(
									LOCTEXT(
										"CopyCrimeForgePackagingError",
										"Copy Error"))
								.OnClicked_Lambda(
									[ClipboardReport]()
									{
										FPlatformApplicationMisc::ClipboardCopy(
											*ClipboardReport);

										return FReply::Handled();
									})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(
							0.0f,
							0.0f,
							5.0f,
							0.0f)
						[
							SNew(SButton)
								.Text(
									LOCTEXT(
										"OpenCrimeForgeUATLog",
										"Open Full Log"))
								.IsEnabled(
									!LogFile.IsEmpty())
								.OnClicked_Lambda(
									[LogFile]()
									{
										if (!LogFile.IsEmpty())
										{
											FPlatformProcess::
												LaunchFileInDefaultExternalApplication(
													*LogFile);
										}

										return FReply::Handled();
									})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(
							0.0f,
							0.0f,
							5.0f,
							0.0f)
						[
							SNew(SButton)
								.Text(
									LOCTEXT(
										"OpenCrimeForgeUATLogFolder",
										"Open Log Folder"))
								.IsEnabled(
									!EffectiveLogFolder.IsEmpty())
								.OnClicked_Lambda(
									[LogFolder = EffectiveLogFolder]()
									{
										if (!LogFolder.IsEmpty())
										{
											FPlatformProcess::ExploreFolder(
												*LogFolder);
										}

										return FReply::Handled();
									})
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SSpacer)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
								.Text(
									LOCTEXT(
										"CloseCrimeForgePackagingError",
										"Close"))
								.OnClicked_Lambda(
									[FailureWindow]()
									{
										FailureWindow
											->RequestDestroyWindow();

										return FReply::Handled();
									})
						]
					]
				]
		);

		FSlateApplication::Get().AddWindow(
			FailureWindow);
	}

	enum class ECrimeForgePreflightSeverity : uint8
	{
		Pass,
		Warning,
		Error
	};

	struct FCrimeForgePreflightItem
	{
		ECrimeForgePreflightSeverity Severity;
		FString Message;
	};

	struct FCrimeForgePreflightResult
	{
		TArray<FCrimeForgePreflightItem> Items;
		int32 PassCount = 0;
		int32 WarningCount = 0;
		int32 ErrorCount = 0;

		void Add(
			ECrimeForgePreflightSeverity Severity,
			const FString& Message)
		{
			Items.Add({ Severity, Message });

			switch (Severity)
			{
			case ECrimeForgePreflightSeverity::Pass:
				++PassCount;
				break;

			case ECrimeForgePreflightSeverity::Warning:
				++WarningCount;
				break;

			case ECrimeForgePreflightSeverity::Error:
				++ErrorCount;
				break;
			}
		}

		void Pass(const FString& Message)
		{
			Add(ECrimeForgePreflightSeverity::Pass, Message);
		}

		void Warning(const FString& Message)
		{
			Add(ECrimeForgePreflightSeverity::Warning, Message);
		}

		void Error(const FString& Message)
		{
			Add(ECrimeForgePreflightSeverity::Error, Message);
		}

		bool HasErrors() const
		{
			return ErrorCount > 0;
		}

		FString BuildReport(const FString& PluginName) const
		{
			FString Report = FString::Printf(
				TEXT(
					"CrimeForge Preflight - %s\n\n"
					"%d passed    %d warning(s)    %d error(s)\n"
					"----------------------------------------\n"
				),
				*PluginName,
				PassCount,
				WarningCount,
				ErrorCount);

			for (const FCrimeForgePreflightItem& Item : Items)
			{
				const TCHAR* Prefix = TEXT("[PASS]");

				switch (Item.Severity)
				{
				case ECrimeForgePreflightSeverity::Warning:
					Prefix = TEXT("[WARN]");
					break;

				case ECrimeForgePreflightSeverity::Error:
					Prefix = TEXT("[ERROR]");
					break;

				default:
					break;
				}

				Report += FString::Printf(
					TEXT("%s %s\n"),
					Prefix,
					*Item.Message);
			}

			return Report;
		}
	};

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

	struct FCrimeForgePALInspectionData
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
		FCrimeForgePALInspectionData& Data)
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

	FCrimeForgePALInspectionData BuildPALInspectionData(
		const IPlugin& Plugin)
	{
		FCrimeForgePALInspectionData Data;
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
				for (const FString& PluginAsset :
					Data.PluginAssets)
				{
					AddUniqueInspectionPath(
						Data.ManagedChunkCandidates,
						PluginAsset);
				}
			}

			for (const TSoftObjectPtr<UObject>& ExplicitAsset :
				Label->ExplicitAssets)
			{
				const FSoftObjectPath AssetPath =
					ExplicitAsset.ToSoftObjectPath();

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

			for (const TSoftClassPtr<UObject>& ExplicitBlueprint :
				Label->ExplicitBlueprints)
			{
				const FSoftObjectPath AssetPath =
					ExplicitBlueprint.ToSoftObjectPath();

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
		const FCrimeForgePALInspectionData& Data)
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
		const FCrimeForgePALInspectionData& Data)
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

	void AppendPALInspectionIssues(
		const FCrimeForgePALInspectionData& Data,
		FString& InOutText)
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

		if (Issues.Num() == 0)
		{
			return;
		}

		InOutText += TEXT("\n\nIssues:");
		for (const FString& Issue : Issues)
		{
			InOutText += TEXT("\n  - ") + Issue;
		}
	}

	bool CreateOrRepairCrimeForgePAL(
		IPlugin& Plugin,
		const FCrimeForgePALInspectionData& ExistingData,
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

		FCrimeForgeModule& CrimeForgeModule =
			FModuleManager::LoadModuleChecked<
				FCrimeForgeModule>(
				TEXT("CrimeForge"));

		CrimeForgeModule.EnsureAssetManagerRegistrationForPlugin(
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
		const FCrimeForgePALInspectionData& Data)
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
			PackageNames.AddUnique(PackageName);
		}
	}

	bool WritePALPackagingManifest(
		const IPlugin& Plugin,
		const int32 ChunkId,
		const FString& ManifestFile)
	{
		const FCrimeForgePALInspectionData Data =
			BuildPALInspectionData(Plugin);

		if (!Data.bPALFound)
		{
			UE_LOG(
				LogCrimeForge,
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
				LogCrimeForge,
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

		if (ExternalCookPackages.Num() == 0)
		{
			UE_LOG(
				LogCrimeForge,
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

		FString JsonText;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&JsonText);

		if (!FJsonSerializer::Serialize(
				RootObject,
				Writer))
		{
			UE_LOG(
				LogCrimeForge,
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
				LogCrimeForge,
				Error,
				TEXT(
					"Failed to write PAL packaging manifest for %s: %s"
				),
				*Plugin.GetName(),
				*ManifestFile);

			return false;
		}

		UE_LOG(
			LogCrimeForge,
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
				LogCrimeForge,
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
				LogCrimeForge,
				Display,
				TEXT(
					"No PAL found at %s. No external-content check is "
					"needed; using the normal plugin DLC cook."
				),
				*PALPath
			);
			return INDEX_NONE;
		}

		bool bHasExternalContent = false;

		// Explicit Assets can point outside /<PluginName>/ and are the main
		// mechanism used for /Game replacement assets.
		for (const TSoftObjectPtr<UObject>& ExplicitAsset : Label->ExplicitAssets)
		{
			const FSoftObjectPath AssetPath =
				ExplicitAsset.ToSoftObjectPath();

			if (IsOutsidePluginMount(AssetPath, PluginMountPath))
			{
				UE_LOG(
					LogCrimeForge,
					Display,
					TEXT(
						"PAL %s manages external asset %s. "
						"Two-stage cook is required."
					),
					*PALPath,
					*AssetPath.ToString()
				);

				bHasExternalContent = true;
				break;
			}
		}

		// Explicit Blueprints are a separate PAL list in UE5. Treat them the
		// same way so class replacements outside the plugin also trigger pass 2.
		if (!bHasExternalContent)
		{
			for (const TSoftClassPtr<UObject>& ExplicitBlueprint :
				Label->ExplicitBlueprints)
			{
				const FSoftObjectPath AssetPath =
					ExplicitBlueprint.ToSoftObjectPath();

				if (IsOutsidePluginMount(
					AssetPath,
					PluginMountPath))
				{
					UE_LOG(
						LogCrimeForge,
						Display,
						TEXT(
							"PAL %s manages external blueprint %s. "
							"Two-stage cook is required."
						),
						*PALPath,
						*AssetPath.ToString()
					);

					bHasExternalContent = true;
					break;
				}
			}
		}

		if (!bHasExternalContent)
		{
			UE_LOG(
				LogCrimeForge,
				Display,
				TEXT(
					"PAL %s contains no Explicit Assets or Explicit "
					"Blueprints outside %s. Skipping PASS 2 and using "
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
				LogCrimeForge,
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
			LogCrimeForge,
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
}


void SCrimeForgeModEntry::Construct(
	const FArguments& Args,
	TSharedRef<IPlugin> InMod,
	TSharedRef<SCrimeForgeModEntryList> InOwner)
{
	Mod = InMod;
	Owner = InOwner;

	UCrimeForgeSettings* Settings =
		UCrimeForgeSettings::Get();

	const FString PluginName =
		Mod->GetName();

	Checkbox =
		SNew(SCheckBox)
		.OnCheckStateChanged(
			this,
			&SCrimeForgeModEntry::OnEnableCheckboxChanged)
		.IsChecked(
			Settings->ModSelection.FindOrAdd(
				PluginName,
				false));

	/*
	 * CrimeForge mods live under <Project>/Mods rather than Unreal's normal
	 * <Project>/Plugins root. Use the physical CrimeForge mod directory as the
	 * authoritative icon location instead of relying on IPlugin::GetBaseDir(),
	 * which can point at a discovered/search-path representation that is not the
	 * directory the mod was actually created in during the current editor run.
	 *
	 * Expected path:
	 *   <Project>/Mods/<ModName>/Resources/Icon128.png
	 */
	FString IconPath =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Mods"),
				Mod->GetName(),
				TEXT("Resources"),
				TEXT("Icon128.png")));

	FPaths::NormalizeFilename(IconPath);

	/*
	 * Keep GetBaseDir() as a secondary lookup for engine/project plugins shown
	 * through the optional filters, where the plugin may not physically live in
	 * <Project>/Mods.
	 */
	if (!FPaths::FileExists(IconPath))
	{
		FString PluginBaseIconPath =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					Mod->GetBaseDir(),
					TEXT("Resources"),
					TEXT("Icon128.png")));

		FPaths::NormalizeFilename(PluginBaseIconPath);

		if (FPaths::FileExists(PluginBaseIconPath))
		{
			IconPath = MoveTemp(PluginBaseIconPath);
		}
	}

	if (!FPaths::FileExists(IconPath))
	{
		const TSharedPtr<IPlugin> CrimeForgePlugin =
			IPluginManager::Get().FindPlugin(
				TEXT("CrimeForge"));

		if (CrimeForgePlugin.IsValid())
		{
			IconPath =
				FPaths::ConvertRelativePathToFull(
					FPaths::Combine(
						CrimeForgePlugin->GetBaseDir(),
						TEXT("Resources"),
						TEXT("CrimeForge128.png")));

			FPaths::NormalizeFilename(IconPath);
		}
	}

	UE_LOG(
		LogCrimeForge,
		Verbose,
		TEXT("Mod card icon for '%s': %s (exists=%s)"),
		*Mod->GetName(),
		*IconPath,
		FPaths::FileExists(IconPath) ? TEXT("true") : TEXT("false"));

	/*
	 * These icons live outside a registered Slate style and can appear after
	 * the editor has already started (for example after Create Mod / legacy
	 * import). A normal FSlateImageBrush can leave the renderer with no loaded
	 * resource here, which paints as a solid white rectangle. Use a dynamic
	 * image brush so Slate registers and loads the PNG resource on demand.
	 */
	ModIconBrush =
		MakeShared<FSlateDynamicImageBrush>(
			FName(*IconPath),
			FVector2D(128.0f, 128.0f),
			FLinearColor::White,
			ESlateBrushTileType::NoTile,
			ESlateBrushImageType::FullColor);

	const FText DisplayName =
		FText::FromString(
			Mod->GetDescriptor().FriendlyName.IsEmpty()
				? Mod->GetName()
				: Mod->GetDescriptor().FriendlyName);

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
						.Text(DisplayName)
						.Justification(ETextJustify::Center)
						.Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
						.HighlightText_Lambda(
							[InOwner]()
							{
								return FText::FromString(
									InOwner->GetLastFilter());
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
							&SCrimeForgeModEntry::OnDeleteMod)
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
					.ContentPadding(FMargin(0.0f))
					.OnClicked(
						this,
						&SCrimeForgeModEntry::OnOpenModContent)
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
							SNew(SImage)
							.Image(ModIconBrush.Get())
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
						&SCrimeForgeModEntry::GetInformationButtonColor)
					.OnClicked(
						this,
						&SCrimeForgeModEntry::OnOpenInformation)
					.ToolTipText(
						this,
						&SCrimeForgeModEntry::GetInformationButtonTooltip)
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
										SCrimeForgeModEntry>>());

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

FReply SCrimeForgeModEntry::OnOpenModContent()
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

FReply SCrimeForgeModEntry::OnOpenInformation()
{
	OnRefreshPALInspector();

	const FString WindowTitle =
		Mod.IsValid()
			? FString::Printf(
				TEXT("%s - Information"),
				*Mod->GetName())
			: TEXT("Mod Information");

	TSharedRef<SWindow> InformationWindow =
		SNew(SWindow)
		.Title(
			FText::FromString(
				WindowTitle))
		.ClientSize(
			FVector2D(
				650.0f,
				300.0f))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SScrollBox)

					+ SScrollBox::Slot()
					[
						SNew(STextBlock)
						.Text(
							this,
							&SCrimeForgeModEntry::GetPALInspectorSummaryText)
						.AutoWrapText(true)
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)

                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(0.0f, 0.0f, 5.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(
                            LOCTEXT(
                                "PALInspectorFixIssues",
                                "Fix Issues"))
                        .ButtonColorAndOpacity(
                            FSlateColor(
                                FLinearColor(
                                    1.0f,
                                    0.35f,
                                    0.03f,
                                    1.0f)))
                        .Visibility(
                            this,
                            &SCrimeForgeModEntry::GetFixInformationVisibility)
                        .IsEnabled(
                            this,
                            &SCrimeForgeModEntry::CanAutoFixInformationIssues)
                        .ToolTipText(
                            LOCTEXT(
                                "PALInspectorFixIssuesTooltip",
                                "Automatically repair safe PAL and Asset Manager configuration issues. Missing explicit asset references are left for manual repair."))
                        .OnClicked(
                            this,
                            &SCrimeForgeModEntry::OnFixInformationIssues)
                    ]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 5.0f, 0.0f)
					[
						SNew(SButton)
						.Text(
							LOCTEXT(
								"PALInspectorRefresh",
								"Refresh"))
						.OnClicked(
							this,
							&SCrimeForgeModEntry::OnRefreshPALInspector)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(
							LOCTEXT(
								"PALInspectorViewAssets",
								"View Assets"))
						.OnClicked(
							this,
							&SCrimeForgeModEntry::OnViewPALAssets)
					]
				]
			]
		];

	FSlateApplication::Get().AddWindow(
		InformationWindow);

	return FReply::Handled();
}

FText SCrimeForgeModEntry::GetPALInspectorSummaryText() const
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

void SCrimeForgeModEntry::RefreshPALInspectorState()
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

	const FCrimeForgePALInspectionData Data =
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
}

FReply SCrimeForgeModEntry::OnRefreshPALInspector()
{
	RefreshPALInspectorState();
	return FReply::Handled();
}

FSlateColor SCrimeForgeModEntry::GetInformationButtonColor() const
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

FText SCrimeForgeModEntry::GetInformationButtonTooltip() const
{
	return
		bInformationHasWarnings
			? LOCTEXT(
				"ModCardInformationWarningTooltip",
				"CrimeForge found one or more issues. Open Information for details and available automatic fixes.")
			: LOCTEXT(
				"ModCardInformationTooltip",
				"Show PAL, chunk, packaging mode, and managed asset information");
}

EVisibility SCrimeForgeModEntry::GetFixInformationVisibility() const
{
	return
		bInformationHasWarnings
			? EVisibility::Visible
			: EVisibility::Collapsed;
}

bool SCrimeForgeModEntry::CanAutoFixInformationIssues() const
{
	return bInformationHasAutoFixableWarnings;
}

FReply SCrimeForgeModEntry::OnFixInformationIssues()
{
	if (!Mod.IsValid())
	{
		return FReply::Handled();
	}

	const FCrimeForgePALInspectionData Before =
		BuildPALInspectionData(*Mod);

	FString FixResult;

	const bool bFixed =
		CreateOrRepairCrimeForgePAL(
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
						"CrimeForge could not complete the automatic repair.\n\n%s"
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
						"%s\n\nCrimeForge fixed the safe configuration issues. "
						"One or more references still require manual repair; "
						"see Information for details."
					),
					*FixResult)));
	}

	return FReply::Handled();
}

FReply SCrimeForgeModEntry::OnViewPALAssets()
{
	if (!Mod.IsValid())
	{
		return FReply::Handled();
	}

	const FCrimeForgePALInspectionData Data =
		BuildPALInspectionData(*Mod);

	const FString AssetReport =
		BuildPALAssetReport(Data);

	TSharedRef<SWindow> AssetsWindow =
		SNew(SWindow)
			.Title(
				FText::FromString(
					FString::Printf(
						TEXT(
							"%s - PAL / Chunk Assets"),
						*Mod->GetName())))
			.ClientSize(
				FVector2D(
					850.0f,
					650.0f))
			.SupportsMaximize(true)
			.SupportsMinimize(false)
			[
				SNew(SBorder)
					.Padding(
						FMargin(
							8.0f))
					[
						SNew(SScrollBox)

						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
								.Text(
									FText::FromString(
										AssetReport))
								.AutoWrapText(false)
						]
					]
			];

	FSlateApplication::Get().AddWindow(
		AssetsWindow);

	return FReply::Handled();
}

void SCrimeForgeModEntry::OnEditMod()
{
	// Construct the plugin metadata object using the descriptor for this plugin
	UModMetadataObject* MetadataObject = NewObject<UModMetadataObject>();
	MetadataObject->TargetIconPath = Mod->GetBaseDir() / TEXT("Resources/Icon128.png");
	MetadataObject->PopulateFromDescriptor(Mod->GetDescriptor());
	MetadataObject->AddToRoot();

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
					.OnClicked(this, &SCrimeForgeModEntry::OnEditModFinished, MetadataObject)
				]
			]
		];

	FSlateApplication::Get().AddModalWindow(PropertiesWindow.ToSharedRef(), Owner);
}

FReply SCrimeForgeModEntry::OnEditModFinished(UModMetadataObject* MetadataObject)
{
	FPluginDescriptor OldDescriptor = Mod->GetDescriptor();

	// Update the descriptor with the new metadata
	FPluginDescriptor NewDescriptor = OldDescriptor;
	MetadataObject->CopyIntoDescriptor(NewDescriptor);
	MetadataObject->RemoveFromRoot();

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
	return FReply::Handled();
}

FString GetArgumentForLaunchType(ECrimeForgeStartGameType LaunchMode) {
	return TEXT("");
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

bool SCrimeForgeModEntry::RunPreflightValidation() const
{
	if (!Mod.IsValid())
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT(
				"PreflightInvalidMod",
				"CrimeForge preflight failed because the mod entry is no longer valid."));

		return false;
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

	FCrimeForgePreflightResult Result;

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

			if (DependencyString.Contains(
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
		Result.Pass(TEXT("No stale PLACEHOLDER_* package references were found."));
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

		int32 ExternalAssetCount = 0;
		int32 ExternalBlueprintCount = 0;
		int32 MissingExplicitReferenceCount = 0;

		for (const TSoftObjectPtr<UObject>& ExplicitAsset :
			Label->ExplicitAssets)
		{
			const FSoftObjectPath AssetPath =
				ExplicitAsset.ToSoftObjectPath();

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

		for (const TSoftClassPtr<UObject>& ExplicitBlueprint :
			Label->ExplicitBlueprints)
		{
			const FSoftObjectPath AssetPath =
				ExplicitBlueprint.ToSoftObjectPath();

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
					"The directory scan can still discover it, but CrimeForge normally registers both."));
		}
	}

	// ------------------------------------------------------------------
	// Present results and decide whether packaging may continue.
	// ------------------------------------------------------------------
	FString Report = Result.BuildReport(PluginName);

	UE_LOG(
		LogCrimeForge,
		Display,
		TEXT("%s"),
		*Report);

	if (Result.HasErrors())
	{
		Report +=
			TEXT(
				"\nPackaging has been blocked. Fix the error(s) above and run CrimeForge again.");

		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(Report));

		return false;
	}

	Report +=
		TEXT(
			"\nPreflight passed with no blocking errors.\n\nContinue packaging?");

	return FMessageDialog::Open(
		EAppMsgType::YesNo,
		FText::FromString(Report)) == EAppReturnType::Yes;
}


void SCrimeForgeModEntry::PackageMod(const TArray<TSharedPtr<SCrimeForgeModEntry>>& NextEntries) const {
    UCrimeForgeSettings* Settings = UCrimeForgeSettings::Get();
    const FString PluginName = Mod->GetName();
    const FString GamePath = Settings->PD3GamePath.Path;

    // Make the game path persistence independent of when the details view
    // decides that editing has finished.
    Settings->SaveSettings();

    const FString ProjectPath = FPaths::IsProjectFilePathSet()
        ? FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())
        : FPaths::RootDir() / FApp::GetProjectName() / FApp::GetProjectName() + TEXT(".uproject");

    FString AdditionalUATArguments;
    if (Settings->bCopyModsToGame) {
        AdditionalUATArguments.Append(TEXT("-CopyToGameDir "));
    }
    if (Settings->LaunchGameAfterPacking != ECrimeForgeStartGameType::NONE && NextEntries.Num() == 0) {
        AdditionalUATArguments.Append(TEXT("-LaunchGame "));
        AdditionalUATArguments.Append(GetArgumentForLaunchType(Settings->LaunchGameAfterPacking)).Append(TEXT(" "));
    }

    /*
     * Repair/ensure Asset Manager registration immediately before UAT starts.
     * This also fixes already-created mods: /<ModName> is persisted into the
     * PrimaryAssetLabel Directories list and the PAL is added to SpecificAssets.
     */
    FCrimeForgeModule& CrimeForgeModule =
        FModuleManager::LoadModuleChecked<FCrimeForgeModule>(TEXT("CrimeForge"));

    CrimeForgeModule.EnsureAssetManagerRegistrationForPlugin(*Mod);

    /*
     * Validate the mod after CrimeForge's automatic Asset Manager repair but
     * before deciding on PASS 2 or launching UAT.
     */
    if (!RunPreflightValidation())
    {
        UE_LOG(
            LogCrimeForge,
            Warning,
            TEXT("Packaging cancelled by preflight for mod %s."),
            *PluginName);

        return;
    }

    // Only request PASS 2 when the PAL explicitly manages content outside
    // the plugin mount. Mods containing only plugin assets stay on the fast
    // original single-stage DLC path.
    const int32 ChunkId = GetModChunkIdIfExternalContentExists(*Mod);

    const FString ResultDirectory =
        FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("CrimeForge"),
            TEXT("Results"));

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
                            "CrimeForge could not build the deterministic PAL "
                            "package list for '%s'. Packaging has been stopped."
                        ),
                        *PluginName)));

            return;
        }

        AdditionalUATArguments.Append(
            FString::Printf(
                TEXT("-CrimeForgePALManifestFile=\"%s\" "),
                *PALPackagingManifestFile));
    }

    UE_LOG(LogCrimeForge, Display, TEXT("Packaging plugin \"%s\". %d remaining"), *PluginName, NextEntries.Num());

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
            TEXT("-CrimeForgeResultFile=\"%s\" "),
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

    const FString CommandLine = FString::Printf(
        TEXT("-ScriptsForProject=\"%s\" PackagePlugin -Project=\"%s\" -PluginName=\"%s\" -GameDir=\"%s\" %s"),
        *ProjectPath,
        *ProjectPath,
        *PluginName,
        *GamePath,
        *AdditionalUATArguments
    );

    const FText PlatformName = GetCurrentPlatformName();
    IUATHelperModule::Get().CreateUatTask(
        CommandLine,
        PlatformName,
        LOCTEXT("PackageModTaskName", "Packaging Mod"),
        LOCTEXT("PackageModTaskShortName", "Package Mod Task"),
        FCrimeForgeStyle::Get().GetBrush("CrimeForge.OpenPluginWindow"),
        nullptr,
        [PluginName, ResultFile, UATLogFile, NextEntries](
            FString resultType,
            double runTime)
        {
            AsyncTask(
                ENamedThreads::GameThread,
                [PluginName,
                 ResultFile,
                 UATLogFile,
                 NextEntries,
                 resultType,
                 runTime]()
                {
                    const FCrimeForgeUATResult ParsedResult =
                        LoadCrimeForgeUATResult(ResultFile);

                    const bool bFailed =
                        ParsedResult.bLoaded
                            ? !ParsedResult.bSuccess
                            : DoesResultTypeIndicateFailure(
                                resultType);

                    if (bFailed)
                    {
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
                        TSharedPtr<SCrimeForgeModEntry> NextMod =
                            NextEntries[0];

                        TArray<TSharedPtr<SCrimeForgeModEntry>>
                            RemainingEntries =
                                NextEntries.FilterByPredicate(
                                    [NextMod](
                                        const TSharedPtr<
                                            SCrimeForgeModEntry>& X)
                                    {
                                        return X != NextMod;
                                    });

                        NextMod->PackageMod(
                            RemainingEntries);
                    }
                });
        }
    );
}

FReply SCrimeForgeModEntry::OnDeleteMod()
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
                    "CrimeForge refused to delete '%s' because its directory is "
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
                "CrimeForge will also remove the mod from the Asset Manager "
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

    // Remove /<ModName> and any /<ModName>/... Specific Assets.
    FCrimeForgeModule& CrimeForgeModule =
        FModuleManager::LoadModuleChecked<FCrimeForgeModule>(
            TEXT("CrimeForge"));

    CrimeForgeModule.RemoveAssetManagerRegistrationForPlugin(
        PluginName,
        PluginMountPath);

    // Remove the saved checkbox selection for the deleted mod.
    if (UCrimeForgeSettings* Settings = UCrimeForgeSettings::Get())
    {
        Settings->ModSelection.Remove(PluginName);
        Settings->SaveSettings();
    }

    /*
     * Remove the deleted descriptor from PluginManager's discovered list and
     * refresh discovery. The content mount may remain alive until editor
     * restart if assets from the mod were already loaded, but it will no
     * longer exist on disk or in CrimeForge's list.
     */
    FText RemovePluginFailReason;
    if (!IPluginManager::Get().RemoveFromPluginsList(
            DescriptorFileName,
            &RemovePluginFailReason))
    {
        UE_LOG(
            LogCrimeForge,
            Warning,
            TEXT(
                "Mod %s was deleted from disk, but PluginManager could not "
                "remove its descriptor from the current in-memory list: %s"
            ),
            *PluginName,
            *RemovePluginFailReason.ToString());
    }

    IPluginManager::Get().RefreshPluginsList();

    UE_LOG(
        LogCrimeForge,
        Display,
        TEXT("Permanently deleted mod %s from %s"),
        *PluginName,
        *ModBaseDir);

    /*
     * Refresh the list after this Slate click handler returns, so this widget
     * is not destroyed while one of its own member functions is executing.
     */
    TWeakPtr<SCrimeForgeModEntryList> WeakOwner = Owner;

    AsyncTask(
        ENamedThreads::GameThread,
        [WeakOwner]()
        {
            if (TSharedPtr<SCrimeForgeModEntryList> PinnedOwner =
                    WeakOwner.Pin())
            {
                PinnedOwner->LoadMods();
            }
        });

    return FReply::Handled();
}

void SCrimeForgeModEntry::OnEnableCheckboxChanged(ECheckBoxState NewState) {
    // Save new checked state for the mod at the
    UCrimeForgeSettings* Settings = UCrimeForgeSettings::Get();
    const FString PluginName = Mod->GetName();

    Settings->ModSelection.Add(PluginName, NewState == ECheckBoxState::Checked);

    Settings->SaveSettings();
}

#undef LOCTEXT_NAMESPACE
