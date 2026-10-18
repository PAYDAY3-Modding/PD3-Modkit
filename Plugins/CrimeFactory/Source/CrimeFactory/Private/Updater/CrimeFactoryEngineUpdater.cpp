#include "Updater/CrimeFactoryEngineUpdater.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace CrimeFactoryEngineUpdater
{
    static bool ParseVersion(const FString& Text, TArray<uint32>& Out)
    {
        TArray<FString> Parts;
        Text.ParseIntoArray(Parts, TEXT("."), false);
        if (Parts.Num() < 2 || Parts.Num() > 4) return false;
        Out.Init(0, 4);
        for (int32 Index = 0; Index < Parts.Num(); ++Index)
        {
            if (Parts[Index].IsEmpty() || !Parts[Index].IsNumeric() || Parts[Index].Len() > 9) return false;
            for (TCHAR Character : Parts[Index]) if (Character < '0' || Character > '9') return false;
            Out[Index] = FCString::Strtoui64(*Parts[Index], nullptr, 10);
        }
        return true;
    }

    static int32 CompareVersions(const TArray<uint32>& A, const TArray<uint32>& B)
    {
        for (int32 Index = 0; Index < 4; ++Index)
            if (A[Index] != B[Index]) return A[Index] > B[Index] ? 1 : -1;
        return 0;
    }

    static bool ReadJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
    {
        FString Text;
        return FFileHelper::LoadFileToString(Text, *Path)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
    }

    static bool ReadVersion(const FString& Path, const FCrimeFactoryEngineUpdateSettings& Settings,
        const bool bRequirePath, FString& Out)
    {
        TSharedPtr<FJsonObject> Json;
        if (!ReadJson(Path, Json)) return false;
        FString Product, EngineVersion, InstallPath, Tag;
        Json->TryGetStringField(TEXT("product"), Product);
        Json->TryGetStringField(TEXT("engineVersion"), EngineVersion);
        Json->TryGetStringField(TEXT("releaseVersion"), Out);
        Json->TryGetStringField(TEXT("releaseTag"), Tag);
        Json->TryGetStringField(TEXT("installPath"), InstallPath);
        TArray<uint32> Parsed;
        return Product == TEXT("UE5-PD3") && EngineVersion == Settings.RequiredEngineVersion
            && ParseVersion(Out, Parsed)
            && Tag == FString::Printf(TEXT("UE5-PD3-%s-v%s"), *EngineVersion, *Out)
            && (!bRequirePath || (!InstallPath.IsEmpty() && FPaths::IsSamePath(InstallPath, Settings.EngineRoot)));
    }

    static bool ReadCatalogue(const FString& Repository, FString& Output, FString& Error)
    {
        // Use the same GitHub CLI credentials as the installer, including private source access.
        // Separate pipes avoid mixing error text into JSON. Bound the check so offline checks finish.
        void* OutRead = nullptr; void* OutWrite = nullptr;
        void* ErrRead = nullptr; void* ErrWrite = nullptr;
        if (!FPlatformProcess::CreatePipe(OutRead, OutWrite) || !FPlatformProcess::CreatePipe(ErrRead, ErrWrite))
        {
            FPlatformProcess::ClosePipe(OutRead, OutWrite);
            FPlatformProcess::ClosePipe(ErrRead, ErrWrite);
            Error = TEXT("Could not create the engine update check process pipes.");
            return false;
        }
        const FString Args = FString::Printf(TEXT("api \"repos/%s/releases?per_page=100\" --paginate --slurp"), *Repository);
        FProcHandle Process = FPlatformProcess::CreateProc(TEXT("gh.exe"), *Args, false, true, true,
            nullptr, 0, nullptr, OutWrite, nullptr, ErrWrite);
        if (!Process.IsValid()) Error = TEXT("Install GitHub CLI (gh) and sign in using the UE5-PD3 installer, then check again.");
        else
        {
            const double Deadline = FPlatformTime::Seconds() + 45.0;
            while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
            {
                Output += FPlatformProcess::ReadPipe(OutRead);
                Error += FPlatformProcess::ReadPipe(ErrRead);
                FPlatformProcess::Sleep(0.02f);
            }
            if (FPlatformProcess::IsProcRunning(Process))
            {
                FPlatformProcess::TerminateProc(Process, true);
                Error = TEXT("The engine release check timed out. Check your connection and GitHub CLI authentication.");
            }
            else
            {
                Output += FPlatformProcess::ReadPipe(OutRead);
                Error += FPlatformProcess::ReadPipe(ErrRead);
                int32 Code = -1;
                FPlatformProcess::GetProcReturnCode(Process, &Code);
                if (Code == 0) Error.Reset();
                else if (Error.IsEmpty()) Error = TEXT("GitHub CLI could not read engine releases. Verify access in the UE5-PD3 installer.");
            }
            FPlatformProcess::CloseProc(Process);
        }
        FPlatformProcess::ClosePipe(OutRead, OutWrite);
        FPlatformProcess::ClosePipe(ErrRead, ErrWrite);
        return Error.IsEmpty();
    }

    FCrimeFactoryEngineUpdateResult Check(const FCrimeFactoryEngineUpdateSettings& Settings)
    {
        FCrimeFactoryEngineUpdateResult Result;
        Result.EngineRoot = Settings.EngineRoot;
        Result.Status = TEXT("Installed engine version unknown");
        // Reject arguments in repository configuration before passing it to an external process.
        TArray<FString> RepoParts;
        Settings.Repository.ParseIntoArray(RepoParts, TEXT("/"), false);
        bool bValidRepo = RepoParts.Num() == 2;
        for (TCHAR C : Settings.Repository)
            bValidRepo &= FChar::IsAlnum(C) || C == '/' || C == '-' || C == '_' || C == '.';
        bool bValidInstallerTag = !Settings.InstallerReleaseTag.IsEmpty();
        for (TCHAR C : Settings.InstallerReleaseTag)
            bValidInstallerTag &= FChar::IsAlnum(C) || C == '-' || C == '_' || C == '.';
        if (!bValidRepo || RepoParts.Contains(FString()) || !bValidInstallerTag)
        {
            Result.Detail = TEXT("Configure a valid owner/repository in [CrimeFactory.EngineUpdates] in DefaultEditor.ini.");
            return Result;
        }
        Result.InstallerURL = FString::Printf(TEXT("https://github.com/%s/releases/tag/%s"),
            *Settings.Repository, *Settings.InstallerReleaseTag);
        if (!IFileManager::Get().FileExists(*(Settings.EngineRoot / TEXT("Engine/Binaries/Win64/UnrealEditor.exe"))))
        {
            Result.Status = TEXT("Engine installation could not be located");
            Result.Detail = TEXT("The running engine root does not contain UnrealEditor.exe. Use the engine installer to locate or repair the installation.");
            return Result;
        }
        TSharedPtr<FJsonObject> BuildVersion;
        double Major = 0, Minor = 0, Patch = 0;
        if (!ReadJson(Settings.EngineRoot / TEXT("Engine/Build/Build.version"), BuildVersion)
            || !BuildVersion->TryGetNumberField(TEXT("MajorVersion"), Major)
            || !BuildVersion->TryGetNumberField(TEXT("MinorVersion"), Minor)
            || !BuildVersion->TryGetNumberField(TEXT("PatchVersion"), Patch)
            || FString::Printf(TEXT("%d.%d.%d"), int32(Major), int32(Minor), int32(Patch)) != Settings.RequiredEngineVersion)
        {
            Result.Status = TEXT("This engine does not match the modkit requirement");
            Result.Detail = FString::Printf(TEXT("This modkit requires Unreal Engine %s. Use a compatible UE5-PD3 installation; no update comparison was made."), *Settings.RequiredEngineVersion);
            return Result;
        }
        if (IFileManager::Get().FileExists(*(Settings.EngineRoot / TEXT("Engine/Build/PD3Update.inprogress"))))
        {
            Result.Status = TEXT("Engine update needs repair");
            Result.Detail = TEXT("An engine deployment did not finish. Close the editor and rerun the installer before relying on its recorded version.");
            return Result;
        }
        const FString LocalMetadata = Settings.EngineRoot / TEXT("Engine/Build/PD3Release.json");
        bool bKnown = ReadVersion(LocalMetadata, Settings, false, Result.InstalledVersion);
        // Old installers stored one global record. Accept it only for this exact installation.
        if (!IFileManager::Get().FileExists(*LocalMetadata))
        {
            for (const TCHAR* Variable : {TEXT("LOCALAPPDATA"), TEXT("PROGRAMDATA")})
            {
                const FString Root = FPlatformMisc::GetEnvironmentVariable(Variable);
                if (!Root.IsEmpty() && ReadVersion(Root / TEXT("UE5-PD3/installed.json"), Settings, true, Result.InstalledVersion))
                { bKnown = true; break; }
            }
        }
        if (!bKnown) Result.InstalledVersion.Reset();
        FString Catalogue, Error;
        if (!ReadCatalogue(Settings.Repository, Catalogue, Error))
        {
            Result.Status = TEXT("Engine update check failed");
            Result.Detail = Error;
            Result.Severity = ECrimeFactoryPreflightSeverity::Error;
            return Result;
        }
        TArray<TSharedPtr<FJsonValue>> Pages;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Catalogue), Pages))
        {
            Result.Detail = TEXT("The engine release catalogue was not valid JSON. Update GitHub CLI and retry.");
            return Result;
        }
        TArray<uint32> Latest;
        const FString Prefix = FString::Printf(TEXT("UE5-PD3-%s-v"), *Settings.RequiredEngineVersion);
        for (const auto& Page : Pages)
        {
            const TArray<TSharedPtr<FJsonValue>>* Releases = nullptr;
            if (!Page->TryGetArray(Releases)) continue;
            for (const auto& Value : *Releases)
            {
                const TSharedPtr<FJsonObject>* Release = nullptr;
                if (!Value->TryGetObject(Release)) continue;
                FString Tag; bool bDraft = true, bPrerelease = true;
                if (!(*Release)->TryGetStringField(TEXT("tag_name"), Tag)
                    || !(*Release)->TryGetBoolField(TEXT("draft"), bDraft)
                    || !(*Release)->TryGetBoolField(TEXT("prerelease"), bPrerelease)
                    || bDraft || bPrerelease || !Tag.StartsWith(Prefix)) continue;
                TArray<uint32> Version;
                const FString VersionText = Tag.Mid(Prefix.Len());
                if (!ParseVersion(VersionText, Version)) continue;
                if (Latest.IsEmpty() || CompareVersions(Version, Latest) > 0)
                {
                    Latest = Version;
                    Result.LatestVersion = VersionText;
                    Result.ReleaseURL = FString::Printf(TEXT("https://github.com/%s/releases/tag/%s"), *Settings.Repository, *Tag);
                }
            }
        }
        if (Latest.IsEmpty())
        {
            Result.Detail = FString::Printf(TEXT("No stable UE5-PD3 %s releases were found. Other Unreal versions and prereleases are excluded."), *Settings.RequiredEngineVersion);
            return Result;
        }
        if (!bKnown)
        {
            Result.Detail = TEXT("No valid release manifest was found for this engine. Its UE Build.version is not a UE5-PD3 release version. Locate or repair this exact installation in the installer; no update comparison was made.");
            return Result;
        }
        TArray<uint32> Installed;
        ParseVersion(Result.InstalledVersion, Installed);
        Result.bUpdateAvailable = CompareVersions(Latest, Installed) > 0;
        Result.Status = Result.bUpdateAvailable ? TEXT("An engine update is available") : TEXT("Engine is up to date");
        Result.Severity = Result.bUpdateAvailable ? ECrimeFactoryPreflightSeverity::Warning : ECrimeFactoryPreflightSeverity::Pass;
        Result.Detail = TEXT("Engine releases are checked independently of modkit Git commits. Stable releases for the required Unreal version are selected. Use the existing engine installer for verified incremental patches; it checks the available patch chain before installation. Close every editor using this engine before applying an update. Opening the installer page does not install an update.");
        return Result;
    }
}

FCrimeFactoryEngineUpdateSettings FCrimeFactoryEngineUpdateSettings::Read()
{
    FCrimeFactoryEngineUpdateSettings Result;
    const TCHAR* Section = TEXT("CrimeFactory.EngineUpdates");
    GConfig->GetString(Section, TEXT("Repository"), Result.Repository, GEditorIni);
    GConfig->GetString(Section, TEXT("RequiredEngineVersion"), Result.RequiredEngineVersion, GEditorIni);
    GConfig->GetString(Section, TEXT("InstallerReleaseTag"), Result.InstallerReleaseTag, GEditorIni);
    Result.EngineRoot = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT(".."));
    FPaths::NormalizeDirectoryName(Result.EngineRoot);
    FPaths::CollapseRelativeDirectories(Result.EngineRoot);
    return Result;
}
