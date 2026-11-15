#include "ModCreation/SCrimeFactoryCreateMod.h"
#include "Editor/CrimeFactoryStyle.h"
#include "Widgets/SCrimeFactoryModDetails.h"
#include "Dom/JsonValue.h"
#include "Metadata/CrimeFactoryModIcon.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "PluginUtils.h"
#include "Features/IPluginsEditorFeature.h"
#include "Interfaces/IPluginManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryCreateMod"

void SCrimeFactoryCreateMod::Construct(const FArguments&, TSharedRef<SDockTab> InTab)
{
    Tab = InTab;
    Wizard = MakeShared<FModWizardDefinition>();
    auto Heading = [](FText Text) -> TSharedRef<SWidget> {
        return SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("HeadingSmall"));
    };
    Metadata.Reset(NewObject<UModMetadataObject>());
    auto Form = SNew(SVerticalBox);
    Form->AddSlot().AutoHeight()
    [SNew(SCrimeFactoryModDetails).Metadata(Metadata.Get())
     .Identifier_Lambda([this] { return FText::FromString(ModName); })
     .OnIdentifierChanged_Lambda([this](FText Text) { ModName = Text.ToString(); })
     .DefaultIcon_Lambda([this] { return Icon(Wizard->GetSelectedTemplate()); })];
    Form->AddSlot().AutoHeight().Padding(0, 8)[SNew(STextBlock).AutoWrapText(true)
        .ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(this, &SCrimeFactoryCreateMod::Validation)];

    ChildSlot
    [SNew(SBorder).IsEnabled_Lambda([this] { return !bCreating; }).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FStyleColors::Panel).Padding(16)
     [SNew(SVerticalBox)
      + SVerticalBox::Slot().AutoHeight()[Heading(LOCTEXT("Title", "Create a New Mod"))]
      + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 16)
        [SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("Instructions", "Choose a template to start your mod with, then enter its details."))]
      + SVerticalBox::Slot().FillHeight(1)
        [SNew(SSplitter)
         + SSplitter::Slot().Value(0.63f).MinSize(260)
           [SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[Heading(LOCTEXT("Templates", "Templates"))]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 8, 12)
              [SNew(SHorizontalBox)
               + SHorizontalBox::Slot().FillWidth(1)[SNew(SSearchBox).HintText(LOCTEXT("Search", "Search Templates..."))
                 .OnTextChanged_Lambda([this](FText Text) { Search = Text.ToString(); Filter(); })]
               + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
                 [SNew(SButton).OnClicked(this, &SCrimeFactoryCreateMod::Refresh).ToolTipText(LOCTEXT("RefreshTip", "Rescan installed template folders"))
                  [SNew(SHorizontalBox)
                   + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SImage).Image(FAppStyle::GetBrush("Icons.Refresh"))]
                   + SHorizontalBox::Slot().AutoWidth().Padding(4, 0)[SNew(STextBlock).Text(LOCTEXT("Refresh", "Refresh"))]]]]
            + SVerticalBox::Slot().FillHeight(1)
              [SNew(SOverlay)
               + SOverlay::Slot()[SAssignNew(Grid, STileView<FItem>).ListItemsSource(&Filtered).ItemWidth(198).ItemHeight(260)
                 .SelectionMode(ESelectionMode::Single).OnGenerateTile(this, &SCrimeFactoryCreateMod::Tile)
                 .OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type Info) {
                    if (Item) Wizard->OnTemplateSelectionChanged(Item, Info);
                 })]
               + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16)
                 [SNew(STextBlock).AutoWrapText(true).Visibility_Lambda([this] { return Filtered.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
                  .Text_Lambda([this] {
                    if (!Wizard->DiscoveryError.IsEmpty()) return Wizard->DiscoveryError;
                    return Wizard->GetTemplatesSource().IsEmpty() ? LOCTEXT("NoTemplates", "No valid templates installed. Open the Templates folder to install one, then Refresh.") : LOCTEXT("NoMatches", "No templates match your search.");
                  })]]]
         + SSplitter::Slot().Value(0.37f).MinSize(260)
           [SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FStyleColors::Recessed).Padding(16)
            [SNew(SScrollBox) + SScrollBox::Slot()
             [SNew(SVerticalBox)
              + SVerticalBox::Slot().AutoHeight()[Heading(LOCTEXT("Details", "Mod Details"))]
              + SVerticalBox::Slot().AutoHeight()[Form]]]]]
      + SVerticalBox::Slot().AutoHeight().Padding(0, 12)[SNew(SSeparator)]
      + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
         + SHorizontalBox::Slot().FillWidth(1).HAlign(HAlign_Left)
           [SNew(SButton).Text(LOCTEXT("OpenFolder", "Open Templates Folder")).OnClicked_Lambda([this] {
             const FString Folder = FPaths::ConvertRelativePathToFull(Wizard->GetTemplatesDirectory());
             if (IFileManager::Get().DirectoryExists(*Folder)) FPlatformProcess::ExploreFolder(*Folder);
             else FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MissingFolder", "The Templates folder is missing. Repair your CrimeFactory installation."));
             return FReply::Handled(); })]
         + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
           [SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(LOCTEXT("Create", "Create Mod"))
            .IsEnabled_Lambda([this] { return Validation().IsEmpty(); }).ToolTipText(this, &SCrimeFactoryCreateMod::Validation)
            .OnClicked(this, &SCrimeFactoryCreateMod::Create)]
         + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(LOCTEXT("Cancel", "Cancel"))
           .OnClicked_Lambda([this] { if (auto Owner = Tab.Pin()) Owner->RequestCloseTab(); return FReply::Handled(); })]]]];
    Refresh();
}

const FSlateBrush* SCrimeFactoryCreateMod::Icon(FItem Item) const
{
    if (Item && Item->PluginIconDynamicImageBrush) return Item->PluginIconDynamicImageBrush.Get();
    return FCrimeFactoryStyle::Get().GetBrush("CrimeFactory.OpenPluginWindow");
}

TSharedRef<ITableRow> SCrimeFactoryCreateMod::Tile(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
    // Match the mod browser's bordered card, centred header and framed icon.
    // Keep a themed selection outline visible while focus moves to Mod Details.
    // The table row supplies hover and keyboard navigation.
    return SNew(STableRow<FItem>, Owner).Padding(4).ToolTipText(Item->Description)
        [SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).Padding(2)
         .BorderBackgroundColor_Lambda([this, Item] {
             return Wizard->GetSelectedTemplate() == Item ? FStyleColors::Select : FSlateColor(FLinearColor::Transparent);
         })
         [SNew(SBorder).Padding(6)
          [SNew(SVerticalBox)
          + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
            [SNew(SBox).MaxDesiredHeight(36).Clipping(EWidgetClipping::ClipToBounds)
             [SNew(STextBlock).Text(Item->Name).Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 12))
              .Justification(ETextJustify::Center).AutoWrapText(true).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]]
          + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 6)
            [SNew(SBorder).BorderImage(&FCrimeFactoryStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("CrimeFactory.ModCardIconButton")).Normal).Padding(4)
             [SNew(SBox).WidthOverride(128).HeightOverride(128)[SNew(SImage).Image(Icon(Item))]]]
          + SVerticalBox::Slot().FillHeight(1).Padding(0, 4)
            [SNew(SBox).Clipping(EWidgetClipping::ClipToBounds)
             [SNew(STextBlock).Text(Item->Description).AutoWrapText(true).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
              .ColorAndOpacity(FSlateColor::UseSubduedForeground())]]]]];
}


void SCrimeFactoryCreateMod::Filter()
{
    Grid->ClearSelection();
    Filtered.Reset();
    for (auto Item : Wizard->GetTemplatesSource())
        if (Search.IsEmpty() || Item->Name.ToString().Contains(Search) || Item->Description.ToString().Contains(Search) || FPaths::GetCleanFilename(Item->OnDiskPath).Contains(Search)) Filtered.Add(Item);
    Grid->RequestListRefresh();
    if (auto Selected = Wizard->GetSelectedTemplate(); Selected && Filtered.Contains(Selected)) Grid->SetSelection(Selected);
}

FReply SCrimeFactoryCreateMod::Refresh()
{
    Wizard->RefreshTemplates();
    for (auto Item : Wizard->GetTemplatesSource())
    {
        FString Path;
        Wizard->GetTemplateIconPath(Item, Path);
        Item->PluginIconDynamicImageBrush = CrimeFactoryModIcon::Load(Path);
    }
    Filter();
    return FReply::Handled();
}

FText SCrimeFactoryCreateMod::Validation() const
{
    if (bCreating) return LOCTEXT("Creating", "Creating mod...");
    if (!Wizard->HasValidTemplateSelection()) return LOCTEXT("NeedTemplate", "Select a valid template.");
    if (ModName.IsEmpty()) return LOCTEXT("NeedName", "Enter a Mod Name.");
    FText Error;
    if (!FPluginUtils::ValidateNewPluginNameAndLocation(ModName, FPaths::ConvertRelativePathToFull(Wizard->GetPluginFolderPath()), &Error)) return Error;
    return FText::GetEmpty();
}

FReply SCrimeFactoryCreateMod::Create()
{
    if (!Validation().IsEmpty()) return FReply::Handled();
    const auto Template = Wizard->GetSelectedTemplate();
    const auto Current = FModTemplateDescription::FromFolder(Template->OnDiskPath);
    if (!Current)
    {
        Refresh();
        FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("TemplateRemoved", "The template is no longer valid. Select a template after refreshing."));
        return FReply::Handled();
    }
    for (const auto& Dependency : Current->Dependencies)
    {
        if (!IPluginManager::Get().FindPlugin(Dependency->Name))
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("MissingDependency", "Install the required plugin '{0}' before using this template."), FText::FromString(Dependency->Name)));
            return FReply::Handled();
        }
    }
    bCreating = true;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().WaitForCompletion();
    FPluginUtils::FNewPluginParamsWithDescriptor Params;
    Params.TemplateFolders = Wizard->GetFoldersForSelection();
    Params.Descriptor.bCanContainContent = true;
    Params.Descriptor.FriendlyName = Metadata->FriendlyName.IsEmpty() ? ModName : Metadata->FriendlyName;
    Params.Descriptor.Version = 1;
    Params.Descriptor.VersionName = TEXT("1.0");
    Params.Descriptor.Category = TEXT("Other");
    Params.Descriptor.CreatedBy = Metadata->CreatedBy;
    Params.Descriptor.CreatedByURL = Metadata->CreatedByURL;
    Params.Descriptor.Description = Metadata->Description;
    Params.Descriptor.bIsBetaVersion = false;
    TArray<TSharedPtr<FJsonValue>> Authors;
    for (const FString& Author : Metadata->ModAuthors) Authors.Add(MakeShared<FJsonValueString>(Author));
    Params.Descriptor.AdditionalFieldsToWrite.Add(TEXT("ModAuthors"), MakeShared<FJsonValueArray>(Authors));
    Params.Descriptor.AdditionalFieldsToWrite.Add(TEXT("ModPageURL"), MakeShared<FJsonValueString>(Metadata->ModPageURL));
    Wizard->IconOverridePath = Metadata->ChosenIconPath;
    FText Error;
    const auto Plugin = Wizard->CreateModFromSelection(ModName, FPaths::ConvertRelativePathToFull(Wizard->GetPluginFolderPath()), Params.Descriptor, true, Error);
    bCreating = false;
    if (!Plugin || !Wizard->bPostCreationSucceeded)
    {
        if (Error.IsEmpty()) Error = LOCTEXT("PostFailed", "The mod was copied, but template asset processing failed. See the Output Log. The mod has been retained to avoid deleting loaded assets.");
        FMessageDialog::Open(EAppMsgType::Ok, Error);
    }
    else
    {
        Template->OnPluginCreated(Plugin);
        if (OnCreated) OnCreated(*Plugin);
        FNotificationInfo Info(FText::Format(LOCTEXT("Created", "'{0}' was created successfully."), FText::FromString(ModName)));
        Info.bUseThrobber = false;
        Info.ExpireDuration = 8.0f;
        if (auto Notification = FSlateNotificationManager::Get().AddNotification(Info))
            Notification->SetCompletionState(SNotificationItem::CS_Success);
        if (auto Owner = Tab.Pin()) Owner->RequestCloseTab();
    }
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
