#pragma once

#include "CoreMinimal.h"
#include "CrimeFactoryActorVisualisationSettings.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"

class FProperty;
class UMeshComponent;
class UMaterialInterface;
class USkeletalMesh;
class UStaticMesh;

/** Mesh resolution/ownership only. Actor discovery, scheduling and editor boundaries belong to the shared manager. */
class FCrimeFactoryPawnPreview
{
public:
    void ReloadDefinitions();
    void UpdateActor(AActor& Actor);
    void RemoveActor(AActor& Actor);
    void RemoveAll();
    void GetActors(TArray<TWeakObjectPtr<AActor>>& OutActors) const;
    void Prune();
    bool InvalidateForObject(const UObject* Object);
    static bool SupportsRequestProperty(const FProperty* Property);

private:
    struct FPart
    {
        USkeletalMesh* SkeletalMesh = nullptr;
        UStaticMesh* StaticMesh = nullptr;
        TArray<UMaterialInterface*> Materials;
        TArray<float> PrimitiveData;
        TMap<FName, float> MorphTargets;
        FTransform Transform = FTransform::Identity;
        int32 Parent = INDEX_NONE;
        FName Socket;
        bool bVisible = true;
    };

    struct FAppearance
    {
        TArray<FPart> Parts;
        TArray<TStrongObjectPtr<UObject>> Assets;
        TSet<FObjectKey> Dependencies;
        FVector PawnScale = FVector::OneVector;
        int32 GroundingPart = INDEX_NONE;
        FString Error;
        void Keep(UObject* Object);
    };

    struct FActorPreview
    {
        TSharedPtr<FAppearance> Appearance;
        TArray<TWeakObjectPtr<UMeshComponent>> Components;
    };

    TSharedRef<FAppearance> Resolve(UObject* Request);
    void ReadPawn(UClass* Class, UObject* Variation, const void* VariationInterval,
        class UScriptStruct* IntervalType, FAppearance& Out);
    static UObject* ReadRequest(AActor& Actor, FName PropertyName, FString& Description);
    static TOptional<FTransform> GetSpawnTransform(const AActor& Actor);
    void Warn(AActor& Actor, const FString& Message);

    TMap<TWeakObjectPtr<UClass>, FName> Definitions;
    TArray<TStrongObjectPtr<UClass>> DefinitionClasses;
    TMap<FObjectKey, TSharedRef<FAppearance>> Cache;
    TMap<TWeakObjectPtr<AActor>, FActorPreview> Previews;
    TMap<TWeakObjectPtr<AActor>, FString> Warnings;
    bool bEnabled = true;
    float Scale = 1.0f;
};
