#include "Updater/PD3ModkitUpdater.h"
#include "Packaging/CrimeFactoryUpdaterUI.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "FileHelpers.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace PD3ModkitUpdater
{
    static constexpr float StartupCheckDelaySeconds = 4.0f;
    static const TCHAR* CanonicalRepositoryFragment = TEXT("github.com/payday3-modding/pd3-modkit");

    enum class EUpdateState : uint8
    {
        UpToDate,
        UpdateAvailable,
        Diverged,
        Error
    };

    struct FUpdateCheckResult
    {
        EUpdateState State = EUpdateState::Error;
        FString Error;
        FString RepositoryRoot;
        FString RemoteName;
        FString BranchName;
        bool bResumePending = false;
        FString LocalCommit;
        FString RemoteCommit;
        FString LatestCommitSubject;
        int32 CommitsAhead = 0;
        int32 CommitsBehind = 0;
        int32 LocalWorkingTreeChanges = 0;
    };

    static FTSTicker::FDelegateHandle StartupTickerHandle;
    static FTSTicker::FDelegateHandle PresentationTickerHandle;
    static bool bCheckInProgress = false;
    static TAtomic<bool> bShuttingDown(false);

    struct FPendingUpdatePresentation
    {
        FUpdateCheckResult Modkit;
        FCrimeFactoryEngineUpdateResult Engine;
        bool bUserInitiated = false;
    };

    // Read and written only on the game thread. Completing a task must not open
    // native windows: tasks also execute inside subsystem waits and window setup.
    static TOptional<FPendingUpdatePresentation> PendingCheckResult;

    static FString QuoteArgument(const FString& Value)
    {
        FString Escaped = Value;
        Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
        return FString::Printf(TEXT("\"%s\""), *Escaped);
    }

    static bool RunGit(
        const FString& WorkingDirectory,
        const FString& Arguments,
        int32& OutReturnCode,
        FString& OutStdOut,
        FString& OutStdErr)
    {
        FString Parameters;
        if (!WorkingDirectory.IsEmpty())
        {
            Parameters = FString::Printf(
                TEXT("-C %s %s"),
                *QuoteArgument(WorkingDirectory),
                *Arguments);
        }
        else
        {
            Parameters = Arguments;
        }

        OutReturnCode = INDEX_NONE;
        OutStdOut.Reset();
        OutStdErr.Reset();

        const bool bLaunched = FPlatformProcess::ExecProcess(
            TEXT("git"),
            *Parameters,
            &OutReturnCode,
            &OutStdOut,
            &OutStdErr);

        OutStdOut.TrimStartAndEndInline();
        OutStdErr.TrimStartAndEndInline();
        return bLaunched && OutReturnCode == 0;
    }

    static bool RunGitSimple(
        const FString& WorkingDirectory,
        const FString& Arguments,
        FString& OutStdOut,
        FString& OutError)
    {
        int32 ReturnCode = INDEX_NONE;
        FString StdErr;
        if (!RunGit(WorkingDirectory, Arguments, ReturnCode, OutStdOut, StdErr))
        {
            OutError = StdErr;
            if (OutError.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("Git command failed (exit code %d): git %s"),
                    ReturnCode,
                    *Arguments);
            }
            return false;
        }

        return true;
    }

    static FString NormalizeRemoteUrl(FString Url)
    {
        Url.TrimStartAndEndInline();
        Url.ToLowerInline();
        Url.ReplaceInline(TEXT("\\"), TEXT("/"));
        Url.RemoveFromEnd(TEXT(".git"));
        return Url;
    }

    static bool RemoteMatchesCanonicalRepository(const FString& RemoteUrl)
    {
        FString Normalized = NormalizeRemoteUrl(RemoteUrl);

        // HTTPS form: https://github.com/PAYDAY3-Modding/PD3-Modkit.git
        if (Normalized.Contains(CanonicalRepositoryFragment))
        {
            return true;
        }

        // SSH form: git@github.com:PAYDAY3-Modding/PD3-Modkit.git
        Normalized.ReplaceInline(TEXT("github.com:"), TEXT("github.com/"));
        return Normalized.Contains(CanonicalRepositoryFragment);
    }

    static bool FindRepositoryRoot(FString& OutRepositoryRoot, FString& OutError)
    {
        const FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

        FString Output;
        if (!RunGitSimple(ProjectDirectory, TEXT("rev-parse --show-toplevel"), Output, OutError))
        {
            OutError = FString::Printf(
                TEXT("The PD3 Modkit project is not inside a Git working tree, or Git is not available on PATH.\n\n%s"),
                *OutError);
            return false;
        }

        OutRepositoryRoot = FPaths::ConvertRelativePathToFull(Output);
        FPaths::NormalizeDirectoryName(OutRepositoryRoot);
        return true;
    }

    static bool FindCanonicalRemote(
        const FString& RepositoryRoot,
        FString& OutRemoteName,
        FString& OutError)
    {
        FString RemotesOutput;
        if (!RunGitSimple(RepositoryRoot, TEXT("remote"), RemotesOutput, OutError))
        {
            return false;
        }

        TArray<FString> Remotes;
        RemotesOutput.ParseIntoArrayLines(Remotes, true);

        for (FString& Remote : Remotes)
        {
            Remote.TrimStartAndEndInline();
            if (Remote.IsEmpty())
            {
                continue;
            }

            FString RemoteUrl;
            FString RemoteError;
            if (!RunGitSimple(
                    RepositoryRoot,
                    FString::Printf(TEXT("remote get-url %s"), *QuoteArgument(Remote)),
                    RemoteUrl,
                    RemoteError))
            {
                continue;
            }

            if (RemoteMatchesCanonicalRepository(RemoteUrl))
            {
                OutRemoteName = Remote;
                return true;
            }
        }

        OutError = TEXT(
            "No Git remote points to github.com/PAYDAY3-Modding/PD3-Modkit. "
            "The automatic updater only updates from the official PD3 Modkit repository.");
        return false;
    }

    static bool ParseAheadBehindCounts(
        const FString& CountsText,
        int32& OutAhead,
        int32& OutBehind)
    {
        TArray<FString> Parts;
        CountsText.ParseIntoArrayWS(Parts);
        if (Parts.Num() < 2)
        {
            return false;
        }

        OutAhead = FCString::Atoi(*Parts[0]);
        OutBehind = FCString::Atoi(*Parts[1]);
        return true;
    }

    static void CollectWorkingTreeChangeInfo(FUpdateCheckResult& Result)
    {
        FString Status;
        FString IgnoredError;
        if (!RunGitSimple(
                Result.RepositoryRoot,
                TEXT("status --porcelain=v1 --untracked-files=all"),
                Status,
                IgnoredError))
        {
            return;
        }

        TArray<FString> Lines;
        Status.ParseIntoArrayLines(Lines, true);
        Result.LocalWorkingTreeChanges = Lines.Num();
    }

    static FUpdateCheckResult PerformUpdateCheck()
    {
        FUpdateCheckResult Result;

        if (!FindRepositoryRoot(Result.RepositoryRoot, Result.Error))
        {
            return Result;
        }

        if (!FindCanonicalRemote(Result.RepositoryRoot, Result.RemoteName, Result.Error))
        {
            return Result;
        }

        FString Output;
        if (!RunGitSimple(
                Result.RepositoryRoot,
                TEXT("branch --show-current"),
                Result.BranchName,
                Result.Error))
        {
            return Result;
        }

        Result.BranchName.TrimStartAndEndInline();
        if (Result.BranchName.IsEmpty())
        {
            Result.Error = TEXT(
                "The PD3 Modkit repository is currently in detached HEAD state. "
                "Check out a normal branch before using automatic updates.");
            return Result;
        }

        // An installed update can need recovery/rebuild even with no commits behind.
        // Keep this available offline; the helper validates the checkpoint and HEAD.
        FString CheckpointPath;
        if (RunGitSimple(Result.RepositoryRoot,
                TEXT("rev-parse --path-format=absolute --git-path crimefactory-update.json"),
                CheckpointPath, Output) && IFileManager::Get().FileExists(*CheckpointPath))
        {
            FString CheckpointJson;
            TSharedPtr<FJsonObject> Checkpoint;
            if (!FFileHelper::LoadFileToString(CheckpointJson, *CheckpointPath) ||
                !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CheckpointJson), Checkpoint) ||
                !Checkpoint.IsValid())
            {
                Result.Error = TEXT("The updater recovery checkpoint is unreadable. Review crimefactory-update.json in the Git directory before updating.");
                return Result;
            }
            FString Stage;
            if (!Checkpoint->TryGetStringField(TEXT("Stage"), Stage) || Stage != TEXT("EditorReady"))
            {
                Result.bResumePending = true;
                Result.State = EUpdateState::UpdateAvailable;
                CollectWorkingTreeChangeInfo(Result);
                return Result;
            }
        }

        // Fetch only updates refs. It never changes the user's working tree.
        if (!RunGitSimple(
                Result.RepositoryRoot,
                FString::Printf(TEXT("fetch --quiet %s"), *QuoteArgument(Result.RemoteName)),
                Output,
                Result.Error))
        {
            Result.Error = FString::Printf(
                TEXT("Could not fetch updates from '%s'.\n\n%s"),
                *Result.RemoteName,
                *Result.Error);
            return Result;
        }

        const FString RemoteRef = FString::Printf(
            TEXT("%s/%s"),
            *Result.RemoteName,
            *Result.BranchName);

        if (!RunGitSimple(
                Result.RepositoryRoot,
                FString::Printf(TEXT("rev-parse --verify %s"), *QuoteArgument(RemoteRef)),
                Result.RemoteCommit,
                Result.Error))
        {
            Result.Error = FString::Printf(
                TEXT("The official remote does not contain branch '%s'.\n\n%s"),
                *Result.BranchName,
                *Result.Error);
            return Result;
        }

        if (!RunGitSimple(
                Result.RepositoryRoot,
                TEXT("rev-parse HEAD"),
                Result.LocalCommit,
                Result.Error))
        {
            return Result;
        }

        FString Counts;
        if (!RunGitSimple(
                Result.RepositoryRoot,
                FString::Printf(
                    TEXT("rev-list --left-right --count HEAD...%s"),
                    *QuoteArgument(RemoteRef)),
                Counts,
                Result.Error))
        {
            return Result;
        }

        if (!ParseAheadBehindCounts(Counts, Result.CommitsAhead, Result.CommitsBehind))
        {
            Result.Error = FString::Printf(
                TEXT("Could not parse Git ahead/behind counts: '%s'."),
                *Counts);
            return Result;
        }

        // This is informational only, so ignore a failure here.
        FString IgnoredError;
        RunGitSimple(
            Result.RepositoryRoot,
            FString::Printf(
                TEXT("log -1 --format=%%s %s"),
                *QuoteArgument(RemoteRef)),
            Result.LatestCommitSubject,
            IgnoredError);

        CollectWorkingTreeChangeInfo(Result);

        if (Result.CommitsBehind == 0)
        {
            Result.State = EUpdateState::UpToDate;
            return Result;
        }

        if (Result.CommitsAhead > 0)
        {
            Result.State = EUpdateState::Diverged;
            return Result;
        }

        Result.State = EUpdateState::UpdateAvailable;
        return Result;
    }

    static FString ShortCommit(const FString& Commit)
    {
        return Commit.Left(FMath::Min(Commit.Len(), 8));
    }

    static bool WriteUpdateHelperScript(
        FString& OutScriptPath,
        FString& OutUpdaterDirectory,
        FString& OutError)
    {
        // Keep the running updater outside the project tree. Once Unreal exits,
        // Git is free to update the project without touching the helper process
        // or the script it is currently executing.
        OutUpdaterDirectory = FPaths::Combine(
            FPlatformProcess::UserSettingsDir(),
            TEXT("PAYDAY3-Modding"),
            TEXT("PD3-Modkit"),
            TEXT("Updater"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits));

        const FString UpdateDirectory = OutUpdaterDirectory;

        if (!FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*UpdateDirectory))
        {
            OutError = FString::Printf(
                TEXT("Could not create updater directory: %s"),
                *UpdateDirectory);
            return false;
        }

        const TSharedPtr<IPlugin> CrimeFactoryPlugin =
            IPluginManager::Get().FindPlugin(TEXT("CrimeFactory"));
        if (!CrimeFactoryPlugin.IsValid())
        {
            OutError = TEXT("Could not locate the CrimeFactory plugin installation directory.");
            return false;
        }

        const FString TemplatePath = FPaths::Combine(
            CrimeFactoryPlugin->GetBaseDir(),
            TEXT("Resources"),
            TEXT("PD3ModkitUpdate.ps1"));

        FString ScriptContents;
        if (!FFileHelper::LoadFileToString(ScriptContents, *TemplatePath))
        {
            OutError = FString::Printf(
                TEXT("Could not load the updater helper template: %s"),
                *TemplatePath);
            return false;
        }

        OutScriptPath = FPaths::Combine(UpdateDirectory, TEXT("PD3ModkitUpdate.ps1"));
        if (!FFileHelper::SaveStringToFile(
                ScriptContents,
                *OutScriptPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            OutError = FString::Printf(
                TEXT("Could not write updater helper script: %s"),
                *OutScriptPath);
            return false;
        }

        return true;
    }

    static bool LaunchUpdateHelperAndCloseEditor(
        const FUpdateCheckResult& Result,
        FString& OutError)
    {
        // Make the update explicit and safe. We do this before starting the
        // external helper so cancelling the save dialog cannot leave an updater
        // waiting to run after some unrelated editor shutdown.
        if (!FEditorFileUtils::SaveDirtyPackages(
                true,  // prompt user
                true,  // maps
                true,  // content
                false, // fast save
                false, // notify when nothing needed saving
                false  // do not allow "Don't Save"; Cancel aborts update
            ))
        {
            OutError = TEXT(
                "The update was cancelled because not all unsaved editor changes were saved.");
            return false;
        }

        FString ScriptPath;
        FString UpdaterDirectory;
        if (!WriteUpdateHelperScript(ScriptPath, UpdaterDirectory, OutError))
        {
            return false;
        }

        const FString ReadyFilePath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdater.ready"));
        IFileManager::Get().Delete(*ReadyFilePath, false, true, true);

        const FString UpdateLogPath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdate.log"));

        FString ProjectFile = FPaths::GetProjectFilePath();
        if (ProjectFile.IsEmpty())
        {
            OutError = TEXT("Could not determine the current .uproject path for automatic reopening.");
            return false;
        }

        ProjectFile = FPaths::ConvertRelativePathToFull(ProjectFile);
        FPaths::NormalizeFilename(ProjectFile);

        const FString EditorExecutable = FPlatformProcess::ExecutablePath();
        if (EditorExecutable.IsEmpty())
        {
            OutError = TEXT("Could not determine the current Unreal Editor executable path.");
            return false;
        }

        const uint32 EditorPid = FPlatformProcess::GetCurrentProcessId();

        // Pass launch data through a JSON file instead of a long PowerShell command
        // line. This avoids Windows quoting/parameter-binding problems for project
        // paths containing spaces or other shell-significant characters.
        const FString ConfigPath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdate.args.json"));

        TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
        Config->SetNumberField(TEXT("EditorPid"), static_cast<double>(EditorPid));
        Config->SetStringField(TEXT("RepositoryRoot"), Result.RepositoryRoot);
        Config->SetStringField(TEXT("RemoteName"), Result.RemoteName);
        Config->SetStringField(TEXT("BranchName"), Result.BranchName);
        Config->SetStringField(TEXT("LogPath"), UpdateLogPath);
        Config->SetStringField(TEXT("EditorExecutable"), EditorExecutable);
        // Use the running engine, including registered/source-built engines;
        // the project's EngineAssociation is not necessarily a Launcher version.
        Config->SetStringField(TEXT("EngineDirectory"), FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));
        Config->SetStringField(TEXT("ProjectFile"), ProjectFile);
        Config->SetStringField(TEXT("ReadyFile"), ReadyFilePath);

        FString ConfigJson;
        const TSharedRef<TJsonWriter<>> JsonWriter =
            TJsonWriterFactory<>::Create(&ConfigJson);
        if (!FJsonSerializer::Serialize(Config, JsonWriter))
        {
            OutError = TEXT("Could not serialize the external updater launch configuration.");
            return false;
        }

        if (!FFileHelper::SaveStringToFile(
                ConfigJson,
                *ConfigPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            OutError = FString::Printf(
                TEXT("Could not write updater launch configuration: %s"),
                *ConfigPath);
            return false;
        }

        const FString BootstrapLogPath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdate.bootstrap.log"));
        const FString ProcessLogPath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdate.process.log"));
        const FString WrapperPath = FPaths::Combine(
            UpdaterDirectory,
            TEXT("PD3ModkitUpdater.cmd"));

        IFileManager::Get().Delete(*BootstrapLogPath, false, true, true);
        IFileManager::Get().Delete(*ProcessLogPath, false, true, true);

        // The wrapper intentionally references everything relative to itself.
        // That keeps its command line ASCII-only and avoids quoting the user's
        // project/repository paths in cmd.exe.
        const FString WrapperContents =
            TEXT("@echo off\r\n")
            TEXT("setlocal DisableDelayedExpansion\r\n")
            TEXT("set \"PD3_PROCESS_LOG=%~dp0PD3ModkitUpdate.process.log\"\r\n")
            TEXT(">\"%PD3_PROCESS_LOG%\" echo [wrapper] PD3 Modkit updater launcher started.\r\n")
            TEXT(">>\"%PD3_PROCESS_LOG%\" echo [wrapper] Working directory: %CD%\r\n")
            TEXT(">>\"%PD3_PROCESS_LOG%\" echo [wrapper] Script: %~dp0PD3ModkitUpdate.ps1\r\n")
            TEXT(">>\"%PD3_PROCESS_LOG%\" echo [wrapper] Config: %~dp0PD3ModkitUpdate.args.json\r\n")
            TEXT("\"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" ")
            TEXT("-NoLogo -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden ")
            TEXT("-File \"%~dp0PD3ModkitUpdate.ps1\" ")
            TEXT("-ConfigPath \"%~dp0PD3ModkitUpdate.args.json\" ")
            TEXT(">>\"%PD3_PROCESS_LOG%\" 2>&1\r\n")
            TEXT("set \"PD3_EXIT=%ERRORLEVEL%\"\r\n")
            TEXT(">>\"%PD3_PROCESS_LOG%\" echo [wrapper] PowerShell exited with code %PD3_EXIT%.\r\n")
            TEXT("exit /b %PD3_EXIT%\r\n");

        if (!FFileHelper::SaveStringToFile(
                WrapperContents,
                *WrapperPath,
                FFileHelper::EEncodingOptions::ForceAnsi))
        {
            OutError = FString::Printf(
                TEXT("Could not write updater launcher: %s"),
                *WrapperPath);
            return false;
        }

        uint32 UpdaterProcessId = 0;

        // Launch only the simple wrapper filename from its own working directory.
        // This intentionally avoids sending the long set of project paths through
        // FPlatformProcess::CreateProc's command-line quoting layer.
        FProcHandle ProcessHandle = FPlatformProcess::CreateProc(
            TEXT("cmd.exe"),
            TEXT("/D /S /C PD3ModkitUpdater.cmd"),
            true,   // detached: updater must survive the editor process
            false,
            true,   // suppress the console window; the WPF window remains visible
            &UpdaterProcessId,
            0,
            *UpdaterDirectory,
            nullptr,
            nullptr,
            nullptr);

        if (!ProcessHandle.IsValid())
        {
            OutError = TEXT(
                "Could not launch the external PD3 Modkit updater wrapper. "
                "Windows Command Prompt and Windows PowerShell must be available.");
            return false;
        }

        // Do not close Unreal until the independent updater has actually created
        // and rendered its WPF window. The batch wrapper and JSON launch config
        // also ensure PowerShell startup errors are persisted even if the script
        // fails before its own logger can initialize.
        constexpr double ReadyTimeoutSeconds = 20.0;
        const double ReadyDeadline = FPlatformTime::Seconds() + ReadyTimeoutSeconds;
        bool bUpdaterReady = false;

        while (FPlatformTime::Seconds() < ReadyDeadline)
        {
            if (IFileManager::Get().FileExists(*ReadyFilePath))
            {
                bUpdaterReady = true;
                break;
            }

            if (!FPlatformProcess::IsProcRunning(ProcessHandle))
            {
                break;
            }

            FPlatformProcess::Sleep(0.05f);
        }

        if (!bUpdaterReady)
        {
            const bool bWasStillRunning = FPlatformProcess::IsProcRunning(ProcessHandle);
            if (bWasStillRunning)
            {
                FPlatformProcess::TerminateProc(ProcessHandle, true);
                FPlatformProcess::WaitForProc(ProcessHandle);
            }

            int32 ReturnCode = INDEX_NONE;
            FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
            FPlatformProcess::CloseProc(ProcessHandle);

            FString BootstrapLog;
            FFileHelper::LoadFileToString(BootstrapLog, *BootstrapLogPath);

            FString ProcessLog;
            FFileHelper::LoadFileToString(ProcessLog, *ProcessLogPath);

            FString Diagnostic = FString::Printf(
                TEXT("[Unreal launcher] External updater failed before readiness. ProcessId=%u ReturnCode=%d\r\n"),
                UpdaterProcessId,
                ReturnCode);

            if (!ProcessLog.IsEmpty())
            {
                Diagnostic += TEXT("\r\n=== Wrapper / PowerShell process log ===\r\n");
                Diagnostic += ProcessLog;
            }

            if (!BootstrapLog.IsEmpty())
            {
                Diagnostic += TEXT("\r\n=== PowerShell bootstrap log ===\r\n");
                Diagnostic += BootstrapLog;
            }

            if (ProcessLog.IsEmpty() && BootstrapLog.IsEmpty())
            {
                Diagnostic += TEXT(
                    "\r\nNeither the wrapper nor PowerShell produced a log. "
                    "The launcher may have been blocked before execution.\r\n");
            }

            FFileHelper::SaveStringToFile(
                Diagnostic,
                *BootstrapLogPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

            FString InlineDiagnostic = !ProcessLog.IsEmpty() ? ProcessLog : BootstrapLog;
            InlineDiagnostic.TrimStartAndEndInline();

            constexpr int32 MaxInlineDiagnosticChars = 1800;
            if (InlineDiagnostic.Len() > MaxInlineDiagnosticChars)
            {
                InlineDiagnostic =
                    InlineDiagnostic.Right(MaxInlineDiagnosticChars) +
                    TEXT("\n\n[Earlier diagnostic text omitted]");
            }

            OutError = FString::Printf(
                TEXT("The PD3 Modkit updater did not start correctly, so Unreal Editor was left open.\n\n"
                     "Startup diagnostics:\n%s\n\n"
                     "Full diagnostics:\n%s\n\n"
                     "Process log:\n%s"),
                InlineDiagnostic.IsEmpty()
                    ? TEXT("No launcher or PowerShell diagnostic text was produced.")
                    : *InlineDiagnostic,
                *BootstrapLogPath,
                *ProcessLogPath);
            return false;
        }

        FPlatformProcess::CloseProc(ProcessHandle);

        // The standalone updater window is now alive outside the project and is
        // explicitly waiting for this editor PID. It is safe to request a clean
        // editor shutdown. Git will only begin after that PID has disappeared.
        FPlatformMisc::RequestExit(false);
        return true;
    }

    static void PresentCheckResult(
        const FUpdateCheckResult& Result,
        const FCrimeFactoryEngineUpdateResult& EngineResult,
        const bool bUserInitiated)
    {
        if (bShuttingDown.Load())
        {
            return;
        }

        bCheckInProgress = false;

        FCrimeFactoryUpdatePresentation View;
        View.Engine = EngineResult;
        View.Branch = Result.BranchName;
        View.Repository = Result.RepositoryRoot;
        View.CurrentCommit = ShortCommit(Result.LocalCommit);
        View.LatestCommit = ShortCommit(Result.RemoteCommit);
        View.LatestSubject = Result.LatestCommitSubject;
        View.CommitsAhead = Result.CommitsAhead;
        View.CommitsBehind = Result.CommitsBehind;
        View.LocalChanges = Result.LocalWorkingTreeChanges;
        switch (Result.State)
        {
        case EUpdateState::UpToDate:
            if (!bUserInitiated && !EngineResult.bUpdateAvailable) return;
            View.Status = TEXT("Your modkit is up to date");
            View.Detail = TEXT("No official updates are waiting to be installed. Local commits and file changes are shown below.");
            break;
        case EUpdateState::Diverged:
            View.Status = TEXT("This update needs a manual merge");
            View.Severity = ECrimeFactoryPreflightSeverity::Warning;
            View.Detail = TEXT("The local and remote branches have diverged. Merge or rebase your local branch manually, then check again. CrimeFactory will not rewrite your commits.");
            break;
        case EUpdateState::UpdateAvailable:
            View.Status = Result.bResumePending ? TEXT("A modkit update needs recovery or rebuild") : TEXT("A modkit update is available");
            View.Severity = ECrimeFactoryPreflightSeverity::Warning;
            View.bCanUpdate = true;
            View.Detail = TEXT("Unreal Editor will save your work and close. The standalone updater will back up Content, preserve local files and staging in a recovery backup, update Git, restore compatible changes, regenerate project files, and clean and rebuild the project's C++ modules before reopening. The engine's C++ build toolchain must be installed. Local commits are never rewritten automatically. Changes that overlap upstream edits need manual review; their originals and staging remain recoverable. Pending updates resume without downloading again.");
            break;
        case EUpdateState::Error:
        default:
            UE_LOG(LogTemp, Warning, TEXT("PD3 Modkit update check failed: %s"), *Result.Error);
            if (!bUserInitiated && !EngineResult.bUpdateAvailable) return;
            View.Status = TEXT("The update check could not finish");
            View.Severity = ECrimeFactoryPreflightSeverity::Error;
            View.Detail = Result.Error;
            break;
        }
        if (CrimeFactoryUpdaterUI::Present(View) && View.bCanUpdate)
        {
            FString LaunchError;
            if (!LaunchUpdateHelperAndCloseEditor(Result, LaunchError))
            {
                View.bCanUpdate = false;
                View.Status = TEXT("The updater could not start");
                View.Detail = LaunchError;
                View.Severity = ECrimeFactoryPreflightSeverity::Error;
                CrimeFactoryUpdaterUI::Present(View);
            }
        }
    }
    static bool TickPendingCheckResult(float DeltaTime)
    {
        if (bShuttingDown.Load()) return false;
        if (!PendingCheckResult.IsSet() || !CrimeFactoryUpdaterUI::CanPresent()) return true;

        FPendingUpdatePresentation ResultToPresent = MoveTemp(PendingCheckResult.GetValue());
        PendingCheckResult.Reset();
        PresentCheckResult(ResultToPresent.Modkit, ResultToPresent.Engine, ResultToPresent.bUserInitiated);
        return !bShuttingDown.Load();
    }
}

void FPD3ModkitUpdater::Initialize()
{
    using namespace PD3ModkitUpdater;

    bShuttingDown.Store(false);

    if (IsRunningCommandlet() || FApp::IsUnattended())
    {
        return;
    }

    StartupTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateStatic(&FPD3ModkitUpdater::HandleStartupCheck),
        StartupCheckDelaySeconds);
    PresentationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateStatic(&TickPendingCheckResult));
}

void FPD3ModkitUpdater::Shutdown()
{
    using namespace PD3ModkitUpdater;

    bShuttingDown.Store(true);
    PendingCheckResult.Reset();

    if (PresentationTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(PresentationTickerHandle);
        PresentationTickerHandle.Reset();
    }

    if (StartupTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(StartupTickerHandle);
        StartupTickerHandle.Reset();
    }
}

bool FPD3ModkitUpdater::HandleStartupCheck(float DeltaTime)
{
    using namespace PD3ModkitUpdater;

    StartupTickerHandle.Reset();
    CheckForUpdates(false);
    return false;
}

void FPD3ModkitUpdater::CheckForUpdates(const bool bUserInitiated)
{
    using namespace PD3ModkitUpdater;

    if (bShuttingDown.Load() || IsRunningCommandlet() || FApp::IsUnattended())
    {
        return;
    }

    if (bCheckInProgress)
    {
        if (bUserInitiated)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::FromString(TEXT("A PD3 Modkit update check is already in progress.")),
                FText::FromString(TEXT("PD3 Modkit Updater")));
        }
        return;
    }

    bCheckInProgress = true;
    const FCrimeFactoryEngineUpdateSettings EngineSettings = FCrimeFactoryEngineUpdateSettings::Read();

    Async(EAsyncExecution::ThreadPool, [bUserInitiated, EngineSettings]()
    {
        // Start independently so a failed Git repository check cannot skip engine detection.
        auto EngineCheck = Async(EAsyncExecution::ThreadPool, [EngineSettings]()
        {
            return CrimeFactoryEngineUpdater::Check(EngineSettings);
        });
        const PD3ModkitUpdater::FUpdateCheckResult Result =
            PD3ModkitUpdater::PerformUpdateCheck();
        const FCrimeFactoryEngineUpdateResult EngineResult = EngineCheck.Get();

        AsyncTask(ENamedThreads::GameThread, [Result, EngineResult, bUserInitiated]()
        {
            if (!bShuttingDown.Load())
            {
                PendingCheckResult.Emplace(FPendingUpdatePresentation{Result, EngineResult, bUserInitiated});
            }
        });
    });
}
