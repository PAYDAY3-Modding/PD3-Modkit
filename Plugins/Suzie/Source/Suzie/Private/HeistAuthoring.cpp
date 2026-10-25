#include "HeistAuthoring.h"

#include "SuziePlugin.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

void SuzieHeistAuthoring::CopyActorTickConfiguration(AActor& Destination, const AActor& Source)
{
    FActorTickFunction& To = Destination.PrimaryActorTick;
    const FActorTickFunction& From = Source.PrimaryActorTick;
    To.TickGroup = From.TickGroup;
    To.EndTickGroup = From.EndTickGroup;
    To.bTickEvenWhenPaused = From.bTickEvenWhenPaused;
    To.bCanEverTick = From.bCanEverTick;
    To.bStartWithTickEnabled = From.bStartWithTickEnabled;
    To.bAllowTickOnDedicatedServer = From.bAllowTickOnDedicatedServer;
    To.TickInterval = From.TickInterval;
}

TArray<TArray<int32>> SuzieHeistAuthoring::GetOpeningSquadSizes(const UObject& Settings)
{
    TArray<TArray<int32>> Result;
    const FStructProperty* Difficulty = FindFProperty<FStructProperty>(Settings.GetClass(), TEXT("DifficultySettingsArray"));
    const FArrayProperty* Progressions = Difficulty
        ? FindFProperty<FArrayProperty>(Difficulty->Struct, TEXT("ProgressionArray")) : nullptr;
    const FStructProperty* Progression = Progressions ? CastField<FStructProperty>(Progressions->Inner) : nullptr;
    const FArrayProperty* Squads = Progression
        ? FindFProperty<FArrayProperty>(Progression->Struct, TEXT("SquadArray")) : nullptr;
    const FStructProperty* Squad = Squads ? CastField<FStructProperty>(Squads->Inner) : nullptr;
    const FArrayProperty* Pawns = Squad ? FindFProperty<FArrayProperty>(Squad->Struct, TEXT("PawnArray")) : nullptr;
    const FNumericProperty* Weight = Squad ? FindFProperty<FNumericProperty>(Squad->Struct, TEXT("Weight")) : nullptr;
    if (!Pawns || !Weight || !Weight->IsFloatingPoint()) return Result;

    for (int32 Index = 0; Index < Difficulty->ArrayDim; ++Index)
    {
        TArray<int32>& Sizes = Result.AddDefaulted_GetRef();
        const void* Data = Difficulty->ContainerPtrToValuePtr<void>(&Settings, Index);
        FScriptArrayHelper ProgressionArray(Progressions, Progressions->ContainerPtrToValuePtr<void>(Data));
        if (!ProgressionArray.Num()) continue;
        FScriptArrayHelper SquadArray(Squads, Squads->ContainerPtrToValuePtr<void>(ProgressionArray.GetRawPtr(0)));
        for (int32 SquadIndex = 0; SquadIndex < SquadArray.Num(); ++SquadIndex)
        {
            const void* Entry = SquadArray.GetRawPtr(SquadIndex);
            if (Weight->GetFloatingPointPropertyValue(Weight->ContainerPtrToValuePtr<void>(Entry)) <= 0.0) continue;
            const int32 Size = FScriptArrayHelper(Pawns, Pawns->ContainerPtrToValuePtr<void>(Entry)).Num();
            if (Size > 0) Sizes.Add(Size);
        }
    }
    return Result;
}

namespace
{
    bool HasNativeAncestor(const AActor& Actor, const TCHAR* Name)
    {
        for (const UClass* Class = Actor.GetClass(); Class; Class = Class->GetSuperClass())
        {
            if (Class->GetPathName() == Name) return true;
        }
        return false;
    }

    bool IsEscape(const AActor& Actor)
    {
        return HasNativeAncestor(Actor, TEXT("/Script/Starbreeze.SBZPlayerEscapeVolume"));
    }

    bool IsSpawner(const AActor& Actor)
    {
        return HasNativeAncestor(Actor, TEXT("/Script/Starbreeze.SBZPawnSpawnBase"));
    }

    void LogField(const AActor& Actor, const TCHAR* Name)
    {
        const FProperty* Property = FindFProperty<FProperty>(Actor.GetClass(), Name);
        if (!Property) return;
        FString Value;
        Property->ExportText_InContainer(0, Value, &Actor, nullptr, nullptr, PPF_None);
        UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] %s %s=%s"), *Actor.GetPathName(), Name, *Value);
    }

    void AuditHeist()
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World) return;
        int32 Escapes = 0, Groups = 0, Spawners = 0;
        int32 LargestEnabledCapacity = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            const AActor& Actor = **It;
            if (IsEscape(Actor))
            {
                ++Escapes;
                UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] %s tick can=%d start=%d group=%d collision=%d"),
                    *Actor.GetPathName(), Actor.PrimaryActorTick.bCanEverTick,
                    Actor.PrimaryActorTick.bStartWithTickEnabled, int32(Actor.PrimaryActorTick.TickGroup),
                    Actor.GetActorEnableCollision());
                for (const TCHAR* Name : {TEXT("OnEscapeTimerDone"), TEXT("EscapeTimeDuration"),
                    TEXT("bShouldStartEnabled"), TEXT("PlayerPawnParentClass")}) LogField(Actor, Name);
            }
            if (HasNativeAncestor(Actor, TEXT("/Script/Starbreeze.PD3PawnSpawnGroup")))
            {
                ++Groups;
                for (const TCHAR* Name : {TEXT("bIsEnabledForAssault"), TEXT("AllowedPawnTypeContainer"),
                    TEXT("PawnSpawnArray"), TEXT("SpawnGroupRoom")}) LogField(Actor, Name);
                int32 Capacity = 0;
                FArrayProperty* Points = FindFProperty<FArrayProperty>(Actor.GetClass(), TEXT("PawnSpawnArray"));
                FObjectPropertyBase* Point = Points ? CastField<FObjectPropertyBase>(Points->Inner) : nullptr;
                if (Point)
                {
                    FScriptArrayHelper Helper(Points, Points->ContainerPtrToValuePtr<void>(&Actor));
                    for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    {
                        UObject* Spawner = Point->GetObjectPropertyValue(Helper.GetRawPtr(Index));
                        FArrayProperty* Definitions = Spawner
                            ? FindFProperty<FArrayProperty>(Spawner->GetClass(), TEXT("SpawnDefinitionArray")) : nullptr;
                        if (Definitions)
                        {
                            Capacity += FScriptArrayHelper(Definitions,
                                Definitions->ContainerPtrToValuePtr<void>(Spawner)).Num();
                        }
                    }
                }
                UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] %s serialized definition capacity=%d "
                    "(retail initialization additionally filters invalid/destroyed spawners)"),
                    *Actor.GetPathName(), Capacity);
                const FBoolProperty* Enabled = FindFProperty<FBoolProperty>(Actor.GetClass(), TEXT("bIsEnabledForAssault"));
                if (Enabled && Enabled->GetPropertyValue_InContainer(&Actor))
                    LargestEnabledCapacity = FMath::Max(LargestEnabledCapacity, Capacity);
            }
            if (IsSpawner(Actor))
            {
                ++Spawners;
                LogField(Actor, TEXT("SpawnDefinitionArray"));
                LogField(Actor, TEXT("WeightedLocation"));
            }
        }
        const AWorldSettings* WorldSettings = World->GetWorldSettings();
        const FObjectPropertyBase* SettingsProperty = WorldSettings
            ? FindFProperty<FObjectPropertyBase>(WorldSettings->GetClass(), TEXT("AssaultManagerSettings")) : nullptr;
        const UObject* Settings = SettingsProperty ? SettingsProperty->GetObjectPropertyValue_InContainer(WorldSettings) : nullptr;
        if (Settings)
        {
            UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Assigned assault settings=%s; largest enabled serialized group capacity=%d"),
                *Settings->GetPathName(), LargestEnabledCapacity);
            const TArray<TArray<int32>> Sizes = SuzieHeistAuthoring::GetOpeningSquadSizes(*Settings);
            for (int32 DifficultyIndex = 0; DifficultyIndex < Sizes.Num(); ++DifficultyIndex)
            {
                if (Sizes[DifficultyIndex].IsEmpty()) continue;
                int32 Minimum = MAX_int32, Maximum = 0;
                for (int32 Size : Sizes[DifficultyIndex])
                {
                    Minimum = FMath::Min(Minimum, Size);
                    Maximum = FMath::Max(Maximum, Size);
                }
                UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Difficulty[%d] opening squads=%d sizes min=%d max=%d"),
                    DifficultyIndex, Sizes[DifficultyIndex].Num(), Minimum, Maximum);
                if (LargestEnabledCapacity < Minimum)
                {
                    UE_LOG(LogSuzie, Warning, TEXT("[HeistAudit] CAPACITY MISMATCH Difficulty[%d]: no enabled editor group "
                        "can fit any opening squad (capacity=%d, smallest request=%d). Add prepared positions/references or "
                        "intentionally configure smaller squads; recook. This check does not measure retail registration."),
                        DifficultyIndex, LargestEnabledCapacity, Minimum);
                }
                else if (LargestEnabledCapacity < Maximum)
                {
                    UE_LOG(LogSuzie, Warning, TEXT("[HeistAudit] CAPACITY MISMATCH Difficulty[%d]: opening squads up to %d "
                        "exceed the largest enabled editor group capacity %d; some requests cannot fit."),
                        DifficultyIndex, Maximum, LargestEnabledCapacity);
                }
            }
        }
        UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Editor snapshot: escapes=%d groups=%d spawners=%d. "
            "This does not measure retail manager registration or runtime timer state."), Escapes, Groups, Spawners);
    }

    void PrepareActors(const TArray<AActor*>& Actors)
    {
        if (!GEditor) return;
        UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Preparing %d actors."), Actors.Num());
        const FScopedTransaction Transaction(NSLOCTEXT("Suzie", "PrepareHeistActors", "Prepare selected heist actors"));
        int32 Changed = 0;
        for (AActor* Actor : Actors)
        {
            // Original mounted reference maps must remain read-only.
            if (!Actor) continue;
            if (Actor->GetPackage()->HasAnyPackageFlags(PKG_Cooked))
            {
                UE_LOG(LogSuzie, Warning, TEXT("[HeistAudit] Skipped cooked reference %s. Use an authored map."),
                    *Actor->GetPathName());
                continue;
            }
            bool bChanged = false;
            if (IsEscape(*Actor) && Actor->GetClass()->GetPathName() == TEXT("/Script/Starbreeze.SBZPlayerEscapeVolume"))
            {
                Actor->Modify();
                SuzieHeistAuthoring::CopyActorTickConfiguration(*Actor, *Actor->GetClass()->GetDefaultObject<AActor>());
                bChanged = true;
            }
            if (IsSpawner(*Actor))
            {
                bChanged |= SuzieHeistAuthoring::UpdateSpawnAuthoringData(*Actor, false);
            }
            if (bChanged)
            {
                Actor->MarkPackageDirty();
                ++Changed;
                UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Prepared %s. Review and save the authored map; recook before testing."),
                    *Actor->GetPathName());
            }
        }
        UE_LOG(LogSuzie, Display, TEXT("[HeistAudit] Prepared %d selected actors. Existing nonempty spawn definitions "
            "and Blueprint tick overrides are preserved. No packages were saved automatically."), Changed);
    }

    void PrepareSelected()
    {
        if (!GEditor) return;
        TArray<AActor*> Actors;
        GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Actors);
        PrepareActors(Actors);
    }

    void PrepareNamed(const TArray<FString>& Names)
    {
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!World || Names.IsEmpty()) return;
        TArray<AActor*> Actors;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (Names.Contains(It->GetName())) Actors.Add(*It);
        }
        if (Actors.Num() != Names.Num())
        {
            UE_LOG(LogSuzie, Warning, TEXT("[HeistAudit] Named preparation matched %d of %d names; no actors changed."),
                Actors.Num(), Names.Num());
            return;
        }
        PrepareActors(Actors);
    }

    FAutoConsoleCommand AuditCommand(TEXT("Suzie.Heists.Audit"),
        TEXT("Log loaded editor escape volumes, groups and serialized spawn data; not a retail runtime trace."),
        FConsoleCommandDelegate::CreateStatic(&AuditHeist));
    FAutoConsoleCommand PrepareCommand(TEXT("Suzie.Heists.PrepareSelected"),
        TEXT("Undoable: reset selected native escape tick configuration and initialize empty selected spawn definitions at actor transforms. Does not save."),
        FConsoleCommandDelegate::CreateStatic(&PrepareSelected));
    FAutoConsoleCommand PrepareNamedCommand(TEXT("Suzie.Heists.PrepareNamed"),
        TEXT("Scripted preparation of exact actor names in the current editor world; uses the same undoable operation as PrepareSelected."),
        FConsoleCommandWithArgsDelegate::CreateStatic(&PrepareNamed));
}
