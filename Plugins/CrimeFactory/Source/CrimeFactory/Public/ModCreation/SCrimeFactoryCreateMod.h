#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STileView.h"
#include "ModCreation/ModWizardDefinition.h"
#include "UObject/StrongObjectPtr.h"
#include "Metadata/ModMetadataObject.h"

class SDockTab;
class SCrimeFactoryCreateMod : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCrimeFactoryCreateMod) {} SLATE_END_ARGS()
    void Construct(const FArguments&, TSharedRef<SDockTab> InTab);
    void SetOnCreated(TFunction<void(IPlugin&)> Callback) { OnCreated = MoveTemp(Callback); }
private:
    using FItem = TSharedPtr<FPluginTemplateDescription>;
    TSharedPtr<FModWizardDefinition> Wizard;
    TWeakPtr<SDockTab> Tab;
    TFunction<void(IPlugin&)> OnCreated;
    TArray<FItem> Filtered;
    TSharedPtr<STileView<FItem>> Grid;
    FString Search, ModName;
    TStrongObjectPtr<UModMetadataObject> Metadata;
    bool bCreating = false;
    void Filter();
    FReply Refresh();
    FReply Create();
    FText Validation() const;
    const FSlateBrush* Icon(FItem Item) const;
    TSharedRef<ITableRow> Tile(FItem Item, const TSharedRef<STableViewBase>& Owner);
};
