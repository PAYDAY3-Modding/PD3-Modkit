#pragma once

#include "CoreMinimal.h"
#include "Packaging/CrimeFactoryPackagingUI.h"

struct FCrimeFactoryEngineUpdateSettings
{
    FString Repository;
    FString RequiredEngineVersion;
    FString InstallerReleaseTag;
    FString EngineRoot;
    static FCrimeFactoryEngineUpdateSettings Read();
};

struct FCrimeFactoryEngineUpdateResult
{
    FString InstalledVersion;
    FString LatestVersion;
    FString EngineRoot;
    FString Status;
    FString Detail;
    FString ReleaseURL;
    FString InstallerURL;
    bool bUpdateAvailable = false;
    ECrimeFactoryPreflightSeverity Severity = ECrimeFactoryPreflightSeverity::Warning;
};

namespace CrimeFactoryEngineUpdater
{
    // Called on the updater's worker thread. Uses the installer's authenticated gh catalogue.
    FCrimeFactoryEngineUpdateResult Check(const FCrimeFactoryEngineUpdateSettings& Settings);
}
