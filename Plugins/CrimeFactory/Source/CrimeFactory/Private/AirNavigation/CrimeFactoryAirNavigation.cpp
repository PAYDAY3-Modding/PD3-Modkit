#include "AirNavigation/CrimeFactoryAirNavigation.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "HAL/IConsoleManager.h"
#include "Misc/MessageDialog.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogCrimeFactoryAirNav, Log, All);

namespace
{
    constexpr uint32 InvalidPackedNode = MAX_uint32;
    constexpr uint16 InvalidLinkId = MAX_uint16;
    constexpr int32 MaximumNodesPerTree = 65527; // eight children must fit uint16 indices

    TAutoConsoleVariable<int32> CVarAirNavMinHalfSize(
        TEXT("CrimeFactory.AirNav.MinHalfSize"), 64,
        TEXT("Minimum half-size in Unreal units for experimental CrimeFactory air navigation generation."),
        ECVF_Default);

    struct FNode
    {
        FIntVector Center = FIntVector::ZeroValue; // relative to tree centre, signed 16-bit on disk
        uint16 HalfSize = 0;
        uint16 FirstChildIndex = 0;
        bool bLeaf = false;
        bool bBlocked = false;
        TMap<uint8, TArray<uint16>> Neighbors; // TreeIndex -> Node indices
        TArray<uint16> LinkIds;
    };

    struct FTree
    {
        FVector Center = FVector::ZeroVector;
        uint16 MinimumHalfSize = 64;
        TArray<FNode> Nodes;
    };

    struct FLink
    {
        UActorComponent* Component = nullptr;
        uint32 LeftIndex = InvalidPackedNode;
        uint32 RightIndex = InvalidPackedNode;
        TArray<FVector> WorldPoints;
    };

    bool IsNativeType(const UClass* Class, const TCHAR* TypePath)
    {
        for (; Class; Class = Class->GetSuperClass())
        {
            if (Class->GetPathName() == TypePath)
            {
                return true;
            }
        }
        return false;
    }

    bool IsCooked(const UObject* Object)
    {
        return Object && Object->GetOutermost()->HasAnyPackageFlags(PKG_Cooked);
    }

    FArrayProperty* ArrayField(const UStruct* Owner, const TCHAR* Name)
    {
        return Owner ? FindFProperty<FArrayProperty>(Owner, Name) : nullptr;
    }

    FStructProperty* StructField(const UStruct* Owner, const TCHAR* Name)
    {
        return Owner ? FindFProperty<FStructProperty>(Owner, Name) : nullptr;
    }

    bool SetInteger(const UStruct* Owner, void* Container, const TCHAR* Name, uint64 Value)
    {
        FNumericProperty* Property = Owner ? FindFProperty<FNumericProperty>(Owner, Name) : nullptr;
        if (!Property || !Property->IsInteger())
        {
            return false;
        }
        Property->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Container), Value);
        return true;
    }

    bool SetFlag(const UStruct* Owner, void* Container, const TCHAR* Name, bool Value)
    {
        FBoolProperty* Property = Owner ? FindFProperty<FBoolProperty>(Owner, Name) : nullptr;
        if (!Property)
        {
            return false;
        }
        Property->SetPropertyValue_InContainer(Container, Value);
        return true;
    }

    bool GetVector(const UObject* Object, const TCHAR* Name, FVector& Out)
    {
        FStructProperty* Property = Object ? StructField(Object->GetClass(), Name) : nullptr;
        if (!Property || Property->Struct != TBaseStructure<FVector>::Get())
        {
            return false;
        }
        Out = *Property->ContainerPtrToValuePtr<FVector>(Object);
        return true;
    }

    bool SetVector(const UStruct* Owner, void* Container, const TCHAR* Name, const FVector& Value)
    {
        FStructProperty* Property = StructField(Owner, Name);
        if (!Property || Property->Struct != TBaseStructure<FVector>::Get())
        {
            return false;
        }
        Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Container), &Value);
        return true;
    }

    // Mirrors the eight-child order found in vanilla BEBE and the retail traversal function.
    constexpr int8 ChildSign[8][3] = {
        {-1,-1,+1}, {+1,-1,+1}, {+1,+1,+1}, {-1,+1,+1},
        {-1,-1,-1}, {+1,-1,-1}, {+1,+1,-1}, {-1,+1,-1}
    };

    bool IsBlocked(UWorld& World, const FVector& Center, uint16 HalfSize)
    {
        // The retail binary calls an overlap query with a box; the original collision object
        // mask is not yet verified. This editor-side approximation must be tested in-game.
        FCollisionQueryParams Params(SCENE_QUERY_STAT(CrimeFactoryAirNav), false);
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        TArray<FOverlapResult> Overlaps;
        World.OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, Objects,
            FCollisionShape::MakeBox(FVector(static_cast<double>(HalfSize))), Params);
        for (const FOverlapResult& Hit : Overlaps)
        {
            const UPrimitiveComponent* Primitive = Hit.GetComponent();
            if (Primitive && Primitive->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block &&
                !IsNativeType(Primitive->GetOwner() ? Primitive->GetOwner()->GetClass() : nullptr,
                    TEXT("/Script/Starbreeze.SBZAirNavigationBoundsVolume")))
            {
                return true;
            }
        }
        return false;
    }

    bool GenerateNode(UWorld& World, FTree& Tree, int32 Index)
    {
        // Index instead of a reference: adding nodes reallocates the array.
        const FNode Original = Tree.Nodes[Index];
        FNode& Node = Tree.Nodes[Index];
        const FVector Center = Tree.Center + FVector(Original.Center);
        Node.bBlocked = IsBlocked(World, Center, Original.HalfSize);
        Node.bLeaf = !Node.bBlocked || Original.HalfSize <= Tree.MinimumHalfSize;
        if (Node.bLeaf)
        {
            return true;
        }
        if (Tree.Nodes.Num() > MaximumNodesPerTree - 8)
        {
            UE_LOG(LogCrimeFactoryAirNav, Error, TEXT("Octree node count would exceed 16-bit indexing capacity."));
            return false;
        }
        const uint16 ChildHalf = Original.HalfSize / 2;
        if (ChildHalf == 0)
        {
            Node.bLeaf = true;
            return true;
        }
        const int32 First = Tree.Nodes.AddDefaulted(8);
        Tree.Nodes[Index].FirstChildIndex = static_cast<uint16>(First);
        for (int32 Child = 0; Child < 8; ++Child)
        {
            const FIntVector Relative(
                Original.Center.X + ChildSign[Child][0] * ChildHalf,
                Original.Center.Y + ChildSign[Child][1] * ChildHalf,
                Original.Center.Z + ChildSign[Child][2] * ChildHalf);
            if (FMath::Abs(Relative.X) > MAX_int16 || FMath::Abs(Relative.Y) > MAX_int16 ||
                FMath::Abs(Relative.Z) > MAX_int16)
            {
                UE_LOG(LogCrimeFactoryAirNav, Error, TEXT("Octree centre exceeds int16 coordinate range."));
                return false;
            }
            Tree.Nodes[First + Child].Center = Relative;
            Tree.Nodes[First + Child].HalfSize = ChildHalf;
        }
        bool bAllBlockedLeaves = true;
        for (int32 Child = 0; Child < 8; ++Child)
        {
            if (!GenerateNode(World, Tree, First + Child))
            {
                return false;
            }
            const FNode& Result = Tree.Nodes[First + Child];
            bAllBlockedLeaves &= Result.bLeaf && Result.bBlocked;
        }
        // The vanilla data uses 0xFFFF for a collapsed blocked leaf.
        if (bAllBlockedLeaves)
        {
            Tree.Nodes.RemoveAt(First, 8, EAllowShrinking::No);
            Tree.Nodes[Index].bLeaf = true;
            Tree.Nodes[Index].bBlocked = true;
            Tree.Nodes[Index].FirstChildIndex = MAX_uint16;
        }
        return true;
    }

    // Face, edge, and corner contacts are present in the vanilla neighbour arrays.
    bool Touches(const FVector& A, double AH, const FVector& B, double BH)
    {
        const double Limit = AH + BH + 0.01;
        return FMath::Abs(A.X - B.X) <= Limit &&
            FMath::Abs(A.Y - B.Y) <= Limit &&
            FMath::Abs(A.Z - B.Z) <= Limit;
    }

    void GatherTouching(const TArray<FTree>& Trees, uint8 OtherTreeIndex, uint32 SourceIndex,
        const FVector& SourceCenter, uint16 SourceHalf, int32 CandidateIndex, TArray<uint16>& Out)
    {
        const FTree& Tree = Trees[OtherTreeIndex];
        if (!Tree.Nodes.IsValidIndex(CandidateIndex))
        {
            return;
        }
        const FNode& Candidate = Tree.Nodes[CandidateIndex];
        if (!Touches(SourceCenter, SourceHalf, Tree.Center + FVector(Candidate.Center), Candidate.HalfSize))
        {
            return;
        }
        if (Candidate.bLeaf)
        {
            if (OtherTreeIndex != (SourceIndex >> 16) || CandidateIndex != (SourceIndex & 0xFFFF))
            {
                Out.Add(static_cast<uint16>(CandidateIndex));
            }
            return;
        }
        if (Candidate.FirstChildIndex == MAX_uint16 ||
            Candidate.FirstChildIndex + 7 >= Tree.Nodes.Num())
        {
            return;
        }
        for (int32 Child = 0; Child < 8; ++Child)
        {
            GatherTouching(Trees, OtherTreeIndex, SourceIndex, SourceCenter, SourceHalf,
                Candidate.FirstChildIndex + Child, Out);
        }
    }

    void BuildNeighbors(TArray<FTree>& Trees)
    {
        // The source index is packed, so a node is only excluded when both indices match.
        for (int32 TreeIndex = 0; TreeIndex < Trees.Num(); ++TreeIndex)
        {
            for (int32 NodeIndex = 0; NodeIndex < Trees[TreeIndex].Nodes.Num(); ++NodeIndex)
            {
                FNode& Node = Trees[TreeIndex].Nodes[NodeIndex];
                if (!Node.bLeaf || Node.bBlocked)
                {
                    continue;
                }
                const FVector Center = Trees[TreeIndex].Center + FVector(Node.Center);
                const uint32 Packed = (static_cast<uint32>(TreeIndex) << 16) | static_cast<uint32>(NodeIndex);
                for (int32 OtherTree = 0; OtherTree < Trees.Num(); ++OtherTree)
                {
                    TArray<uint16> Found;
                    GatherTouching(Trees, static_cast<uint8>(OtherTree), Packed, Center, Node.HalfSize, 0, Found);
                    if (!Found.IsEmpty())
                    {
                        Found.Sort();
                        Node.Neighbors.Add(static_cast<uint8>(OtherTree), MoveTemp(Found));
                    }
                }
            }
        }
    }

    uint32 FindLeafNode(const TArray<FTree>& Trees, const FVector& Location)
    {
        uint32 Best = InvalidPackedNode;
        double BestHalf = TNumericLimits<double>::Max();
        for (int32 TreeIndex = 0; TreeIndex < Trees.Num(); ++TreeIndex)
        {
            const FTree& Tree = Trees[TreeIndex];
            int32 Index = 0;
            while (Tree.Nodes.IsValidIndex(Index))
            {
                const FNode& Node = Tree.Nodes[Index];
                const FVector Center = Tree.Center + FVector(Node.Center);
                const FVector Delta = Location - Center;
                if (FMath::Abs(Delta.X) > Node.HalfSize || FMath::Abs(Delta.Y) > Node.HalfSize ||
                    FMath::Abs(Delta.Z) > Node.HalfSize)
                {
                    break;
                }
                if (Node.bLeaf)
                {
                    // Vanilla connectors can reference blocked leaf nodes.
                    if (Node.HalfSize < BestHalf)
                    {
                        Best = (static_cast<uint32>(TreeIndex) << 16) | static_cast<uint32>(Index);
                        BestHalf = Node.HalfSize;
                    }
                    break;
                }
                const uint16 First = Node.FirstChildIndex;
                if (First == MAX_uint16 || First + 7 >= Tree.Nodes.Num())
                {
                    break;
                }
                int32 Child = Delta.Z >= 0.0 ? 0 : 4;
                if (Delta.Y >= 0.0)
                {
                    Child += Delta.X >= 0.0 ? 2 : 3;
                }
                else if (Delta.X >= 0.0)
                {
                    Child += 1;
                }
                Index = First + Child;
            }
        }
        return Best;
    }

    bool BuildLinks(UWorld& World, TArray<FTree>& Trees, TArray<FLink>& OutLinks, FString& Error)
    {
        TArray<UActorComponent*> Components;
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            TArray<UActorComponent*> Owned;
            It->GetComponents(Owned);
            for (UActorComponent* Component : Owned)
            {
                if (Component && IsNativeType(Component->GetClass(),
                        TEXT("/Script/Starbreeze.SBZAirNavLinkComponent")))
                {
                    Components.Add(Component);
                }
            }
        }
        Components.Sort([](const UActorComponent& A, const UActorComponent& B)
        {
            return A.GetPathName() < B.GetPathName();
        });

        for (UActorComponent* Component : Components)
        {
            FVector Left, Right;
            if (!GetVector(Component, TEXT("LeftLink"), Left) ||
                !GetVector(Component, TEXT("RightLink"), Right))
            {
                Error = FString::Printf(TEXT("Cannot read link endpoints on %s"), *Component->GetPathName());
                return false;
            }
            AActor* Owner = Component->GetOwner();
            if (!Owner)
            {
                continue;
            }
            FLink Link;
            Link.Component = Component;
            const FTransform& Transform = Owner->GetActorTransform();
            Link.WorldPoints.Add(Transform.TransformPosition(Left));
            FArrayProperty* Extras = ArrayField(Component->GetClass(), TEXT("AdditionalPoints"));
            FStructProperty* VectorType = Extras ? CastField<FStructProperty>(Extras->Inner) : nullptr;
            if (!VectorType || VectorType->Struct != TBaseStructure<FVector>::Get())
            {
                Error = FString::Printf(TEXT("Cannot read AdditionalPoints on %s"), *Component->GetPathName());
                return false;
            }
            FScriptArrayHelper Additional(Extras, Extras->ContainerPtrToValuePtr<void>(Component));
            for (int32 PointIndex = 0; PointIndex < Additional.Num(); ++PointIndex)
            {
                const FVector& P = *reinterpret_cast<const FVector*>(Additional.GetRawPtr(PointIndex));
                Link.WorldPoints.Add(Transform.TransformPosition(P));
            }
            Link.WorldPoints.Add(Transform.TransformPosition(Right));
            Link.LeftIndex = FindLeafNode(Trees, Link.WorldPoints[0]);
            Link.RightIndex = FindLeafNode(Trees, Link.WorldPoints.Last());
            if (Link.LeftIndex == InvalidPackedNode || Link.RightIndex == InvalidPackedNode ||
                Link.LeftIndex == Link.RightIndex)
            {
                UE_LOG(LogCrimeFactoryAirNav, Warning,
                    TEXT("Unresolved air-nav link %s (left=%u right=%u). Excluding from generated link array."),
                    *Component->GetPathName(), Link.LeftIndex, Link.RightIndex);
                continue;
            }
            if (IsCooked(Component) || IsCooked(Owner))
            {
                Error = FString::Printf(TEXT("Link %s is from cooked reference content. Refusing to modify it."),
                    *Component->GetPathName());
                return false;
            }
            if (OutLinks.Num() >= MAX_uint16)
            {
                Error = TEXT("Too many navigation links for uint16 NavLinkID.");
                return false;
            }
            const uint16 Id = static_cast<uint16>(OutLinks.Num());
            Trees[Link.LeftIndex >> 16].Nodes[Link.LeftIndex & 0xFFFF].LinkIds.Add(Id);
            Trees[Link.RightIndex >> 16].Nodes[Link.RightIndex & 0xFFFF].LinkIds.Add(Id);
            OutLinks.Add(MoveTemp(Link));
        }
        return true;
    }

    struct FFields
    {
        FArrayProperty* Trees = nullptr;
        FStructProperty* TreeEntry = nullptr;
        FArrayProperty* Nodes = nullptr;
        FStructProperty* NodeEntry = nullptr;
        FArrayProperty* Neighbors = nullptr;
        FStructProperty* NeighborEntry = nullptr;
        FArrayProperty* Indices = nullptr;
        FArrayProperty* NodeLinks = nullptr;
        FArrayProperty* LinkObjects = nullptr;
        FSoftObjectProperty* LinkObjectType = nullptr;
        FStructProperty* LinkData = nullptr;
        FArrayProperty* LinkPoints = nullptr;
    };

    bool ResolveFields(AActor& Nav, const TArray<FLink>& Links, FFields& F, FString& Error)
    {
        F.Trees = ArrayField(Nav.GetClass(), TEXT("NavData"));
        F.TreeEntry = F.Trees ? CastField<FStructProperty>(F.Trees->Inner) : nullptr;
        F.Nodes = F.TreeEntry ? ArrayField(F.TreeEntry->Struct, TEXT("Nodes")) : nullptr;
        F.NodeEntry = F.Nodes ? CastField<FStructProperty>(F.Nodes->Inner) : nullptr;
        F.Neighbors = F.NodeEntry ? ArrayField(F.NodeEntry->Struct, TEXT("NeighborInfo")) : nullptr;
        F.NeighborEntry = F.Neighbors ? CastField<FStructProperty>(F.Neighbors->Inner) : nullptr;
        F.Indices = F.NeighborEntry ? ArrayField(F.NeighborEntry->Struct, TEXT("Indicies")) : nullptr;
        // Some dumps spell this property 'Indicies'; preserve the actual reflected spelling.
        if (!F.Indices && F.NeighborEntry)
        {
            F.Indices = ArrayField(F.NeighborEntry->Struct, TEXT("Indices"));
        }
        F.NodeLinks = F.NodeEntry ? ArrayField(F.NodeEntry->Struct, TEXT("NavLinkIDArray")) : nullptr;
        F.LinkObjects = ArrayField(Nav.GetClass(), TEXT("AirNavLinkArray"));
        F.LinkObjectType = F.LinkObjects ? CastField<FSoftObjectProperty>(F.LinkObjects->Inner) : nullptr;
        if (!F.Trees || !F.TreeEntry || !F.Nodes || !F.NodeEntry || !F.Neighbors ||
            !F.NeighborEntry || !F.Indices || !F.NodeLinks || !F.LinkObjects || !F.LinkObjectType)
        {
            Error = TEXT("Missing expected Starbreeze USTRUCTs/arrays in Suzie reflection. Check generated native types.");
            return false;
        }
        // Validate all fields before Modify(): a generated stub with a missing property
        // must not result in half-written native structures.
        auto IntegerPresent = [](const UStruct* Type, const TCHAR* Field)
        {
            const FNumericProperty* P = Type ? FindFProperty<FNumericProperty>(Type, Field) : nullptr;
            return P && P->IsInteger();
        };
        const FStructProperty* TreeCenter = StructField(F.TreeEntry->Struct, TEXT("TreeCenter"));
        const FNumericProperty* IndexElement = CastField<FNumericProperty>(F.Indices->Inner);
        const FNumericProperty* LinkElement = CastField<FNumericProperty>(F.NodeLinks->Inner);
        if (!IntegerPresent(F.TreeEntry->Struct, TEXT("MinHalfSize")) ||
            !IntegerPresent(F.NodeEntry->Struct, TEXT("HalfSize")) ||
            !IntegerPresent(F.NodeEntry->Struct, TEXT("FirstChildIndex")) ||
            !IntegerPresent(F.NeighborEntry->Struct, TEXT("TreeIndex")) ||
            !IndexElement || !IndexElement->IsInteger() ||
            !LinkElement || !LinkElement->IsInteger() ||
            !FindFProperty<FBoolProperty>(F.NodeEntry->Struct, TEXT("bIsLeaf")) ||
            !FindFProperty<FBoolProperty>(F.NodeEntry->Struct, TEXT("bIsBlocked")) ||
            !FindFProperty<FFloatProperty>(F.TreeEntry->Struct, TEXT("TreeHalfSize")) ||
            !TreeCenter || TreeCenter->Struct != TBaseStructure<FVector>::Get())
        {
            Error = TEXT("Invalid/incomplete native air-navigation field layout; aborted before modifying actors.");
            return false;
        }
        const FNumericProperty* Coordinate = FindFProperty<FNumericProperty>(F.NodeEntry->Struct, TEXT("Center"));
        if (!Coordinate || !Coordinate->IsInteger() || Coordinate->ArrayDim != 3)
        {
            Error = TEXT("Native octree Center[3] array is missing or has the wrong element type.");
            return false;
        }
        for (const FLink& Link : Links)
        {
            FStructProperty* Data = StructField(Link.Component->GetClass(), TEXT("NavLinkData"));
            FArrayProperty* Points = Data ? ArrayField(Data->Struct, TEXT("Points")) : nullptr;
            FStructProperty* PointType = Points ? CastField<FStructProperty>(Points->Inner) : nullptr;
            if (!Data || !Points || !PointType || PointType->Struct != TBaseStructure<FVector>::Get())
            {
                Error = FString::Printf(TEXT("Incomplete NavLinkData reflection on %s."), *Link.Component->GetPathName());
                return false;
            }
            for (const TCHAR* Field : {TEXT("LeftAirNodeIndex"), TEXT("RightAirNodeIndex"), TEXT("NavLinkID")})
            {
                FNumericProperty* Integer = FindFProperty<FNumericProperty>(Data->Struct, Field);
                if (!Integer || !Integer->IsInteger())
                {
                    Error = FString::Printf(TEXT("Missing NavLinkData.%s on %s."), Field, *Link.Component->GetPathName());
                    return false;
                }
            }
            if (!FindFProperty<FBoolProperty>(Data->Struct, TEXT("bIsOpen")))
            {
                Error = FString::Printf(TEXT("Missing NavLinkData.bIsOpen on %s."), *Link.Component->GetPathName());
                return false;
            }
        }
        return true;
    }

    void WriteIntegers(const FArrayProperty* Property, void* Container, const TArray<uint16>& Values)
    {
        FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Container));
        Helper.EmptyAndAddValues(Values.Num());
        FNumericProperty* Number = CastFieldChecked<FNumericProperty>(Property->Inner);
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            Number->SetIntPropertyValue(Helper.GetRawPtr(Index), static_cast<uint64>(Values[Index]));
        }
    }

    void WriteTrees(AActor& Nav, const FFields& Fields, const TArray<FTree>& Trees)
    {
        FScriptArrayHelper TreeArray(Fields.Trees, Fields.Trees->ContainerPtrToValuePtr<void>(&Nav));
        TreeArray.EmptyAndAddValues(Trees.Num());
        for (int32 TreeIndex = 0; TreeIndex < Trees.Num(); ++TreeIndex)
        {
            const FTree& Tree = Trees[TreeIndex];
            void* Entry = TreeArray.GetRawPtr(TreeIndex);
            const UScriptStruct* TreeType = Fields.TreeEntry->Struct;
            SetInteger(TreeType, Entry, TEXT("MinHalfSize"), Tree.MinimumHalfSize);
            SetVector(TreeType, Entry, TEXT("TreeCenter"), Tree.Center);
            if (FFloatProperty* HalfSize = FindFProperty<FFloatProperty>(TreeType, TEXT("TreeHalfSize")))
            {
                // Retail BEBE trees serialize this as 0.0 even with a nonzero root node size.
                HalfSize->SetPropertyValue_InContainer(Entry, 0.0f);
            }
            FScriptArrayHelper NodeArray(Fields.Nodes, Fields.Nodes->ContainerPtrToValuePtr<void>(Entry));
            NodeArray.EmptyAndAddValues(Tree.Nodes.Num());
            for (int32 NodeIndex = 0; NodeIndex < Tree.Nodes.Num(); ++NodeIndex)
            {
                const FNode& Node = Tree.Nodes[NodeIndex];
                void* NodeData = NodeArray.GetRawPtr(NodeIndex);
                const UScriptStruct* NodeType = Fields.NodeEntry->Struct;
                FNumericProperty* Coordinate = FindFProperty<FNumericProperty>(NodeType, TEXT("Center"));
                if (Coordinate && Coordinate->ArrayDim == 3)
                {
                    const int32 Values[3] = {Node.Center.X, Node.Center.Y, Node.Center.Z};
                    for (int32 Axis = 0; Axis < 3; ++Axis)
                    {
                        Coordinate->SetIntPropertyValue(Coordinate->ContainerPtrToValuePtr<void>(NodeData, Axis), static_cast<int64>(Values[Axis]));
                    }
                }
                SetInteger(NodeType, NodeData, TEXT("HalfSize"), Node.HalfSize);
                SetFlag(NodeType, NodeData, TEXT("bIsLeaf"), Node.bLeaf);
                SetFlag(NodeType, NodeData, TEXT("bIsBlocked"), Node.bBlocked);
                SetInteger(NodeType, NodeData, TEXT("FirstChildIndex"), Node.FirstChildIndex);
                WriteIntegers(Fields.NodeLinks, NodeData, Node.LinkIds);
                FScriptArrayHelper NeighborArray(Fields.Neighbors,
                    Fields.Neighbors->ContainerPtrToValuePtr<void>(NodeData));
                NeighborArray.EmptyAndAddValues(Node.Neighbors.Num());
                int32 NeighborIndex = 0;
                TArray<uint8> TreeKeys;
                Node.Neighbors.GetKeys(TreeKeys);
                TreeKeys.Sort();
                for (uint8 OtherTree : TreeKeys)
                {
                    void* Info = NeighborArray.GetRawPtr(NeighborIndex++);
                    SetInteger(Fields.NeighborEntry->Struct, Info, TEXT("TreeIndex"), OtherTree);
                    WriteIntegers(Fields.Indices, Info, Node.Neighbors.FindChecked(OtherTree));
                }
            }
        }
    }

    void WriteLinks(AActor& Nav, const FFields& Fields, const TArray<FLink>& Links)
    {
        FScriptArrayHelper Objects(Fields.LinkObjects, Fields.LinkObjects->ContainerPtrToValuePtr<void>(&Nav));
        Objects.EmptyAndAddValues(Links.Num());
        for (int32 Index = 0; Index < Links.Num(); ++Index)
        {
            const FLink& Link = Links[Index];
            UActorComponent* Component = Link.Component;
            Fields.LinkObjectType->SetObjectPropertyValue(Objects.GetRawPtr(Index), Component);
            FStructProperty* Data = StructField(Component->GetClass(), TEXT("NavLinkData"));
            void* StructData = Data->ContainerPtrToValuePtr<void>(Component);
            SetInteger(Data->Struct, StructData, TEXT("LeftAirNodeIndex"), Link.LeftIndex);
            SetInteger(Data->Struct, StructData, TEXT("RightAirNodeIndex"), Link.RightIndex);
            SetInteger(Data->Struct, StructData, TEXT("NavLinkID"), Index);
            SetFlag(Data->Struct, StructData, TEXT("bIsOpen"), false); // runtime applies bStartEnabled
            FArrayProperty* PointProperty = ArrayField(Data->Struct, TEXT("Points"));
            FScriptArrayHelper PointArray(PointProperty, PointProperty->ContainerPtrToValuePtr<void>(StructData));
            PointArray.EmptyAndAddValues(Link.WorldPoints.Num());
            FStructProperty* PointType = CastFieldChecked<FStructProperty>(PointProperty->Inner);
            for (int32 PointIndex = 0; PointIndex < Link.WorldPoints.Num(); ++PointIndex)
            {
                PointType->CopyCompleteValue(PointArray.GetRawPtr(PointIndex), &Link.WorldPoints[PointIndex]);
            }
            Component->MarkPackageDirty();
            if (AActor* Owner = Component->GetOwner())
            {
                Owner->MarkPackageDirty();
            }
        }
    }

    bool Generate(UWorld& World, AActor& Nav, FString& Error, int32& TotalNodes, int32& TotalLinks)
    {
        if (IsCooked(&Nav))
        {
            Error = TEXT("The navigation actor belongs to a cooked vanilla map. Open an authored map instead.");
            return false;
        }
        TArray<AActor*> Bounds;
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            if (IsNativeType(It->GetClass(), TEXT("/Script/Starbreeze.SBZAirNavigationBoundsVolume")))
            {
                Bounds.Add(*It);
            }
        }
        Bounds.Sort([](const AActor& A, const AActor& B)
        {
            return A.GetPathName() < B.GetPathName();
        });
        if (Bounds.IsEmpty())
        {
            Error = TEXT("No loaded ASBZAirNavigationBoundsVolume found. Place one or more in the authored map.");
            return false;
        }
        if (Bounds.Num() > 255)
        {
            Error = TEXT("More than 255 navigation trees: TreeIndex is stored in a uint8.");
            return false;
        }
        int32 Minimum = CVarAirNavMinHalfSize.GetValueOnGameThread();
        if (Minimum < 1 || Minimum > 16383)
        {
            Error = TEXT("CrimeFactory.AirNav.MinHalfSize must be between 1 and 16383.");
            return false;
        }
        TArray<FTree> Trees;
        Trees.Reserve(Bounds.Num());
        for (AActor* Volume : Bounds)
        {
            const FBox Box = Volume->GetComponentsBoundingBox(true);
            if (!Box.IsValid)
            {
                Error = FString::Printf(TEXT("Invalid navigation bounds for %s."), *Volume->GetPathName());
                return false;
            }
            FTree& Tree = Trees.AddDefaulted_GetRef();
            Tree.Center = Box.GetCenter();
            Tree.MinimumHalfSize = static_cast<uint16>(Minimum);
            const FVector Extent = Box.GetExtent();
            const int32 RootHalf = FMath::CeilToInt(Extent.GetMax() / Minimum) * Minimum;
            if (RootHalf < Minimum || RootHalf > 32767)
            {
                Error = FString::Printf(TEXT("Bounds %s exceed the valid octree size. Subdivide the volume."),
                    *Volume->GetPathName());
                return false;
            }
            Tree.Nodes.AddDefaulted();
            Tree.Nodes[0].HalfSize = static_cast<uint16>(RootHalf);
            if (!GenerateNode(World, Tree, 0))
            {
                Error = FString::Printf(TEXT("Octree generation failed for %s. Reduce bounds or increase minimum size."),
                    *Volume->GetPathName());
                return false;
            }
            TotalNodes += Tree.Nodes.Num();
            UE_LOG(LogCrimeFactoryAirNav, Display, TEXT("%s: %d generated octree nodes."),
                *Volume->GetPathName(), Tree.Nodes.Num());
        }
        BuildNeighbors(Trees);
        TArray<FLink> Links;
        if (!BuildLinks(World, Trees, Links, Error))
        {
            return false;
        }
        FFields Fields;
        if (!ResolveFields(Nav, Links, Fields, Error))
        {
            return false;
        }
        const FScopedTransaction Transaction(NSLOCTEXT("CrimeFactory", "GenerateAirNavigation", "Generate Air Navigation"));
        Nav.Modify();
        for (const FLink& Link : Links)
        {
            Link.Component->Modify();
            if (AActor* Owner = Link.Component->GetOwner())
            {
                Owner->Modify();
            }
        }
        WriteTrees(Nav, Fields, Trees);
        WriteLinks(Nav, Fields, Links);
        Nav.MarkPackageDirty();
        TotalLinks = Links.Num();
        return true;
    }
}

void CrimeFactoryAirNavigation::GenerateForEditorWorld()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || World->WorldType != EWorldType::Editor)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Open an authored level in the editor before generating air navigation.")));
        return;
    }
    TArray<AActor*> NavActors;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (IsNativeType(It->GetClass(), TEXT("/Script/Starbreeze.SBZAirNavigation")))
        {
            NavActors.Add(*It);
        }
    }
    if (NavActors.Num() != 1)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(FString::Printf(TEXT("Expected exactly one loaded ASBZAirNavigation actor, found %d."), NavActors.Num())));
        return;
    }
    if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(
        TEXT("Experimental Starbreeze air-navigation generator. This will replace the selected map's octree data "
             "and update authored link components in loaded sublevels. The retail collision filter and some "
             "connector selection rules are not yet verified. Back up your authored level first. Continue?"))) != EAppReturnType::Yes)
    {
        return;
    }
    int32 Nodes = 0;
    int32 Links = 0;
    FString Error;
    if (!Generate(*World, *NavActors[0], Error, Nodes, Links))
    {
        UE_LOG(LogCrimeFactoryAirNav, Error, TEXT("Generation aborted: %s"), *Error);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
        return;
    }
    const FString Result = FString::Printf(TEXT("Generated %d nodes and %d links. "
        "Review the Output Log, save the modified authored levels, forge, and validate in-game."), Nodes, Links);
    UE_LOG(LogCrimeFactoryAirNav, Display, TEXT("%s"), *Result);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Result));
}
