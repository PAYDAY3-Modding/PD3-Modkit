#include "CrimeFactoryPawnPreview.h"

#include "CrimeFactoryVisualisationComponents.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ICookInfo.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogCrimeFactoryPawnPreview, Log, All);

namespace
{
    const FName PreviewTag(TEXT("CrimeFactory.PawnPreview"));
    const FName PreviewName(TEXT("CrimeFactoryPawnPreview"));

    UClass* RequestDataClass()
    {
        return FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZPawnSpawnRequestData"));
    }

    UScriptStruct* RequestStruct()
    {
        return FindObject<UScriptStruct>(nullptr, TEXT("/Script/Starbreeze.SBZPawnSpawnRequest"));
    }

    UObject* ObjectValue(const FProperty* Property, const void* Value)
    {
        if (!Property || !Value) { return nullptr; }
        if (const auto* Soft = CastField<FSoftObjectProperty>(Property))
        {
            return Soft->GetPropertyValue(Value).LoadSynchronous();
        }
        const auto* Object = CastField<FObjectPropertyBase>(Property);
        return Object ? Object->GetObjectPropertyValue(Value) : nullptr;
    }

    UObject* ObjectField(const UStruct* Type, const void* Data, FName Name)
    {
        const FProperty* Field = FindFProperty<FProperty>(Type, Name);
        return Field ? ObjectValue(Field, Field->ContainerPtrToValuePtr<void>(Data)) : nullptr;
    }

    bool BoolField(const UStruct* Type, const void* Data, FName Name)
    {
        const auto* Field = FindFProperty<FBoolProperty>(Type, Name);
        return Field && Field->GetPropertyValue_InContainer(Data);
    }

    template <typename T>
    T* ObjectField(const UObject& Object, FName Name)
    {
        return Cast<T>(ObjectField(Object.GetClass(), &Object, Name));
    }

    void ReadMaterials(const UObject& Object, TArray<UMaterialInterface*>& Out)
    {
        const auto* Array = FindFProperty<FArrayProperty>(Object.GetClass(), TEXT("OverrideMaterials"));
        if (!Array || !CastField<FObjectPropertyBase>(Array->Inner)) { return; }
        FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(&Object));
        Out.Reset(Values.Num());
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            Out.Add(Cast<UMaterialInterface>(ObjectValue(Array->Inner, Values.GetRawPtr(Index))));
        }
    }
}

void FCrimeFactoryPawnPreview::FAppearance::Keep(UObject* Object)
{
    if (Object && !Dependencies.Contains(FObjectKey(Object)))
    {
        Dependencies.Add(FObjectKey(Object));
        Assets.Emplace(Object);
    }
}

bool FCrimeFactoryPawnPreview::SupportsRequestProperty(const FProperty* Property)
{
    if (const auto* Struct = CastField<FStructProperty>(Property))
    {
        return Struct->Struct && Struct->Struct == RequestStruct();
    }
    if (const auto* Class = CastField<FClassProperty>(Property))
    {
        return Class->MetaClass && Class->MetaClass->IsChildOf(APawn::StaticClass());
    }
    if (const auto* Class = CastField<FSoftClassProperty>(Property))
    {
        return Class->MetaClass && Class->MetaClass->IsChildOf(APawn::StaticClass());
    }
    if (const auto* Object = CastField<FObjectPropertyBase>(Property))
    {
        UClass* DataClass = RequestDataClass();
        return Object->PropertyClass && ((DataClass && Object->PropertyClass->IsChildOf(DataClass))
            || Object->PropertyClass->IsChildOf(UBlueprint::StaticClass()));
    }
    return false;
}

void FCrimeFactoryPawnPreview::ReloadDefinitions()
{
    Definitions.Empty();
    DefinitionClasses.Empty();
    Cache.Empty();
    const auto* Settings = GetDefault<UCrimeFactoryActorVisualisationSettings>();
    bEnabled = Settings->bEnablePawnPreviews;
    Scale = FMath::IsFinite(Settings->PawnPreviewScale) ? FMath::Max(0.01f, Settings->PawnPreviewScale) : 1.0f;
    TSet<UClass*> Seen;
    FCookLoadScope EditorOnlyLoadScope(ECookLoadType::EditorOnly);
    for (const auto& Definition : Settings->PawnSpawnerDefinitions)
    {
        if (Definition.ActorClass.IsNull()) { continue; }
        UClass* Class = Definition.ActorClass.LoadSynchronous();
        if (!Class || !Class->IsChildOf(AActor::StaticClass()))
        {
            UE_LOG(LogCrimeFactoryPawnPreview, Warning, TEXT("Cannot resolve pawn preview spawner class %s"), *Definition.ActorClass.ToString());
            continue;
        }
        if (Seen.Contains(Class))
        {
            Definitions.Remove(Class);
            UE_LOG(LogCrimeFactoryPawnPreview, Warning, TEXT("Duplicate pawn preview definitions for %s ignored. Configure each class once."), *Class->GetPathName());
            continue;
        }
        Seen.Add(Class);
        Definitions.Add(Class, Definition.PawnRequestProperty);
        DefinitionClasses.Emplace(Class);
    }
    for (auto It = Warnings.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) { It.RemoveCurrent(); }
    }
}

UObject* FCrimeFactoryPawnPreview::ReadRequest(AActor& Actor, FName Name, FString& Description)
{
    const FProperty* Property = FindFProperty<FProperty>(Actor.GetClass(), Name);
    Description = Name.ToString();
    if (!SupportsRequestProperty(Property))
    {
        Description += TEXT(" is missing or has an unsupported type (expected a pawn class, spawn request, or spawn-request Data Asset)");
        return nullptr;
    }
    const void* Value = Property->ContainerPtrToValuePtr<void>(&Actor);
    UObject* Request = nullptr;
    if (const auto* Struct = CastField<FStructProperty>(Property))
    {
        Request = ObjectField(Struct->Struct, Value, TEXT("Data"));
    }
    else
    {
        Request = ObjectValue(Property, Value);
    }
    if (const auto* Blueprint = Cast<UBlueprint>(Request)) { Request = Blueprint->GeneratedClass; }
    Description += Request ? TEXT(" = ") + Request->GetPathName() : TEXT(" is unassigned or unavailable");
    return Request;
}

TSharedRef<FCrimeFactoryPawnPreview::FAppearance> FCrimeFactoryPawnPreview::Resolve(UObject* Request)
{
    const FObjectKey Key(Request);
    if (const auto* Found = Cache.Find(Key)) { return *Found; }
    auto Result = MakeShared<FAppearance>();
    Cache.Add(Key, Result);
    Result->Keep(Request);
    UClass* PawnClass = Cast<UClass>(Request);
    UObject* Variation = nullptr;
    const void* Interval = nullptr;
    UScriptStruct* IntervalType = nullptr;
    UClass* DataClass = RequestDataClass();
    if (DataClass && Request->IsA(DataClass))
    {
        const auto* Array = FindFProperty<FArrayProperty>(Request->GetClass(), TEXT("PawnVariationDatas"));
        const auto* Entry = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
        if (Entry)
        {
            FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Request));
            if (Values.Num())
            {
                // Deterministic representative: first authored pawn variation. Never randomize on refresh.
                const void* Data = Values.GetRawPtr(0);
                PawnClass = Cast<UClass>(ObjectField(Entry->Struct, Data, TEXT("PawnClass")));
                const AActor* Default = PawnClass && PawnClass->IsChildOf(APawn::StaticClass()) ? PawnClass->GetDefaultObject<AActor>() : nullptr;
                if (BoolField(Entry->Struct, Data, TEXT("bOverrideVariationData")))
                {
                    Variation = ObjectField(Entry->Struct, Data, TEXT("VariationData"));
                }
                else if (Default)
                {
                    Variation = ObjectField<UObject>(*Default, TEXT("VariationData"));
                }
                if (BoolField(Entry->Struct, Data, TEXT("bOverrideVariationDataIntervals")))
                {
                    const auto* Field = FindFProperty<FStructProperty>(Entry->Struct, TEXT("VariationCategoryIndexIntervals"));
                    if (Field) { Interval = Field->ContainerPtrToValuePtr<void>(Data); IntervalType = Field->Struct; }
                }
            }
            else
            {
                // Older data used the now-deprecated class array.
                const auto* Legacy = FindFProperty<FArrayProperty>(Request->GetClass(), TEXT("ClassVariationArray"));
                if (Legacy)
                {
                    FScriptArrayHelper Classes(Legacy, Legacy->ContainerPtrToValuePtr<void>(Request));
                    if (Classes.Num()) { PawnClass = Cast<UClass>(ObjectValue(Legacy->Inner, Classes.GetRawPtr(0))); }
                    if (PawnClass && PawnClass->IsChildOf(APawn::StaticClass()))
                    {
                        Variation = ObjectField<UObject>(*PawnClass->GetDefaultObject<AActor>(), TEXT("VariationData"));
                    }
                }
            }
        }
    }
    else if (PawnClass && PawnClass->IsChildOf(APawn::StaticClass()))
    {
        Variation = ObjectField<UObject>(*PawnClass->GetDefaultObject<AActor>(), TEXT("VariationData"));
    }
    if (!PawnClass || !PawnClass->IsChildOf(APawn::StaticClass()) || PawnClass->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated))
    {
        Result->Error = TEXT("The request has no available pawn class in its first authored variation.");
        return Result;
    }
    ReadPawn(PawnClass, Variation, Interval, IntervalType, *Result);
    return Result;
}

void FCrimeFactoryPawnPreview::ReadPawn(UClass* Class, UObject* Variation, const void* Interval,
    UScriptStruct* IntervalType, FAppearance& Out)
{
    AActor* Default = Class->GetDefaultObject<AActor>();
    Out.Keep(Class);
    Out.Keep(Default);
    Out.Keep(Variation);
    if (!Default) { Out.Error = TEXT("Pawn class defaults are unavailable."); return; }

    // Read templates, including inherited Blueprint SCS overrides, without constructing a pawn
    // or running a construction script. Custom gameplay mesh-component subclasses are never cloned.
    TArray<USceneComponent*> Templates;
    TMap<const USceneComponent*, const USceneComponent*> Parents;
    TMap<const USceneComponent*, FName> Sockets;
    TMap<FName, USceneComponent*> ByName;
    ForEachObjectWithOuter(Default, [&](UObject* Object)
    {
        if (auto* Component = Cast<USceneComponent>(Object))
        {
            Templates.AddUnique(Component);
            ByName.Add(Component->GetFName(), Component);
        }
    }, true);
    TMap<USCS_Node*, USceneComponent*> Nodes;
    auto* ActualClass = Cast<UBlueprintGeneratedClass>(Class);
    for (UClass* Current = Class; Current; Current = Current->GetSuperClass())
    {
        Out.Keep(Current);
        auto* BlueprintClass = Cast<UBlueprintGeneratedClass>(Current);
        if (!BlueprintClass || !BlueprintClass->SimpleConstructionScript) { continue; }
        for (USCS_Node* Node : BlueprintClass->SimpleConstructionScript->GetAllNodes())
        {
            auto* Template = Node ? Cast<USceneComponent>(Node->GetActualComponentTemplate(ActualClass)) : nullptr;
            if (!Template) { continue; }
            Nodes.Add(Node, Template);
            Templates.AddUnique(Template);
            ByName.Add(Node->GetVariableName(), Template);
        }
    }
    for (USceneComponent* Template : Templates)
    {
        Parents.Add(Template, Template->GetAttachParent());
        Sockets.Add(Template, Template->GetAttachSocketName());
        Out.Keep(Template);
    }
    for (const auto& Node : Nodes)
    {
        if (!Node.Key->ParentComponentOrVariableName.IsNone())
        {
            Parents[Node.Value] = ByName.FindRef(Node.Key->ParentComponentOrVariableName);
        }
        Sockets[Node.Value] = Node.Key->AttachToName;
        for (USCS_Node* Child : Node.Key->GetChildNodes())
        {
            if (auto* const* ChildTemplate = Nodes.Find(Child)) { Parents[*ChildTemplate] = Node.Value; }
        }
    }
    const USceneComponent* Root = Default->GetRootComponent();
    if (!Root)
    {
        for (const USceneComponent* Template : Templates)
        {
            if (!Parents.FindRef(Template)) { Root = Template; break; }
        }
    }
    if (Root) { Out.PawnScale = Root->GetRelativeScale3D(); }

    TMap<const USceneComponent*, int32> MeshIndices;
    const USkeletalMeshComponent* PrimaryMesh = nullptr;
    if (const auto* Character = Cast<ACharacter>(Default)) { PrimaryMesh = Character->GetMesh(); }
    int32 Primary = INDEX_NONE;
    for (USceneComponent* Template : Templates)
    {
        auto* Mesh = Cast<UMeshComponent>(Template);
        if (!Mesh || Mesh->IsEditorOnly()) { continue; }
        auto* Skeletal = Cast<USkeletalMeshComponent>(Mesh);
        auto* Static = Cast<UStaticMeshComponent>(Mesh);
        if (!Skeletal && !Static) { continue; }
        const int32 Index = Out.Parts.AddDefaulted();
        FPart& Part = Out.Parts[Index];
        MeshIndices.Add(Template, Index);
        Part.SkeletalMesh = Skeletal ? Skeletal->GetSkeletalMeshAsset() : nullptr;
        Part.StaticMesh = Static ? Static->GetStaticMesh() : nullptr;
        Part.bVisible = Mesh->GetVisibleFlag() && !Mesh->bHiddenInGame && !Mesh->bOnlyOwnerSee;
        Part.PrimitiveData = Mesh->GetCustomPrimitiveData().Data;
        for (UMaterialInterface* Material : Mesh->OverrideMaterials) { Part.Materials.Add(Material); }
        if (Skeletal && (Primary == INDEX_NONE || Skeletal == PrimaryMesh)) { Primary = Index; }
    }
    for (const auto& Mesh : MeshIndices)
    {
        FPart& Part = Out.Parts[Mesh.Value];
        const USceneComponent* Current = Mesh.Key;
        TSet<const USceneComponent*> Visited;
        while (Current && Current != Root)
        {
            if (Visited.Contains(Current)) { Out.Error = TEXT("Circular pawn component attachment hierarchy."); return; }
            Visited.Add(Current);
            Part.Transform = Part.Transform * Current->GetRelativeTransform();
            Part.Socket = Sockets.FindRef(Current);
            Current = Parents.FindRef(Current);
            if (const int32* ParentIndex = MeshIndices.Find(Current)) { Part.Parent = *ParentIndex; break; }
        }
    }

    if (Variation)
    {
        USkeletalMesh* Mesh = ObjectField<USkeletalMesh>(*Variation, TEXT("SkeletalMesh"));
        if (!Mesh) { Mesh = ObjectField<USkeletalMesh>(*Variation, TEXT("Mesh")); }
        UStaticMesh* Static = ObjectField<UStaticMesh>(*Variation, TEXT("StaticMesh"));
        if (Primary == INDEX_NONE && (Mesh || Static)) { Primary = Out.Parts.AddDefaulted(); }
        if (Primary != INDEX_NONE)
        {
            FPart& Part = Out.Parts[Primary];
            if (Mesh) { Part.SkeletalMesh = Mesh; Part.StaticMesh = nullptr; }
            if (Static && !Part.SkeletalMesh) { Part.StaticMesh = Static; }
            const auto* Materials = FindFProperty<FArrayProperty>(Variation->GetClass(), TEXT("OverrideMaterials"));
            if (Materials && FScriptArrayHelper(Materials, Materials->ContainerPtrToValuePtr<void>(Variation)).Num())
            {
                ReadMaterials(*Variation, Part.Materials);
            }
            const auto* Variations = FindFProperty<FArrayProperty>(Variation->GetClass(), TEXT("Variations"));
            const auto* ValueType = Variations ? CastField<FStructProperty>(Variations->Inner) : nullptr;
            if (ValueType)
            {
                FScriptArrayHelper Values(Variations, Variations->ContainerPtrToValuePtr<void>(Variation));
                if (!Interval)
                {
                    const auto* DefaultInterval = FindFProperty<FStructProperty>(Class, TEXT("VariationCategoryIndexIntervals"));
                    if (DefaultInterval) { Interval = DefaultInterval->ContainerPtrToValuePtr<void>(Default); IntervalType = DefaultInterval->Struct; }
                }
                int32 Representative = 0;
                if (Interval && IntervalType)
                {
                    const auto* Min = FindFProperty<FIntProperty>(IntervalType, TEXT("Min"));
                    const auto* Max = FindFProperty<FIntProperty>(IntervalType, TEXT("Max"));
                    if (Min && Max)
                    {
                        const int32 Lower = Min->GetPropertyValue_InContainer(Interval);
                        const int32 Upper = Max->GetPropertyValue_InContainer(Interval);
                        if (Lower >= 0)
                        {
                            if (Lower > Upper || !Values.IsValidIndex(Lower)) { Out.Error = TEXT("Appearance variation interval is out of range."); return; }
                            Representative = Lower;
                        }
                    }
                }
                if (Values.Num())
                {
                    const void* Value = Values.GetRawPtr(Representative);
                    const auto* Data = FindFProperty<FFloatProperty>(ValueType->Struct, TEXT("Data"));
                    if (Data)
                    {
                        Part.PrimitiveData.Reset(Data->ArrayDim);
                        for (int32 Index = 0; Index < Data->ArrayDim; ++Index)
                        {
                            Part.PrimitiveData.Add(Data->GetPropertyValue(Data->ContainerPtrToValuePtr<void>(Value, Index)));
                        }
                    }
                    const auto* Morphs = FindFProperty<FMapProperty>(ValueType->Struct, TEXT("MorphTargets"));
                    const auto* Name = Morphs ? CastField<FNameProperty>(Morphs->KeyProp) : nullptr;
                    const auto* Weight = Morphs ? CastField<FFloatProperty>(Morphs->ValueProp) : nullptr;
                    if (Name && Weight)
                    {
                        FScriptMapHelper Map(Morphs, Morphs->ContainerPtrToValuePtr<void>(Value));
                        for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
                        {
                            if (Map.IsValidIndex(Index)) { Part.MorphTargets.Add(Name->GetPropertyValue(Map.GetKeyPtr(Index)), Weight->GetPropertyValue(Map.GetValuePtr(Index))); }
                        }
                    }
                }
            }
        }
    }
    bool bAnyVisibleMesh = false;
    for (FPart& Part : Out.Parts)
    {
        Out.Keep(Part.SkeletalMesh);
        Out.Keep(Part.StaticMesh);
        for (UMaterialInterface* Material : Part.Materials) { Out.Keep(Material); }
        if (Part.SkeletalMesh && !Part.SkeletalMesh->GetRefSkeleton().GetNum()) { Out.Error = TEXT("A requested skeletal mesh has no usable reference skeleton."); return; }
        bAnyVisibleMesh |= Part.bVisible && (Part.SkeletalMesh || Part.StaticMesh);
    }
    if (!bAnyVisibleMesh) { Out.Error = TEXT("No visible mesh templates or appearance meshes are available; runtime-generated appearance cannot be previewed."); }
    // Ground the body, rather than an attachment (for example, a weapon extending below it).
    if (Out.Parts.IsValidIndex(Primary) && Out.Parts[Primary].bVisible &&
        (Out.Parts[Primary].SkeletalMesh || Out.Parts[Primary].StaticMesh))
    {
        Out.GroundingPart = Primary;
    }
    else
    {
        Out.GroundingPart = Out.Parts.IndexOfByPredicate([](const FPart& Part)
        {
            return Part.bVisible && (Part.SkeletalMesh || Part.StaticMesh);
        });
    }
}

TOptional<FTransform> FCrimeFactoryPawnPreview::GetSpawnTransform(const AActor& Actor)
{
    FTransform Spawn = Actor.GetActorTransform();
    const auto* Array = FindFProperty<FArrayProperty>(Actor.GetClass(), TEXT("SpawnDefinitionArray"));
    const auto* Entry = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
    if (Entry && Entry->Struct->GetPathName() == TEXT("/Script/Starbreeze.SBZPawnSpawnDefinition"))
    {
        const auto* Transform = FindFProperty<FStructProperty>(Entry->Struct, TEXT("Transform"));
        FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(&Actor));
        if (Values.Num() && Transform && Transform->Struct == TBaseStructure<FTransform>::Get())
        {
            Spawn = *Transform->ContainerPtrToValuePtr<FTransform>(Values.GetRawPtr(0));
        }
        const auto* Handling = FindFProperty<FStructProperty>(Actor.GetClass(), TEXT("RotationHandling"));
        if (Handling)
        {
            const void* Data = Handling->ContainerPtrToValuePtr<void>(&Actor);
            const auto* Method = FindFProperty<FEnumProperty>(Handling->Struct, TEXT("Method"));
            const auto* Heading = FindFProperty<FFloatProperty>(Handling->Struct, TEXT("HeadingAngle"));
            if (Method && Heading && Method->GetEnum()->GetNameStringByValue(Method->GetUnderlyingProperty()->GetSignedIntPropertyValue(Method->ContainerPtrToValuePtr<void>(Data))) == TEXT("SpecificHeadingAngle"))
            {
                const float Angle = Heading->GetPropertyValue_InContainer(Data);
                if (!FMath::IsFinite(Angle)) { return {}; }
                Spawn.SetRotation(FRotator(0, Angle, 0).Quaternion());
            }
        }
    }
    return Spawn.IsValid() ? TOptional<FTransform>(Spawn) : TOptional<FTransform>();
}

void FCrimeFactoryPawnPreview::Warn(AActor& Actor, const FString& Message)
{
    if (Warnings.FindRef(&Actor) != Message)
    {
        Warnings.Add(&Actor, Message);
        UE_LOG(LogCrimeFactoryPawnPreview, Warning, TEXT("%s: %s Billboard marker retained when configured."), *Actor.GetPathName(), *Message);
    }
}

void FCrimeFactoryPawnPreview::UpdateActor(AActor& Actor)
{
    FName Property;
    bool bMatched = false;
    for (UClass* Class = Actor.GetClass(); Class; Class = Class->GetSuperClass())
    {
        if (const FName* Found = Definitions.Find(Class)) { Property = *Found; bMatched = true; break; }
    }
    if (!bEnabled || !bMatched) { RemoveActor(Actor); Warnings.Remove(&Actor); return; }
    FCookLoadScope EditorOnlyLoadScope(ECookLoadType::EditorOnly);
    FString Description;
    UObject* Request = ReadRequest(Actor, Property, Description);
    if (!IsValid(Request)) { RemoveActor(Actor); Warn(Actor, Description); return; }
    TSharedRef<FAppearance> Appearance = Resolve(Request);
    const TOptional<FTransform> Spawn = GetSpawnTransform(Actor);
    if (!Appearance->Error.IsEmpty() || !Spawn.IsSet())
    {
        RemoveActor(Actor);
        Warn(Actor, Description + TEXT(": ") + (Spawn.IsSet() ? Appearance->Error : TEXT("Invalid spawn transform.")));
        return;
    }
    Warnings.Remove(&Actor);
    FActorPreview& Preview = Previews.FindOrAdd(&Actor);
    const bool bChanged = Preview.Appearance != Appearance;
    while (Preview.Components.Num() > Appearance->Parts.Num())
    {
        if (auto* Component = Preview.Components.Pop().Get()) { CrimeFactoryVisualisation::Destroy(*Component); }
    }
    Preview.Components.SetNum(Appearance->Parts.Num());
    TArray<bool> Reconfigure;
    Reconfigure.Init(bChanged, Appearance->Parts.Num());
    for (int32 Index = 0; Index < Appearance->Parts.Num(); ++Index)
    {
        const FPart& Part = Appearance->Parts[Index];
        UMeshComponent* Component = Preview.Components[Index].Get();
        if (Component && ((Part.SkeletalMesh != nullptr) != Component->IsA<USkeletalMeshComponent>()))
        {
            CrimeFactoryVisualisation::Destroy(*Component); Component = nullptr;
        }
        if (!Component)
        {
            UClass* Type = Part.SkeletalMesh ? USkeletalMeshComponent::StaticClass() : UStaticMeshComponent::StaticClass();
            Component = NewObject<UMeshComponent>(&Actor, Type, MakeUniqueObjectName(&Actor, Type, PreviewName), CrimeFactoryVisualisation::PreviewFlags);
            CrimeFactoryVisualisation::Configure(*Component, PreviewTag);
            // This cooked-editor engine's IsVisible() also gates editor skeletal meshes on
            // HiddenInGame. Runtime exclusion comes from editor-only/duplicate-transient flags,
            // editor-world filtering, and the shared pre-save/export/PIE teardown instead.
            Component->SetHiddenInGame(false);
            if (auto* Skeletal = Cast<USkeletalMeshComponent>(Component))
            {
                Skeletal->SetAnimationMode(EAnimationMode::AnimationCustomMode, false);
                Skeletal->bEnableAnimation = false;
                Skeletal->bPauseAnims = true;
                Skeletal->bForceRefpose = true;
                Skeletal->SetDisablePostProcessBlueprint(true);
                Skeletal->SetAllowClothActors(false);
                Skeletal->SetUpdateAnimationInEditor(false);
            }
            Preview.Components[Index] = Component;
            Reconfigure[Index] = true;
        }
        if (Reconfigure[Index])
        {
            if (auto* Skeletal = Cast<USkeletalMeshComponent>(Component))
            {
                Skeletal->SetSkeletalMesh(Part.SkeletalMesh);
                Skeletal->ClearMorphTargets();
                for (const auto& Morph : Part.MorphTargets) { Skeletal->SetMorphTarget(Morph.Key, Morph.Value); }
            }
            else { CastChecked<UStaticMeshComponent>(Component)->SetStaticMesh(Part.StaticMesh); }
            Component->EmptyOverrideMaterials();
            for (int32 Material = 0; Material < Part.Materials.Num(); ++Material) { Component->SetMaterial(Material, Part.Materials[Material]); }
            Component->SetCustomPrimitiveData(Part.PrimitiveData);
            Component->SetVisibility(Part.bVisible);
        }
    }
    FTransform PawnWorld = Spawn.GetValue();
    PawnWorld.SetScale3D(PawnWorld.GetScale3D() * Appearance->PawnScale * Scale);
    TSet<int32> Applied;
    TSet<int32> Applying;
    TFunction<bool(int32)> Attach = [&](int32 Index)
    {
        if (Applied.Contains(Index)) { return true; }
        if (Applying.Contains(Index)) { return false; }
        Applying.Add(Index);
        const FPart& Part = Appearance->Parts[Index];
        USceneComponent* Parent = Actor.GetRootComponent();
        FTransform Relative = Part.Transform;
        if (Part.Parent != INDEX_NONE)
        {
            if (!Appearance->Parts.IsValidIndex(Part.Parent) || !Attach(Part.Parent)) { return false; }
            Parent = Preview.Components[Part.Parent].Get();
        }
        else
        {
            const FTransform World = Part.Transform * PawnWorld;
            Relative = Parent ? World.GetRelativeTransform(Parent->GetComponentTransform()) : World;
        }
        UMeshComponent* Component = Preview.Components[Index].Get();
        if (Component->GetAttachParent() != Parent || Component->GetAttachSocketName() != Part.Socket)
        {
            if (Component->IsRegistered()) { Component->UnregisterComponent(); }
            Component->DetachFromComponent(FDetachmentTransformRules(EDetachmentRule::KeepRelative, false));
            Component->SetupAttachment(Parent, Part.Parent == INDEX_NONE ? NAME_None : Part.Socket);
        }
        Component->SetRelativeTransform(Relative);
        if (!Component->IsRegistered()) { Component->RegisterComponent(); }
        if (Reconfigure[Index])
        {
            if (auto* Skeletal = Cast<USkeletalMeshComponent>(Component))
            {
                // Evaluate the reference skeleton/morph overrides once, with no animation instance
                // or post-process Blueprint. Animation evaluation and component ticking stay off.
                Skeletal->bEnableAnimation = true;
                Skeletal->RefreshBoneTransforms();
                Skeletal->bEnableAnimation = false;
                Skeletal->MarkRenderDynamicDataDirty();
            }
        }
        Applying.Remove(Index);
        Applied.Add(Index);
        return true;
    };
    for (int32 Index = 0; Index < Appearance->Parts.Num(); ++Index)
    {
        if (!Attach(Index)) { RemoveActor(Actor); Warn(Actor, Description + TEXT(": Circular mesh hierarchy.")); return; }
    }
    if (Appearance->Parts.IsValidIndex(Appearance->GroundingPart))
    {
        const FPart& Body = Appearance->Parts[Appearance->GroundingPart];
        const UMeshComponent* BodyComponent = Preview.Components[Appearance->GroundingPart].Get();
        // Imported bounds exclude editor bounds extensions. Work in pawn space so the lift
        // respects spawn rotation, character scale and preview scale, including socket parents.
        const FBox MeshBounds = Body.SkeletalMesh ? Body.SkeletalMesh->GetImportedBounds().GetBox() : Body.StaticMesh->GetBoundingBox();
        const FBox PawnBounds = MeshBounds.TransformBy(BodyComponent->GetComponentTransform().GetRelativeTransform(PawnWorld));
        if (PawnBounds.IsValid && FMath::IsFinite(PawnBounds.Min.Z))
        {
            const FVector Lift = PawnWorld.TransformVector(FVector(0.0, 0.0, -PawnBounds.Min.Z));
            // Move only top-level preview components; their attached parts follow exactly once.
            for (int32 Index = 0; Index < Appearance->Parts.Num(); ++Index)
            {
                if (Appearance->Parts[Index].Parent == INDEX_NONE)
                {
                    Preview.Components[Index]->AddWorldOffset(Lift);
                }
            }
        }
    }
    Preview.Appearance = Appearance;
}

void FCrimeFactoryPawnPreview::RemoveActor(AActor& Actor)
{
    Previews.Remove(&Actor);
    TInlineComponentArray<UMeshComponent*> Components(&Actor);
    for (UMeshComponent* Component : Components)
    {
        if (CrimeFactoryVisualisation::IsManaged(Component, PreviewTag)) { CrimeFactoryVisualisation::Destroy(*Component); }
    }
}

void FCrimeFactoryPawnPreview::RemoveAll()
{
    TArray<TWeakObjectPtr<AActor>> Actors;
    GetActors(Actors);
    for (const auto& Actor : Actors) { if (Actor.IsValid()) { RemoveActor(*Actor); } }
    Previews.Empty();
}

void FCrimeFactoryPawnPreview::GetActors(TArray<TWeakObjectPtr<AActor>>& Out) const
{
    for (const auto& Preview : Previews) { Out.AddUnique(Preview.Key); }
}

bool FCrimeFactoryPawnPreview::InvalidateForObject(const UObject* Object)
{
    for (const UObject* Current = Object; Current; Current = Current->GetOuter())
    {
        for (const auto& Entry : Cache)
        {
            if (Entry.Value->Dependencies.Contains(FObjectKey(Current))) { Cache.Empty(); return true; }
        }
    }
    return false;
}

void FCrimeFactoryPawnPreview::Prune()
{
    for (auto It = Previews.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) { It.RemoveCurrent(); }
    }
    for (auto It = Warnings.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) { It.RemoveCurrent(); }
    }
}
