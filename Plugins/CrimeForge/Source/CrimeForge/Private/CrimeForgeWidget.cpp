#include "CrimeForgeWidget.h"
#include "CrimeForge.h"
#include "CrimeForgeModEntryList.h"
#include "CrimeForgeSettings.h"
#include "DesktopPlatformModule.h"

#define LOCTEXT_NAMESPACE "CrimeForgeWidget"

void SCrimeForgeWidget::Construct(const FArguments& InArgs) {
    FDetailsViewArgs DetailsViewArgs;
    DetailsViewArgs.bAllowSearch = true;
    DetailsViewArgs.bHideSelectionTip = true;
    DetailsViewArgs.bLockable = false;
    DetailsViewArgs.bSearchInitialKeyFocus = true;
    DetailsViewArgs.bUpdatesFromSelection = false;
    DetailsViewArgs.bShowOptions = true;
    DetailsViewArgs.bShowModifiedPropertiesOption = true;
    DetailsViewArgs.bShowObjectLabel = false;
    DetailsViewArgs.bCustomNameAreaLocation = true;
    DetailsViewArgs.bCustomFilterAreaLocation = true;
    DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
    DetailsViewArgs.bAllowMultipleTopLevelObjects = true;
    DetailsViewArgs.bShowScrollBar = false;

    FPropertyEditorModule& PropertyEditorModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
    DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
    DetailsView->SetObject(UCrimeForgeSettings::Get());
    DetailsView->OnFinishedChangingProperties().AddLambda([this](const FPropertyChangedEvent&) {
        UCrimeForgeSettings::Get()->SaveSettings();
    });
    
    ChildSlot[
        SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[
            DetailsView.ToSharedRef()
        ]
        +SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)[
            SNew(SBorder)
            .Padding(FMargin(6.0f, 3.0f))
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ModsSectionHeader", "Mods"))
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 9))
            ]
        ]
        +SVerticalBox::Slot()
        .AutoHeight()
        .Padding(FMargin(5.0f, 4.0f, 5.0f, 3.0f))
        [
            SNew(SEditableTextBox)
            .HintText(LOCTEXT("SearchHint", "Search Mods..."))
            .OnTextChanged_Lambda([this](const FText& InText) {
                this->ModList->Filter(InText.ToString());
            })
        ]
        +SVerticalBox::Slot()
        .FillHeight(1.0f)
        .Padding(FMargin(5.0f, 2.0f, 5.0f, 5.0f))
        [
            SAssignNew(ModList, SCrimeForgeModEntryList)
        ]
    ];
}

#undef LOCTEXT_NAMESPACE
