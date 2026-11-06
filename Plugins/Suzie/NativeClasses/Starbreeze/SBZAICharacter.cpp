/**
 * @file SBZAICharacter.cpp
 * @brief Restores AI character variation previews through Suzie editor callbacks.
 *
 * SBZAICharacter::ApplyVariation is not reflected in the current JMAP. These callbacks and
 * the reflected category rep-notify body provide editor application without replacing the class.
 */

#include "SBZVariationNative.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

namespace SuzieAICharacterVariation
{
    /**
     * Resolve the character's variation category and update its primary skeletal mesh.
     *
     * Retail uses a signed seed remainder and clamps it to the category bounds.
     * Negative bounds are open ends; this is not a random draw within the interval.
     * The saved seed is the effective preview seed, without consuming mission spawn allocations.
     * Rebuild subscriptions to the variation data, mesh, materials and archetype defaults.
     *
     * @param Context Access to the character's reflected variation properties and refresh API.
     */
    static void Apply(FSuzieFunctionContext& Context)
    {
        // Leave native defaults, Blueprint templates and commandlet output unchanged.
        if (Context.IsTemplate() || IsRunningCommandlet()) { return; }

        ACharacter* Character = Cast<ACharacter>(Context.GetSelf());
        if (!Character) { return; }

        // Subscribe before testing readiness so pending asset loads can refresh the instance.
        Context.ClearDependencies();
        UObject* Set = Context.GetProperty<UObject*>("VariationData");
        if (IsValid(Set)) { Context.WatchDependency(Set); }

        USkeletalMeshComponent* Mesh = Character->GetMesh();
        if (IsValid(Mesh)) { Context.WatchDependency(Mesh, "SkeletalMesh"); }

        // Blueprint default edits can change the inherited selection on instances.
        Context.WatchDependency(Character->GetArchetype(), "VariationData");
        Context.WatchDependency(Character->GetArchetype(), "VariationCategoryIndexIntervals");
        Context.WatchDependency(Character->GetArchetype(), "Seed");

        if (!SuzieVariation::Ready(Set) || !SuzieVariation::Ready(Mesh)) { return; }

        const int64 Category = Context.Property("VariationCategory").GetEnum();

        // Category intervals are a reflected fixed-size array, not a dynamic value-view array.
        const auto* Intervals = FindFProperty<FStructProperty>(Character->GetClass(), TEXT("VariationCategoryIndexIntervals"));
        if (!Context.IsValid() || !Intervals || Category < 0 || Category >= Intervals->ArrayDim) { return; }

        const auto* Lower = FindFProperty<FIntProperty>(Intervals->Struct, TEXT("Min"));
        const auto* Upper = FindFProperty<FIntProperty>(Intervals->Struct, TEXT("Max"));
        if (!Lower || !Upper) { Context.Fail(TEXT("Unexpected Int32Interval reflection schema")); return; }

        const void* Interval = Intervals->ContainerPtrToValuePtr<void>(Character, static_cast<int32>(Category));
        const int32 First = Lower->GetPropertyValue_InContainer(Interval);
        const int32 Last = Upper->GetPropertyValue_InContainer(Interval);

        const int32 Count = Context.Property("Variations", Set).Num();
        const int32 Seed = Context.GetProperty<int32>("Seed");
        if (!Context.IsValid() || Count <= 0) { return; }

        // Signed IDIV in retail is deliberate: negative seeds clamp to the lower
        // bound. Do not use abs(), unsigned modulo or a vehicle-style random stream.
        const int32 LowerIndex = First < 0 ? 0 : FMath::Clamp(First, 0, Count - 1);
        const int32 UpperIndex = Last < 0 ? Count - 1 : FMath::Clamp(Last, 0, Count - 1);
        const int32 Index = FMath::Max(LowerIndex, FMath::Min(Seed % Count, UpperIndex));

        UObject* SelectedMesh = Context.Property("SkeletalMesh", Set).Get<UObject*>();
        if (!SelectedMesh) { SelectedMesh = Context.Property("Mesh", Set).Get<UObject*>(); }
        if (IsValid(SelectedMesh)) { Context.WatchDependency(SelectedMesh); }

        const auto Materials = Context.Property("OverrideMaterials", Set).Get<TArray<UObject*>>();
        for (UObject* Material : Materials)
        {
            if (IsValid(Material)) { Context.WatchDependency(Material); }
        }

        if (Context.IsValid()) { SuzieVariation::CallUtility(Context, Mesh, Set, Index); }
    }

}

/** Ignore player/crew classes that do not declare the AI character variation path. */
void SuzieVariation::ApplyCharacterSeed(FSuzieFunctionContext& Context)
{
    const UClass* AIClass = FindObject<UClass>(nullptr, TEXT("/Script/Starbreeze.SBZAICharacter"));
    if (AIClass && Context.GetSelf() && Context.GetSelf()->IsA(AIClass))
    {
        SuzieAICharacterVariation::Apply(Context);
    }
}

/** Apply the preview after the character's defaults and required loading are ready. */
SUZIE_POST_INITIALIZE(SBZAICharacter)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Apply a changed variation asset and update the instance's dependency subscriptions. */
SUZIE_PROPERTY_CHANGED(SBZAICharacter, VariationData)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Refresh the selected entry when a category interval changes in the editor. */
SUZIE_PROPERTY_CHANGED(SBZAICharacter, VariationCategoryIndexIntervals)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Apply the preview entry associated with the newly selected category. */
SUZIE_PROPERTY_CHANGED(SBZAICharacter, VariationCategory)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Apply the current variation when the character's primary mesh component changes. */
SUZIE_PROPERTY_CHANGED(SBZAICharacter, Mesh)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Reapply the preview after a watched dependency or archetype property changes. */
SUZIE_REFRESH(SBZAICharacter)
{
    SuzieAICharacterVariation::Apply(Context);
}

/** Refresh the character when the reflected variation category rep-notify is invoked. */
SUZIE_FUNCTION(SBZAICharacter, OnRep_VariationCategory)
{
    SuzieAICharacterVariation::Apply(Context);
}
