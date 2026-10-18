#pragma once
#include "CoreMinimal.h"
#include "Packaging/CrimeFactoryPackagingUI.h"
#include "Updater/CrimeFactoryEngineUpdater.h"

struct FCrimeFactoryUpdatePresentation
{
    FCrimeFactoryEngineUpdateResult Engine;
    FString Status;
    FString Detail;
    FString Branch;
    FString Repository;
    FString CurrentCommit;
    FString LatestCommit;
    FString LatestSubject;
    int32 CommitsBehind = 0;
    int32 CommitsAhead = 0;
    int32 LocalChanges = 0;
    bool bCanUpdate = false;
    ECrimeFactoryPreflightSeverity Severity = ECrimeFactoryPreflightSeverity::Pass;
    FString BuildReport() const;
};

namespace CrimeFactoryUpdaterUI
{
    // Async checks must wait for a normal editor tick and a stable root window.
    bool CanPresent();
    TSharedRef<SWidget> MakeContent(const FCrimeFactoryUpdatePresentation& Data,
        const FSimpleDelegate& OnUpdate, const FSimpleDelegate& OnClose);
    bool Present(const FCrimeFactoryUpdatePresentation& Data);
}
