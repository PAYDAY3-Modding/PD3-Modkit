#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "ModBrowser/CrimeFactoryModEntryList.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateColor.h"
#include "Widgets/Input/SCheckBox.h"

struct FCrimeFactoryPreflightResult;
struct FCrimeFactoryForgeBatch;

/**
 * Entry of a mod within the mod list of the crimefactory window
 */
class SCrimeFactoryModEntry : public SCompoundWidget {
    SLATE_BEGIN_ARGS(SCrimeFactoryModEntry) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& Args, TSharedRef<IPlugin> InMod, TSharedRef<SCrimeFactoryModEntryList> InOwner);

    void PackageMod(const TArray<TSharedPtr<SCrimeFactoryModEntry>>& NextEntries) const;
    bool RunPreflightValidation() const;
    FCrimeFactoryPreflightResult CollectPreflightResult() const;

    FReply OnOpenInformation();
    FReply OnOpenModContent();
    FReply OnViewPALAssets();
    FReply OnRefreshPALInspector();
    FReply OnFixInformationIssues();

    void RefreshPALInspectorState();
    FText GetPALInspectorSummaryText() const;
    FSlateColor GetInformationButtonColor() const;
    FText GetInformationButtonTooltip() const;
    EVisibility GetFixInformationVisibility() const;
    bool CanAutoFixInformationIssues() const;
    void OnEnableCheckboxChanged(ECheckBoxState NewState);

    void OnEditMod();
    void RefreshModIcon();
    FReply OnEditModFinished(class UModMetadataObject* Metadata);
    FReply OnDeleteMod();

    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event) override;

    FORCEINLINE bool IsSelected() {
        return Checkbox && Checkbox->IsChecked();
    }

    FORCEINLINE void SetSelected(bool bInSelected) {
        if (Checkbox) {
            Checkbox->SetIsChecked(bInSelected);
            OnEnableCheckboxChanged(bInSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked);
        }
    }

private:
    void PackageMod(const TArray<TSharedPtr<SCrimeFactoryModEntry>>& NextEntries,
        TSharedRef<struct FCrimeFactoryForgeBatch> Batch) const;
    TSharedPtr<IPlugin> Mod;
    TWeakPtr<SCrimeFactoryModEntryList> Owner;
    TSharedPtr<class SCheckBox> Checkbox;

    /** PAL/chunk information shown by the Information window. */
    FText PALInspectorSummary;
    TSharedPtr<const struct FCrimeFactoryModInspection> PALInformation;
    TArray<TWeakPtr<class SCrimeFactoryModInspector>> InspectorViews;

    /** Cached inspector severity used to tint the Information button. */
    bool bInformationHasWarnings = false;
    bool bInformationHasAutoFixableWarnings = false;

    /** Brush for the mod's Icon128.png, or CrimeFactory128.png as fallback. */
    TSharedPtr<struct FSlateDynamicImageBrush> ModIconBrush;
    FString ModIconFilename;
    FDateTime ModIconTimestamp;
    int64 ModIconFileSize = -1;
    TSharedPtr<class SImage> ModIconImage;
    EActiveTimerReturnType PollModIcon(double Time, float Delta);
    
    /** Dialog for editing mod properties */
    TSharedPtr<SWindow> PropertiesWindow;
};
