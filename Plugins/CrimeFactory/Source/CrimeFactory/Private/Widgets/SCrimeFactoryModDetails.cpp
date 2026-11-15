#include "Widgets/SCrimeFactoryModDetails.h"
#include "Metadata/CrimeFactoryModIcon.h"
#include "Editor/CrimeFactoryStyle.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryModDetails"

void SCrimeFactoryModDetails::Construct(const FArguments& Args)
{
    Metadata = Args._Metadata;
    DefaultIcon = Args._DefaultIcon;
    if (Metadata.IsValid() && !Metadata->TargetIconPath.IsEmpty()) Preview = CrimeFactoryModIcon::Load(Metadata->TargetIconPath);
    const auto Form = SNew(SVerticalBox);
    const auto Field = [&Form](FText Label, TSharedRef<SWidget> Input) {
        Form->AddSlot().AutoHeight().Padding(0, 8, 0, 4)[SNew(STextBlock).Text(Label)];
        Form->AddSlot().AutoHeight()[Input];
    };
    Field(LOCTEXT("Icon", "Icon"), SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth()
          [SNew(SButton).ButtonStyle(&FCrimeFactoryStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("CrimeFactory.ModCardIconButton")))
           .ContentPadding(FMargin(0)).OnClicked(this, &SCrimeFactoryModDetails::ChooseIcon)
           .ToolTipText(LOCTEXT("IconHelp", "Choose a PNG icon. Saved as Resources/Icon128.png."))
           [SNew(SBox).WidthOverride(96).HeightOverride(96)
            [SNew(SImage).Image_Lambda([this] { return Preview ? Preview.Get() : DefaultIcon.Get(FAppStyle::GetBrush("Icons.Plugin")); })]]]
        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(12, 0)
          [SNew(SVerticalBox)
           + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(LOCTEXT("ChooseIcon", "Choose Icon...")).OnClicked(this, &SCrimeFactoryModDetails::ChooseIcon)]
           + SVerticalBox::Slot().AutoHeight().Padding(0, 4)[SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
             .Text(LOCTEXT("IconLocation", "PNG image, saved to Resources/Icon128.png. 128 x 128 recommended."))]]);
    Form->AddSlot().AutoHeight().Padding(0, 8, 0, 4)
    [SNew(SHorizontalBox)
     + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
       [SNew(STextBlock).Text(LOCTEXT("Identifier", "Mod Name"))]
     + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0)
       [SNew(STextBlock).Text(LOCTEXT("PermanentName", "Cannot be renamed after creation"))
        .Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FStyleColors::Warning).AutoWrapText(true)
        .Visibility(Args._OnIdentifierChanged.IsBound() ? EVisibility::Visible : EVisibility::Collapsed)
        .ToolTipText(LOCTEXT("PermanentNameHelp", "Choose a permanent plugin identifier. You can change its Friendly Name later."))]];
    Form->AddSlot().AutoHeight()
    [SNew(SEditableTextBox).Text(Args._Identifier)
        .IsReadOnly(!Args._OnIdentifierChanged.IsBound()).OnTextChanged(Args._OnIdentifierChanged)
        .ToolTipText(LOCTEXT("IdentifierHelp", "Unique, permanent plugin identifier. Spaces and characters unsupported by Unreal plugin names are not allowed."))];
    const auto TextField = [&Field, &Args, this](FText Label, FString UModMetadataObject::* Member) {
        Field(Label, SNew(SEditableTextBox)
            .HintText(Member == &UModMetadataObject::FriendlyName ? Args._Identifier : TAttribute<FText>())
            .ToolTipText(Member == &UModMetadataObject::FriendlyName ? LOCTEXT("FriendlyHelp", "Display name shown to users. Spaces and punctuation are allowed. Leave empty to use the Mod Name.") : FText::GetEmpty())
            .Text_Lambda([this, Member] { return Metadata.IsValid() ? FText::FromString(Metadata.Get()->*Member) : FText::GetEmpty(); })
            .OnTextChanged_Lambda([this, Member](FText Text) { if (Metadata.IsValid()) Metadata.Get()->*Member = Text.ToString(); }));
    };
    TextField(LOCTEXT("FriendlyName", "Friendly Name"), &UModMetadataObject::FriendlyName);
    Field(LOCTEXT("Description", "Description"), SNew(SBox).MinDesiredHeight(96)
        [SNew(SMultiLineEditableTextBox).AutoWrapText(true)
         .Text_Lambda([this] { return Metadata.IsValid() ? FText::FromString(Metadata->Description) : FText::GetEmpty(); })
         .OnTextChanged_Lambda([this](FText Text) { if (Metadata.IsValid()) Metadata->Description = Text.ToString(); })]);
    TextField(LOCTEXT("CreatedBy", "Created By"), &UModMetadataObject::CreatedBy);
    Field(LOCTEXT("Authors", "Mod Authors"), SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[SAssignNew(AuthorRows, SVerticalBox)]
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 4)
          [SNew(SButton).Text(LOCTEXT("AddAuthor", "Add Author")).OnClicked_Lambda([this] {
              if (Metadata.IsValid()) { Metadata->ModAuthors.Add(FString()); RebuildAuthors(); }
              return FReply::Handled();
          })]);
    TextField(LOCTEXT("ModPage", "Mod Page URL"), &UModMetadataObject::ModPageURL);
    TextField(LOCTEXT("CreatorURL", "Created By URL"), &UModMetadataObject::CreatedByURL);
    ChildSlot[Form];
    RebuildAuthors();
}

void SCrimeFactoryModDetails::RebuildAuthors()
{
    AuthorRows->ClearChildren();
    if (!Metadata.IsValid()) return;
    for (int32 Index = 0; Index < Metadata->ModAuthors.Num(); ++Index)
    {
        AuthorRows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
        [SNew(SHorizontalBox)
         + SHorizontalBox::Slot().FillWidth(1)
           [SNew(SEditableTextBox).Text(FText::FromString(Metadata->ModAuthors[Index]))
            .OnTextChanged_Lambda([this, Index](FText Text) { if (Metadata.IsValid() && Metadata->ModAuthors.IsValidIndex(Index)) Metadata->ModAuthors[Index] = Text.ToString(); })]
         + SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
           [SNew(SButton).Text(LOCTEXT("Remove", "Remove")).OnClicked_Lambda([this, Index] {
               if (Metadata.IsValid() && Metadata->ModAuthors.IsValidIndex(Index)) { Metadata->ModAuthors.RemoveAt(Index); RebuildAuthors(); }
               return FReply::Handled();
           })]];
    }
}

FReply SCrimeFactoryModDetails::ChooseIcon()
{
    if (!Metadata.IsValid()) return FReply::Handled();
    TArray<FString> Files;
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (Desktop && Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
        LOCTEXT("ChooseIconTitle", "Choose Mod Icon").ToString(), FPaths::GetPath(Metadata->ChosenIconPath), FString(), TEXT("PNG image (*.png)|*.png"), 0, Files) && !Files.IsEmpty())
    {
        auto Brush = CrimeFactoryModIcon::Load(Files[0]);
        if (!CrimeFactoryModIcon::IsValidPNG(Files[0])) FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("InvalidIcon", "The selected PNG image cannot be loaded."));
        else { Preview = MoveTemp(Brush); Metadata->ChosenIconPath = FPaths::ConvertRelativePathToFull(Files[0]); }
    }
    return FReply::Handled();
}
#undef LOCTEXT_NAMESPACE
