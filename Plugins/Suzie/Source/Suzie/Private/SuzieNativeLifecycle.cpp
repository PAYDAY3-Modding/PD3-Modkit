#include "SuzieNativeLifecycle.h"
#include "SuziePlugin.h"
#include "Components/Widget.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"
#include <atomic>

namespace SuzieNativeLifecycle
{
    namespace
    {
        using FWeak = TWeakObjectPtr<UObject>;
        struct FCallback
        {
            FString ClassPath;
            ESuzieCallbackPhase Phase;
            FName Property;
            FBody Body;
            FString Source;
            bool bDisabled = false;
        };
        struct FObjectState
        {
            bool bInitialized = false;
            bool bObserveWidget = false;
            TWeakPtr<SWidget> LastSlate;
        };
        struct FState
        {
            TArray<FCallback> Callbacks;
            TMap<FString, TArray<int32>> ByPath;
            TMap<TWeakObjectPtr<UClass>, TArray<int32>> Matches;
            TQueue<FWeak, EQueueMode::Mpsc> Created;
            TSet<FWeak> Pending;
            TSet<FWeak> Refresh;
            TSet<FWeak> Active;
            TMap<FWeak, FObjectState> Objects;
            TSet<FWeak> ObservedWidgets;
            TMap<FWeak, TMap<FWeak, TSet<FName>>> Dependents;
            TMap<FWeak, TSet<FWeak>> Dependencies;
            TSet<FWeak> MissingRefreshWarnings;
            std::atomic<bool> bListening{false};
            bool bReady = false;
            FTSTicker::FDelegateHandle Tick;
            FDelegateHandle Constructed;
            FDelegateHandle PropertyChanged;
            FDelegateHandle Replaced;
            FDelegateHandle Collected;
            FDelegateHandle AssetLoaded;
            TQueue<FWeak, EQueueMode::Mpsc> Loaded;
        };
        FState& State() { static FState Value; return Value; }

        const TArray<int32>& Matching(UClass* Class)
        {
            FState& S = State();
            if (const auto* Found = S.Matches.Find(Class)) { return *Found; }
            TArray<UClass*, TInlineAllocator<16>> Hierarchy;
            for (UClass* Current = Class; Current; Current = Current->GetSuperClass()) { Hierarchy.Add(Current); }
            TArray<int32> Result;
            for (int32 I = Hierarchy.Num() - 1; I >= 0; --I)
            {
                if (const auto* Rows = S.ByPath.Find(Hierarchy[I]->GetPathName()))
                { for (int32 Row : *Rows) { if (!S.Callbacks[Row].bDisabled) { Result.Add(Row); } } }
            }
            return S.Matches.Add(Class, MoveTemp(Result));
        }

        bool HasPhase(UObject& Object, ESuzieCallbackPhase Phase)
        {
            for (int32 I : Matching(Object.GetClass())) { if (State().Callbacks[I].Phase == Phase) { return true; } }
            return false;
        }

        bool Ready(UObject& Object)
        {
            return IsValid(&Object) && !Object.HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad |
                RF_NeedPostLoadSubobjects | RF_BeginDestroyed | RF_FinishDestroyed) &&
                !Object.HasAnyInternalFlags(EInternalObjectFlags_AsyncLoading | EInternalObjectFlags::Async) &&
                !Object.GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated);
        }

        void Dispatch(UObject& Object, ESuzieCallbackPhase Phase, FName Member = NAME_None, FName Leaf = NAME_None)
        {
            FState& S = State();
            if (S.Active.Contains(&Object)) { return; }
            // Copy the list: callbacks may create classes/objects and invalidate the cache.
            const TArray<int32> Rows = Matching(Object.GetClass());
            S.Active.Add(&Object);
            for (int32 I : Rows)
            {
                const FCallback& Callback = S.Callbacks[I];
                if (Callback.Phase != Phase || (Phase == ESuzieCallbackPhase::PropertyChanged &&
                    Callback.Property != Member && Callback.Property != Leaf)) { continue; }
                FSuzieFunctionContext Context(&Object, Phase, Member, Leaf);
                Callback.Body(Context);
                if (!IsValid(&Object)) { break; }
            }
            S.Active.Remove(&Object);
        }

        void Created(UObject* Object)
        {
            FState& S = State();
            if (!S.bListening.load(std::memory_order_acquire) || !Object) { return; }
            // Async loading may construct objects here. No reflection dispatch or mutable
            // registry access is allowed on that thread; a weak candidate is queued only.
            S.Created.Enqueue(Object);
        }

        void AssetLoaded(UObject* Object)
        { if (State().bListening.load(std::memory_order_acquire) && Object) { State().Loaded.Enqueue(Object); } }

        void Changed(UObject* Object, FPropertyChangedEvent& Event)
        {
            FState& S = State();
            if (!S.bReady || !IsInGameThread() || !IsValid(Object) || IsGarbageCollecting()) { return; }
            const FName Leaf = Event.Property ? Event.Property->GetFName() : NAME_None;
            const FName Member = Event.MemberProperty ? Event.MemberProperty->GetFName() : Leaf;
            if (!S.Active.Contains(Object) && Ready(*Object))
            { Dispatch(*Object, ESuzieCallbackPhase::PropertyChanged, Member, Leaf); }
            if (const auto* Owners = S.Dependents.Find(Object))
            {
                for (const auto& Pair : *Owners)
                {
                    if (UObject* Owner = Pair.Key.Get(); Owner && (Pair.Value.Contains(NAME_None) ||
                        Pair.Value.Contains(Member) || Pair.Value.Contains(Leaf)))
                    { RequestRefresh(*Owner); }
                }
            }
        }

        void Prune()
        {
            FState& S = State();
            TArray<FWeak> Dead;
            for (const auto& Pair : S.Dependencies) { if (!Pair.Key.IsValid()) { Dead.Add(Pair.Key); } }
            for (FWeak Owner : Dead)
            {
                const TSet<FWeak> Assets = S.Dependencies.FindChecked(Owner);
                for (FWeak Asset : Assets)
                {
                    if (auto* Owners = S.Dependents.Find(Asset))
                    { Owners->Remove(Owner); if (Owners->IsEmpty()) { S.Dependents.Remove(Asset); } }
                }
                S.Dependencies.Remove(Owner);
            }
            for (auto It = S.Dependents.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid())
                {
                    for (const auto& Pair : It.Value())
                    {
                        if (auto* Assets = S.Dependencies.Find(Pair.Key)) { Assets->Remove(It.Key()); }
                        if (UObject* Owner = Pair.Key.Get()) { RequestRefresh(*Owner); }
                    }
                    It.RemoveCurrent();
                }
            }
            for (auto It = S.Objects.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
            for (auto It = S.ObservedWidgets.CreateIterator(); It; ++It) { if (!It->IsValid()) { It.RemoveCurrent(); } }
            for (auto It = S.Matches.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
            for (auto It = S.MissingRefreshWarnings.CreateIterator(); It; ++It) { if (!It->IsValid()) { It.RemoveCurrent(); } }
        }

        void Replaced(const FCoreUObjectDelegates::FReplacementObjectMap& Replacements)
        {
            FState& S = State();
            S.Matches.Empty();
            for (const auto& Pair : Replacements)
            {
                if (const auto* Owners = S.Dependents.Find(Pair.Key))
                { for (const auto& Owner : *Owners) { if (UObject* Target = Owner.Key.Get()) { RequestRefresh(*Target); } } }
                if (Pair.Key) { ClearDependencies(*Pair.Key); S.Objects.Remove(Pair.Key); S.ObservedWidgets.Remove(Pair.Key); S.Pending.Remove(Pair.Key); S.Refresh.Remove(Pair.Key); }
                if (IsValid(Pair.Value)) { S.Created.Enqueue(Pair.Value); RequestRefresh(*Pair.Value); }
            }
        }

        bool Tick(float)
        {
            FState& S = State();
            if (!S.bReady || IsGarbageCollecting() || GIsSavingPackage || FUObjectThreadContext::Get().IsRoutingPostLoad)
            { return true; }
            FWeak Candidate;
            int32 Candidates = 0;
            while (++Candidates <= 2048 && S.Created.Dequeue(Candidate))
            {
                if (UObject* Object = Candidate.Get(); Object &&
                    (HasPhase(*Object, ESuzieCallbackPhase::PostInitialize) || HasPhase(*Object, ESuzieCallbackPhase::Refresh)))
                { S.Pending.Add(Candidate); }
            }
            while (S.Loaded.Dequeue(Candidate))
            {
                if (UObject* Loaded = Candidate.Get(); Loaded &&
                    (HasPhase(*Loaded, ESuzieCallbackPhase::PostInitialize) || HasPhase(*Loaded, ESuzieCallbackPhase::Refresh)))
                { S.Pending.Add(Candidate); }
                if (const auto* Owners = S.Dependents.Find(Candidate))
                { for (const auto& Owner : *Owners) { if (UObject* Target = Owner.Key.Get()) { RequestRefresh(*Target); } } }
            }
            const double Deadline = FPlatformTime::Seconds() + 0.008;
            // Take a snapshot: callbacks may construct more objects or request refreshes.
            const TArray<FWeak> Pending = S.Pending.Array();
            for (FWeak Weak : Pending)
            {
                UObject* Object = Weak.Get();
                if (!Object) { S.Pending.Remove(Weak); continue; }
                if (!Ready(*Object)) { continue; }
                S.Pending.Remove(Weak);
                FObjectState& Info = S.Objects.FindOrAdd(Weak);
                if (!Info.bInitialized)
                {
                    Info.bInitialized = true;
                    Dispatch(*Object, ESuzieCallbackPhase::PostInitialize);
                }
                if (FPlatformTime::Seconds() >= Deadline) { break; }
            }
            // Only explicitly observed widgets are checked. No scan of all loaded UObjects.
            for (FWeak Weak : S.ObservedWidgets)
            {
                FObjectState* Info = S.Objects.Find(Weak);
                if (auto* Widget = Cast<UWidget>(Weak.Get()); Info && Widget && Ready(*Widget))
                {
                    const TSharedPtr<SWidget> Current = Widget->GetCachedWidget();
                    if (Info->LastSlate.Pin() != Current)
                    { Info->LastSlate = Current; if (Current) { S.Refresh.Add(Weak); } }
                }
            }
            const TArray<FWeak> Refreshes = S.Refresh.Array();
            for (FWeak Weak : Refreshes)
            {
                UObject* Object = Weak.Get();
                if (!Object) { S.Refresh.Remove(Weak); continue; }
                if (!Ready(*Object)) { continue; }
                const FObjectState* Info = S.Objects.Find(Weak);
                if (!Info || !Info->bInitialized) { S.Pending.Add(Weak); continue; }
                S.Refresh.Remove(Weak);
                Dispatch(*Object, ESuzieCallbackPhase::Refresh);
                if (FPlatformTime::Seconds() >= Deadline) { break; }
            }
            return true;
        }
    }

    void Register(const TCHAR* ClassPath, ESuzieCallbackPhase Phase, FName Property, FBody Body, const TCHAR* Source)
    {
        FState& S = State();
        if (S.bListening.load() || !Body || !ClassPath || !FString(ClassPath).StartsWith(TEXT("/Script/")))
        { UE_LOG(LogSuzie, Error, TEXT("Invalid or late lifecycle registration (%s)"), Source); return; }
        TArray<int32>& Rows = S.ByPath.FindOrAdd(ClassPath);
        bool bDuplicate = false;
        for (int32 I : Rows)
        {
            FCallback& Existing = S.Callbacks[I];
            if (Existing.Phase == Phase && Existing.Property == Property)
            { Existing.bDisabled = true; bDuplicate = true; }
        }
        const int32 Index = S.Callbacks.Add({ ClassPath, Phase, Property, Body, Source, bDuplicate });
        Rows.Add(Index);
        if (bDuplicate) { UE_LOG(LogSuzie, Error, TEXT("Duplicate lifecycle callback for %s / %s (%s); both disabled"), ClassPath, *Property.ToString(), Source); }
    }

    void Start()
    {
        FState& S = State();
        if (S.bListening.load() || S.Callbacks.IsEmpty() || IsRunningCommandlet()) { return; }
        S.bListening.store(true, std::memory_order_release);
        const bool bDeferred = S.Callbacks.ContainsByPredicate([](const FCallback& C)
        { return C.Phase == ESuzieCallbackPhase::PostInitialize || C.Phase == ESuzieCallbackPhase::Refresh; });
        if (bDeferred)
        {
            S.Constructed = FCoreUObjectDelegates::OnObjectConstructed.AddStatic(&Created);
            S.AssetLoaded = FCoreUObjectDelegates::OnAssetLoaded.AddStatic(&AssetLoaded);
            S.Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
        }
        S.PropertyChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&Changed);
        S.Replaced = FCoreUObjectDelegates::OnObjectsReplaced.AddStatic(&Replaced);
        S.Collected = FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(&Prune);
    }

    void InitializeClass(UObject& Object, const UClass& DeclaringClass)
    {
        // Closest safe constructor equivalent: Suzie-declared storage has been initialized;
        // archetype/JMAP/serialized defaults are still copied afterwards. Native construction
        // thread rules apply; Slate/gameplay work belongs in deferred post-initialization.
        FState& S = State();
        if (S.Callbacks.IsEmpty() || IsRunningCommandlet()) { return; }
        if (const auto* Rows = S.ByPath.Find(DeclaringClass.GetPathName()))
        {
            for (int32 I : *Rows)
            {
                const FCallback& Callback = S.Callbacks[I];
                if (!Callback.bDisabled && Callback.Phase == ESuzieCallbackPhase::Construct)
                {
                    FSuzieFunctionContext Context(&Object, ESuzieCallbackPhase::Construct, NAME_None, NAME_None, &DeclaringClass);
                    Callback.Body(Context);
                }
            }
        }
    }

    void FinishBindings()
    {
        FState& S = State();
        if (S.Callbacks.IsEmpty()) { return; }
        for (FCallback& Callback : S.Callbacks)
        {
            if (Callback.bDisabled) { continue; }
            UClass* Class = FindObject<UClass>(nullptr, *Callback.ClassPath);
            if (!Class || !Class->HasMetaData(TEXT("SuzieDynamicClass")) ||
                (Callback.Phase == ESuzieCallbackPhase::PropertyChanged && !FindFProperty<FProperty>(Class, Callback.Property)))
            {
                Callback.bDisabled = true;
                UE_LOG(LogSuzie, Error, TEXT("Lifecycle target class/property not found: %s / %s (%s)"),
                    *Callback.ClassPath, *Callback.Property.ToString(), *Callback.Source);
            }
        }
        S.Matches.Empty();
        if (IsRunningCommandlet()) { return; }
        S.bReady = true;
        // Native CDOs were constructed while definitions were being restored. They now have
        // final JMAP values. Existing instances on startup/rebinding are discovered once only.
        for (TObjectIterator<UObject> It(RF_NoFlags); It; ++It)
        {
            if (HasPhase(**It, ESuzieCallbackPhase::PostInitialize) || HasPhase(**It, ESuzieCallbackPhase::Refresh))
            { S.Pending.Add(*It); }
        }
    }

    void ClearDependencies(UObject& Owner)
    {
        FState& S = State();
        if (const auto* Existing = S.Dependencies.Find(&Owner))
        {
            const TSet<FWeak> Assets = *Existing;
            for (FWeak Asset : Assets)
            {
                if (auto* Owners = S.Dependents.Find(Asset))
                { Owners->Remove(&Owner); if (Owners->IsEmpty()) { S.Dependents.Remove(Asset); } }
            }
            S.Dependencies.Remove(&Owner);
        }
    }

    void WatchDependency(UObject& Owner, UObject* Dependency, FName Property)
    {
        FState& S = State();
        if (!S.bReady || !IsValid(Dependency) || Dependency == &Owner) { return; }
        if (!HasPhase(Owner, ESuzieCallbackPhase::Refresh))
        {
            if (!S.MissingRefreshWarnings.Contains(&Owner))
            {
                S.MissingRefreshWarnings.Add(&Owner);
                UE_LOG(LogSuzie, Warning, TEXT("%s: WatchDependency requires a SUZIE_REFRESH callback"), *Owner.GetPathName());
            }
            return;
        }
        S.Dependents.FindOrAdd(Dependency).FindOrAdd(&Owner).Add(Property);
        S.Dependencies.FindOrAdd(&Owner).Add(Dependency);
    }

    void RequestRefresh(UObject& Object)
    {
        FState& S = State();
        if (S.bReady && IsValid(&Object) && !S.Active.Contains(&Object) && HasPhase(Object, ESuzieCallbackPhase::Refresh))
        { S.Refresh.Add(&Object); }
    }

    bool ObserveWidget(UObject& Object)
    {
        auto* Widget = Cast<UWidget>(&Object);
        if (!Widget || !State().bReady || !HasPhase(Object, ESuzieCallbackPhase::Refresh)) { return false; }
        FObjectState& Info = State().Objects.FindOrAdd(&Object);
        if (!Info.bObserveWidget) { Info.bObserveWidget = true; Info.LastSlate = Widget->GetCachedWidget(); }
        State().ObservedWidgets.Add(&Object);
        return true;
    }

    void Reset()
    {
        FState& S = State();
        S.bListening.store(false, std::memory_order_release);
        S.bReady = false;
        FCoreUObjectDelegates::OnObjectConstructed.Remove(S.Constructed);
        FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(S.PropertyChanged);
        FCoreUObjectDelegates::OnObjectsReplaced.Remove(S.Replaced);
        FCoreUObjectDelegates::GetPostGarbageCollect().Remove(S.Collected);
        FCoreUObjectDelegates::OnAssetLoaded.Remove(S.AssetLoaded);
        if (S.Tick.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(S.Tick); S.Tick.Reset(); }
        FWeak Discard; while (S.Created.Dequeue(Discard)) {}
        while (S.Loaded.Dequeue(Discard)) {}
        S.Callbacks.Empty(); S.ByPath.Empty(); S.Matches.Empty(); S.Pending.Empty(); S.Refresh.Empty();
        S.Active.Empty(); S.Objects.Empty(); S.ObservedWidgets.Empty(); S.Dependents.Empty(); S.Dependencies.Empty(); S.MissingRefreshWarnings.Empty();
    }
}
