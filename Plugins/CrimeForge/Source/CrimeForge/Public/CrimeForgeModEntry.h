#pragma once
#include "CrimeForgeModEntryList.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateColor.h"

/**
 * Entry of a mod within the mod list of the crimeforge window
 */
class SCrimeForgeModEntry : public SCompoundWidget {
    SLATE_BEGIN_ARGS(SCrimeForgeModEntry) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& Args, TSharedRef<IPlugin> InMod, TSharedRef<SCrimeForgeModEntryList> InOwner);

    void PackageMod(const TArray<TSharedPtr<SCrimeForgeModEntry>>& NextEntries) const;
    bool RunPreflightValidation() const;

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
    FReply OnEditModFinished(class UModMetadataObject* Metadata);
    FReply OnDeleteMod();

    FORCEINLINE bool IsSelected() {
        return Checkbox && Checkbox->IsChecked();
    }

    FORCEINLINE void SetSelected(bool bInSelected) {
        if (Checkbox) {
            Checkbox->SetIsChecked(bInSelected);
        }
    }

private:
    TSharedPtr<IPlugin> Mod;
    TSharedPtr<SCrimeForgeModEntryList> Owner;
    TSharedPtr<class SCheckBox> Checkbox;

    /** PAL/chunk information shown by the Information window. */
    FText PALInspectorSummary;

    /** Cached inspector severity used to tint the Information button. */
    bool bInformationHasWarnings = false;
    bool bInformationHasAutoFixableWarnings = false;

    /** Brush for the mod's Icon128.png, or CrimeForge128.png as fallback. */
    TSharedPtr<struct FSlateDynamicImageBrush> ModIconBrush;
    
    /** Dialog for editing mod properties */
    TSharedPtr<SWindow> PropertiesWindow;
};
