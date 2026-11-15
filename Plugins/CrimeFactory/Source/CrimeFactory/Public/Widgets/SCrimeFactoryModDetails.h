#pragma once
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Metadata/ModMetadataObject.h"

/** Main metadata controls shared by creation and properties. The owner keeps Metadata alive. */
class SCrimeFactoryModDetails : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCrimeFactoryModDetails) {} 
        SLATE_ARGUMENT(UModMetadataObject*, Metadata)
        SLATE_ATTRIBUTE(FText, Identifier)
        SLATE_EVENT(FOnTextChanged, OnIdentifierChanged)
        SLATE_ATTRIBUTE(const FSlateBrush*, DefaultIcon)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
private:
    TWeakObjectPtr<UModMetadataObject> Metadata;
    TAttribute<const FSlateBrush*> DefaultIcon;
    TSharedPtr<struct FSlateDynamicImageBrush> Preview;
    TSharedPtr<class SVerticalBox> AuthorRows;
    void RebuildAuthors();
    FReply ChooseIcon();
};
