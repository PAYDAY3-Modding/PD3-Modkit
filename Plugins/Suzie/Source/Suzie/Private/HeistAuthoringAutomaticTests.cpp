#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace
{
    struct FSpawnFields
    {
        FArrayProperty* Array;
        FStructProperty* Transform;
        FStructProperty* Weighted;
        explicit FSpawnFields(const UClass* Class)
        {
            Array = FindFProperty<FArrayProperty>(Class, TEXT("SpawnDefinitionArray"));
            const FStructProperty* Inner = CastFieldChecked<FStructProperty>(Array->Inner);
            Transform = FindFProperty<FStructProperty>(Inner->Struct, TEXT("Transform"));
            Weighted = FindFProperty<FStructProperty>(Class, TEXT("WeightedLocation"));
        }
        FScriptArrayHelper Definitions(AActor& Actor) const
        { return FScriptArrayHelper(Array, Array->ContainerPtrToValuePtr<void>(&Actor)); }
        FTransform& Definition(AActor& Actor, int32 Index = 0) const
        { return *Transform->ContainerPtrToValuePtr<FTransform>(Definitions(Actor).GetRawPtr(Index)); }
        FVector& Location(AActor& Actor) const { return *Weighted->ContainerPtrToValuePtr<FVector>(&Actor); }
    };

    struct FWorldFixture
    {
        UClass* SpawnerClass = LoadObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZPawnSpawnPredefined"));
        UWorld* World;
        explicit FWorldFixture(EWorldType::Type Type = EWorldType::Editor)
        {
            const FString Name = TEXT("/Temp/HeistAuthoring_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            World = UWorld::CreateWorld(Type, false, TEXT("AutoFixture"), CreatePackage(*Name));
            World->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            World->PersistentLevel->bIsVisible = true;
        }
        ~FWorldFixture() { World->DestroyWorld(false); }
        AActor* Spawn(UClass* Class = nullptr, FVector Location = FVector(125, 250, 375))
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags |= RF_Transactional;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            return World->SpawnActor<AActor>(Class ? Class : SpawnerClass, Location, FRotator(0, 45, 0), Params);
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuzieHeistAutomaticPlacementTest,
    "Suzie.HeistAuthoring.AutomaticPlacementAndBlueprintChildren",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSuzieHeistAutomaticPlacementTest::RunTest(const FString& Parameters)
{
    FWorldFixture Fixture;
    if (!TestNotNull(TEXT("Retail spawn class resolves"), Fixture.SpawnerClass)) return false;
    FSpawnFields Fields(Fixture.SpawnerClass);
    TestEqual(TEXT("Class defaults remain empty"), Fields.Definitions(*Fixture.SpawnerClass->GetDefaultObject<AActor>()).Num(), 0);
    AActor* Native = Fixture.Spawn();
    if (!TestNotNull(TEXT("Native point placed"), Native)) return false;
    if (!TestEqual(TEXT("Placement callback generated one definition"), Fields.Definitions(*Native).Num(), 1)) return false;
    TestTrue(TEXT("Placed transform automatically stored"), Fields.Definition(*Native).Equals(Native->GetActorTransform()));
    Native->SetActorTransform(FTransform(FRotator(10, 90, 5), FVector(700, 800, 900), FVector(2, 2, 2)));
    GEngine->BroadcastOnActorMoved(Native);
    TestTrue(TEXT("Move/rotate/scale callback updates generated transform"), Fields.Definition(*Native).Equals(Native->GetActorTransform()));
    TestTrue(TEXT("Move updates weighted location"), Fields.Location(*Native).Equals(Native->GetActorLocation()));

    UBlueprint* Derived = FKismetEditorUtilities::CreateBlueprint(Fixture.SpawnerClass, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("AutoSpawnDerived")),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    AActor* BPPoint = Fixture.Spawn(Derived->GeneratedClass);
    if (!TestEqual(TEXT("Blueprint placement also generates data"), Fields.Definitions(*BPPoint).Num(), 1)) return false;
    TestTrue(TEXT("Blueprint uses final placed transform"), Fields.Definition(*BPPoint).Equals(BPPoint->GetActorTransform()));

    UBlueprint* Parent = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("AutoSpawnParent")),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    USCS_Node* Root = Parent->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("Root"));
    Parent->SimpleConstructionScript->AddNode(Root);
    USCS_Node* Child = Parent->SimpleConstructionScript->CreateNode(UChildActorComponent::StaticClass(), TEXT("SpawnPoint"));
    CastChecked<UChildActorComponent>(Child->ComponentTemplate)->SetChildActorClass(Fixture.SpawnerClass);
    Root->AddChildNode(Child);
    FKismetEditorUtilities::CompileBlueprint(Parent);
    AActor* ParentActor = Fixture.Spawn(Parent->GeneratedClass, FVector(2000, 3000, 4000));
    TArray<AActor*> Children;
    ParentActor->GetAllChildActors(Children, true);
    if (!TestEqual(TEXT("Blueprint child actor constructed"), Children.Num(), 1)) return false;
    if (!TestEqual(TEXT("Child point automatically prepared"), Fields.Definitions(*Children[0]).Num(), 1)) return false;
    GEngine->BroadcastOnActorMoved(ParentActor);
    TestTrue(TEXT("Child final construction transform stored"), Fields.Definition(*Children[0]).Equals(Children[0]->GetActorTransform()));
    ParentActor->SetActorLocation(FVector(4000, 5000, 6000));
    GEngine->BroadcastOnActorMoved(ParentActor);
    TestTrue(TEXT("Parent movement updates child point"), Fields.Definition(*Children[0]).Equals(Children[0]->GetActorTransform()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuzieHeistAutomaticCustomAndUndoTest,
    "Suzie.HeistAuthoring.AutomaticCustomDataAndUndo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSuzieHeistAutomaticCustomAndUndoTest::RunTest(const FString& Parameters)
{
    FWorldFixture Fixture;
    FSpawnFields Fields(Fixture.SpawnerClass);
    AActor* Point = Fixture.Spawn();
    if (!TestEqual(TEXT("Generated point"), Fields.Definitions(*Point).Num(), 1)) return false;
    const FTransform Original = Point->GetActorTransform();
    {
        FScopedTransaction Transaction(NSLOCTEXT("Suzie", "TestAutomaticSpawnMove", "Test automatic spawn movement"));
        Point->Modify(); Point->GetRootComponent()->Modify();
        Point->SetActorLocation(FVector(1000, 2000, 3000));
        GEngine->BroadcastOnActorMoved(Point);
    }
    TestTrue(TEXT("Undo move transaction"), GEditor->UndoTransaction());
    TestTrue(TEXT("Undo restores actor transform"), Point->GetActorTransform().Equals(Original));
    TestTrue(TEXT("Undo restores generated definition"), Fields.Definition(*Point).Equals(Original));
    Point->SetActorLocation(FVector(1100, 2200, 3300));
    GEngine->BroadcastOnActorMoved(Point);
    TestTrue(TEXT("Automatic tracking survives undo"), Fields.Definition(*Point).Equals(Point->GetActorTransform()));

    const FTransform Custom(FRotator(0, 180, 0), FVector(9000, 8000, 7000));
    Fields.Definition(*Point) = Custom;
    Point->SetActorLocation(FVector(10, 20, 30)); GEngine->BroadcastOnActorMoved(Point);
    TestTrue(TEXT("Custom single definition is preserved"), Fields.Definition(*Point).Equals(Custom));
    Fields.Definitions(*Point).AddValue();
    Fields.Definition(*Point, 1) = Original;
    Point->SetActorLocation(FVector(40, 50, 60)); GEngine->BroadcastOnActorMoved(Point);
    TestEqual(TEXT("Custom array length preserved"), Fields.Definitions(*Point).Num(), 2);
    TestTrue(TEXT("Both custom transforms preserved"), Fields.Definition(*Point).Equals(Custom) && Fields.Definition(*Point, 1).Equals(Original));

    FWorldFixture Preview(EWorldType::EditorPreview);
    AActor* PreviewActor = Preview.Spawn();
    TestEqual(TEXT("Preview actor untouched"), Fields.Definitions(*PreviewActor).Num(), 0);
    Fixture.World->GetPackage()->SetPackageFlags(PKG_Cooked);
    AActor* CookedActor = Fixture.Spawn();
    TestEqual(TEXT("Cooked reference package untouched"), Fields.Definitions(*CookedActor).Num(), 0);
    Fixture.World->GetPackage()->ClearPackageFlags(PKG_Cooked);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSuzieHeistAutomaticSaveTest,
    "Suzie.HeistAuthoring.AutomaticSaveAndReload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSuzieHeistAutomaticSaveTest::RunTest(const FString& Parameters)
{
    FWorldFixture Fixture;
    FSpawnFields Fields(Fixture.SpawnerClass);
    AActor* Point = Fixture.Spawn();
    Fields.Definitions(*Point).EmptyValues(); // Previously saved empty point.
    UClass* EscapeClass = LoadObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZPlayerEscapeVolume"));
    AActor* Escape = Fixture.Spawn(EscapeClass);
    Escape->PrimaryActorTick.bCanEverTick = false;
    Escape->PrimaryActorTick.bStartWithTickEnabled = true;
    Escape->PrimaryActorTick.TickGroup = TG_PrePhysics;
    UPackage* Package = Fixture.World->GetPackage();
    const FString MountPoint = TEXT("/SuzieHeistAutoTest/");
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HeistAuthoringAutomatic/"));
    FPackageName::RegisterMountPoint(MountPoint, Directory);
    ON_SCOPE_EXIT { FPackageName::UnRegisterMountPoint(MountPoint, Directory); };
    const FString FixtureName = TEXT("SaveFixture_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Package->Rename(*(MountPoint + FixtureName), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
    Package->MarkAsFullyLoaded(); // Entire package was created in memory by this fixture.
    const FString OriginalPackageName = Package->GetName();
    const FString PointName = Point->GetName();
    const FString Filename = Directory / (FixtureName + TEXT(".umap"));
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    // Keep a normal save path, but don't queue localization/asset validation
    // for a disposable map that is unmounted immediately after this test.
    UClass* ValidationClass = FindObject<UClass>(nullptr, TEXT("/Script/DataValidation.DataValidationSettings"));
    UObject* ValidationSettings = ValidationClass ? ValidationClass->GetDefaultObject() : nullptr;
    FBoolProperty* ValidateOnSave = ValidationClass ? FindFProperty<FBoolProperty>(ValidationClass, TEXT("bValidateOnSave")) : nullptr;
    const bool bOriginalValidation = ValidateOnSave && ValidateOnSave->GetPropertyValue_InContainer(ValidationSettings);
    if (ValidateOnSave) ValidateOnSave->SetPropertyValue_InContainer(ValidationSettings, false);
    ON_SCOPE_EXIT { if (ValidateOnSave) ValidateOnSave->SetPropertyValue_InContainer(ValidationSettings, bOriginalValidation); };
    if (!TestTrue(TEXT("Save authored fixture"), UPackage::SavePackage(Package, Fixture.World, *Filename, Args))) return false;
    TestEqual(TEXT("Save prepares legacy empty point"), Fields.Definitions(*Point).Num(), 1);
    TestTrue(TEXT("Save repairs old native escape fingerprint"), Escape->PrimaryActorTick.bCanEverTick);

    Package->Rename(*FString::Printf(TEXT("/Temp/Previous_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
    UPackage* ReloadedPackage = LoadPackage(nullptr, *OriginalPackageName, LOAD_None);
    if (!TestNotNull(TEXT("Saved fixture reloads"), ReloadedPackage)) return false;
    UWorld* ReloadedWorld = UWorld::FindWorldInPackage(ReloadedPackage);
    if (!TestNotNull(TEXT("Reloaded world found"), ReloadedWorld)) return false;
    ReloadedWorld->WorldType = EWorldType::Editor;
    AActor* ReloadedPoint = FindObject<AActor>(ReloadedWorld->PersistentLevel, *PointName);
    if (!TestNotNull(TEXT("Reloaded spawn point found"), ReloadedPoint)) return false;
    ReloadedPoint->SetActorLocation(FVector(6000, 7000, 8000));
    GEngine->BroadcastOnActorMoved(ReloadedPoint);
    TestTrue(TEXT("Generated tracking persists across save/reload"), Fields.Definition(*ReloadedPoint).Equals(ReloadedPoint->GetActorTransform()));
    ReloadedWorld->DestroyWorld(false);
    return true;
}
#endif
