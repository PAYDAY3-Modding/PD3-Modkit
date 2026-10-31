#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UBillboardComponent;
class ULevel;
class UTexture2D;
class UWorld;

/** Owns only transient instance visualisations; never edits actor or Blueprint defaults. */
class FCrimeFactoryBillboardManager
{
public:
    void Initialize();
    void Shutdown();
    void RequestRefresh(bool bReloadDefinitions = false);

private:
    template <typename DelegateType, typename CallbackType>
    void Bind(DelegateType& Delegate, CallbackType&& Callback)
    {
        const FDelegateHandle Handle = Delegate.AddLambda(Forward<CallbackType>(Callback));
        Unbinders.Add([&Delegate, Handle]() { Delegate.Remove(Handle); });
    }

    struct FLevelHooks
    {
        FDelegateHandle Added;
        FDelegateHandle Removed;
    };

    bool Tick(float DeltaTime);
    void QueueActor(AActor* Actor);
    void TrackLevel(ULevel* Level);
    void ReloadDefinitions();
    void UpdateActor(AActor* Actor);
    void RemoveActor(AActor* Actor);
    void RemoveAll();
    void BeginActorExport();
    void EndActorExport();
    static bool IsEligible(const AActor* Actor);

    TMap<TWeakObjectPtr<UClass>, TStrongObjectPtr<UTexture2D>> Definitions;
    TArray<TStrongObjectPtr<UClass>> DefinitionClasses;
    TArray<TFunction<void()>> Unbinders;
    TMap<TWeakObjectPtr<AActor>, TWeakObjectPtr<UBillboardComponent>> Billboards;
    TMap<TWeakObjectPtr<ULevel>, FLevelHooks> Levels;
    TSet<TWeakObjectPtr<AActor>> PendingActors;
    FTSTicker::FDelegateHandle TickHandle;
    bool bScanWorlds = false;
    bool bReloadDefinitions = false;
    bool bSuspendedForPIE = false;
    bool bUpdating = false;
    int32 ActorExportDepth = 0;
    float Scale = 1.0f;
};
