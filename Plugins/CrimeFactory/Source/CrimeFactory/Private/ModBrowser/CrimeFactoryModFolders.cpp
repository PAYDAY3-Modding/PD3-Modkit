#include "ModBrowser/CrimeFactoryModEntryList.h"
#include "ModBrowser/CrimeFactoryModEntry.h"
#include "ModBrowser/CrimeFactoryModOrganisation.h"
#include "Editor/CrimeFactory.h"

#include "DragAndDrop/DecoratedDragDropOp.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "InputCoreTypes.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryModFolders"

struct FCrimeFactoryFolderTreeItem
{
    FString Id;
    FText Name;
    TArray<TSharedPtr<FCrimeFactoryFolderTreeItem>> Children;
};

/** Contains virtual IDs only. A drop cannot move or rename anything on disk. */
class FCrimeFactoryOrganisationDrag : public FDecoratedDragDropOp
{
public:
    DRAG_DROP_OPERATOR_TYPE(FCrimeFactoryOrganisationDrag, FDecoratedDragDropOp)
    TWeakPtr<SCrimeFactoryModEntryList> Source;
    TArray<FString> Mods;
    FString Folder;

    static TSharedRef<FCrimeFactoryOrganisationDrag> New(const TSharedRef<SCrimeFactoryModEntryList>& Owner,
        const TArray<FString>& InMods, const FString& InFolder, const FText& Label)
    {
        auto Operation = MakeShared<FCrimeFactoryOrganisationDrag>();
        Operation->Source = Owner;
        Operation->Mods = InMods;
        Operation->Folder = InFolder;
        Operation->DefaultHoverText = Label;
        Operation->CurrentHoverText = Label;
        Operation->Construct();
        return Operation;
    }
};

/** Folder tiles and tree rows use the same model and drag payload. */
class SCrimeFactoryFolderTile : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SCrimeFactoryFolderTile) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&, TSharedRef<SCrimeFactoryModEntryList> InOwner, const FString& InFolder)
    {
        Owner = InOwner;
        Folder = InFolder;
        const FCrimeFactoryModFolder* Node = InOwner->Organisation->Find(Folder);
        ChildSlot
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .BorderBackgroundColor_Lambda([this] { return bValidDrop ? FLinearColor(0.15f, 0.5f, 0.9f) : FLinearColor::White; })
            .Padding(10)
            [
                SNew(SBox).WidthOverride(170).HeightOverride(115)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().FillHeight(1).HAlign(HAlign_Center).VAlign(VAlign_Center)
                    [ SNew(SBox).WidthOverride(58).HeightOverride(58)
                      [ SNew(SImage).Image(FAppStyle::GetBrush("ContentBrowser.AssetTreeFolderOpen")) ] ]
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
                    [ SNew(STextBlock).Text(Node ? FText::FromString(Node->Name) : FText::GetEmpty()).AutoWrapText(true) ]
                ]
            ]
        ];
        SetToolTipText(LOCTEXT("FolderTileHelp", "Double-click to open. Drag onto another folder to move. Right-click for folder actions."));
    }
    virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& Event) override
    {
        return Event.GetEffectingButton() == EKeys::LeftMouseButton
            ? FReply::Handled().DetectDrag(SharedThis(this), EKeys::LeftMouseButton) : FReply::Unhandled();
    }
    virtual FReply OnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent& Event) override
    {
        if (auto Pinned = Owner.Pin(); Pinned && Event.GetEffectingButton() == EKeys::LeftMouseButton)
        { Pinned->NavigateTo(Folder); return FReply::Handled(); }
        return FReply::Unhandled();
    }
    virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event) override
    {
        if (auto Pinned = Owner.Pin(); Pinned && Event.GetEffectingButton() == EKeys::RightMouseButton)
        { Pinned->OpenFolderMenu(Folder, Event.GetScreenSpacePosition()); return FReply::Handled(); }
        return FReply::Unhandled();
    }
    virtual FReply OnDragDetected(const FGeometry&, const FPointerEvent&) override
    {
        if (auto Pinned = Owner.Pin()) return Pinned->BeginFolderDrag(Folder);
        return FReply::Unhandled();
    }
    virtual FReply OnDragOver(const FGeometry&, const FDragDropEvent& Event) override
    {
        auto Pinned = Owner.Pin();
        bValidDrop = Pinned && Pinned->CanAcceptFolderDrop(Event, Folder);
        return bValidDrop ? FReply::Handled() : FReply::Unhandled();
    }
    virtual void OnDragLeave(const FDragDropEvent&) override { bValidDrop = false; }
    virtual FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
    {
        bValidDrop = false;
        if (auto Pinned = Owner.Pin()) return Pinned->AcceptFolderDrop(Event, Folder);
        return FReply::Unhandled();
    }
private:
    FString Folder;
    TWeakPtr<SCrimeFactoryModEntryList> Owner;
    bool bValidDrop = false;
};

TSharedRef<SWidget> SCrimeFactoryModEntryList::MakeOrganisationBrowser()
{
    auto Widget = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 3)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("Folders", "Folders"))
              .ToolTipText(LOCTEXT("ToggleFolders", "Show or hide virtual folders. Check mod cards to move several mods together."))
              .OnClicked_Lambda([this] { Organisation->bShowSidebar = !Organisation->bShowSidebar; SaveOrganisation(); return FReply::Handled(); }) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("Back", "<")).ToolTipText(LOCTEXT("BackHelp", "Back"))
              .IsEnabled_Lambda([this] { return HistoryIndex > 0; })
              .OnClicked(this, &SCrimeFactoryModEntryList::NavigateHistory, -1) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("Forward", ">")).ToolTipText(LOCTEXT("ForwardHelp", "Forward"))
              .IsEnabled_Lambda([this] { return HistoryIndex + 1 < NavigationHistory.Num(); })
              .OnClicked(this, &SCrimeFactoryModEntryList::NavigateHistory, 1) ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("Up", "Up"))
              .IsEnabled_Lambda([this] { return Organisation->CurrentFolder != FCrimeFactoryModOrganisation::AllMods; })
              .OnClicked(this, &SCrimeFactoryModEntryList::NavigateParent) ]
            + SHorizontalBox::Slot().FillWidth(1).Padding(5, 0)
            [ SNew(SScrollBox).Orientation(Orient_Horizontal)
              + SScrollBox::Slot() [ SAssignNew(Breadcrumbs, SHorizontalBox) ] ]
            + SHorizontalBox::Slot().AutoWidth()
            [ SNew(SButton).Text(LOCTEXT("NewFolder", "New Folder"))
              .OnClicked_Lambda([this]
              {
                  EditFolder(Organisation->CurrentFolder == FCrimeFactoryModOrganisation::AllMods ? FString() : Organisation->CurrentFolder, false);
                  return FReply::Handled();
              }) ]
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SAssignNew(BrowserSplitter, SSplitter)
            .OnSplitterFinishedResizing_Lambda([this] { SaveOrganisation(); })
            + SSplitter::Slot()
            .Value_Lambda([this] { return Organisation->SidebarFraction; })
            .OnSlotResized_Lambda([this](float Size) { Organisation->SidebarFraction = FMath::Clamp(Size, 0.12f, 0.6f); })
            [
                SNew(SBox).Visibility_Lambda([this] { return Organisation->bShowSidebar ? EVisibility::Visible : EVisibility::Collapsed; })
                [
                    SAssignNew(FolderTree, STreeView<TSharedPtr<FCrimeFactoryFolderTreeItem>>)
                    .TreeItemsSource(&FolderRoots)
                    .SelectionMode(ESelectionMode::Single)
                    .OnGenerateRow(this, &SCrimeFactoryModEntryList::GenerateFolderRow)
                    .OnGetChildren(this, &SCrimeFactoryModEntryList::GetFolderChildren)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FCrimeFactoryFolderTreeItem> Item, ESelectInfo::Type)
                    { if (Item && !bUpdatingTree) NavigateTo(Item->Id); })
                    .OnExpansionChanged_Lambda([this](TSharedPtr<FCrimeFactoryFolderTreeItem> Item, bool bExpanded)
                    {
                        if (!Item || bUpdatingTree || Item->Id == FCrimeFactoryModOrganisation::AllMods) return;
                        if (bExpanded) Organisation->ExpandedFolders.Add(Item->Id);
                        else Organisation->ExpandedFolders.Remove(Item->Id);
                        SaveOrganisation();
                    })
                    .OnContextMenuOpening(this, &SCrimeFactoryModEntryList::MakeFolderMenu)
                ]
            ]
            + SSplitter::Slot().Value_Lambda([this] { return 1 - Organisation->SidebarFraction; })
            [
                SNew(SScrollBox).ScrollBarAlwaysVisible(true)
                + SScrollBox::Slot()
                [ SAssignNew(ModGrid, SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4, 4)).HAlign(HAlign_Left) ]
            ]
        ];
    RefreshFolderTree();
    NavigateTo(Organisation->CurrentFolder);
    return Widget;
}

void SCrimeFactoryModEntryList::RefreshFolderTree()
{
    TGuardValue<bool> Guard(bUpdatingTree, true);
    FolderRoots.Reset();
    FolderItems.Reset();
    auto All = MakeShared<FCrimeFactoryFolderTreeItem>();
    All->Id = FCrimeFactoryModOrganisation::AllMods;
    All->Name = LOCTEXT("AllMods", "All Mods");
    FolderRoots.Add(All);
    FolderItems.Add(All->Id, All);
    auto Root = MakeShared<FCrimeFactoryFolderTreeItem>();
    Root->Name = LOCTEXT("RootMods", "Root");
    FolderRoots.Add(Root);
    FolderItems.Add(FString(), Root);
    TArray<TSharedPtr<FCrimeFactoryFolderTreeItem>> PendingFolders { Root };
    for (int32 Index = 0; Index < PendingFolders.Num(); ++Index)
    {
        const auto Item = PendingFolders[Index];
        for (const auto& Folder : Organisation->Children(Item->Id))
        {
            auto Child = MakeShared<FCrimeFactoryFolderTreeItem>();
            Child->Id = Folder.Id;
            Child->Name = FText::FromString(Folder.Name);
            Item->Children.Add(Child);
            FolderItems.Add(Child->Id, Child);
            PendingFolders.Add(Child);
        }
    }
    FolderTree->RequestTreeRefresh();
    for (const auto& Pair : FolderItems)
        FolderTree->SetItemExpansion(Pair.Value, Organisation->ExpandedFolders.Contains(Pair.Key));
    if (const auto* Selected = FolderItems.Find(Organisation->CurrentFolder)) FolderTree->SetSelection(*Selected);
}

void SCrimeFactoryModEntryList::GetFolderChildren(TSharedPtr<FCrimeFactoryFolderTreeItem> Item,
    TArray<TSharedPtr<FCrimeFactoryFolderTreeItem>>& Children) const
{
    if (Item) Children = Item->Children;
}

TSharedRef<ITableRow> SCrimeFactoryModEntryList::GenerateFolderRow(TSharedPtr<FCrimeFactoryFolderTreeItem> Item,
    const TSharedRef<STableViewBase>& Table)
{
    return SNew(STableRow<TSharedPtr<FCrimeFactoryFolderTreeItem>>, Table)
        .OnDragDetected_Lambda([this, Item](const FGeometry&, const FPointerEvent&) { return BeginFolderDrag(Item->Id); })
        .OnCanAcceptDrop_Lambda([this](const FDragDropEvent& Event, EItemDropZone, TSharedPtr<FCrimeFactoryFolderTreeItem> Target)
        {
            return Target && CanAcceptFolderDrop(Event, Target->Id) ? TOptional<EItemDropZone>(EItemDropZone::OntoItem) : TOptional<EItemDropZone>();
        })
        .OnAcceptDrop_Lambda([this](const FDragDropEvent& Event, EItemDropZone, TSharedPtr<FCrimeFactoryFolderTreeItem> Target)
        { return Target ? AcceptFolderDrop(Event, Target->Id) : FReply::Unhandled(); })
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2, 0, 5, 0)
            [ SNew(SImage).Image(FAppStyle::GetBrush("ContentBrowser.AssetTreeFolderOpen")) ]
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
            [ SNew(STextBlock).Text(Item->Name).ToolTipText(Item->Name) ]
        ];
}

void SCrimeFactoryModEntryList::AddFolderTile(const FString& Folder)
{
    ModGrid->AddSlot().Padding(4) [ SNew(SCrimeFactoryFolderTile, SharedThis(this), Folder) ];
}

void SCrimeFactoryModEntryList::RefreshFolderContents()
{
    if (Organisation->CurrentFolder != FCrimeFactoryModOrganisation::AllMods && !Organisation->HasFolder(Organisation->CurrentFolder))
        Organisation->CurrentFolder = FString();
    RebuildBreadcrumbs();
    RebuildModGrid();
}

void SCrimeFactoryModEntryList::NavigateTo(const FString& Folder, bool bAddHistory)
{
    if (Folder != FCrimeFactoryModOrganisation::AllMods && !Organisation->HasFolder(Folder)) return;
    const bool bChanged = Organisation->CurrentFolder != Folder;
    Organisation->CurrentFolder = Folder;
    if (bAddHistory && (HistoryIndex == INDEX_NONE || NavigationHistory[HistoryIndex] != Folder))
    {
        NavigationHistory.SetNum(HistoryIndex + 1);
        NavigationHistory.Add(Folder);
        HistoryIndex = NavigationHistory.Num() - 1;
    }
    {
        TGuardValue<bool> Guard(bUpdatingTree, true);
        FString Ancestor = Folder;
        while (const auto* Node = Organisation->Find(Ancestor))
        {
            Ancestor = Node->Parent;
            if (const auto* Parent = FolderItems.Find(Ancestor))
            {
                FolderTree->SetItemExpansion(*Parent, true);
                Organisation->ExpandedFolders.Add(Ancestor);
            }
        }
        if (const auto* Item = FolderItems.Find(Folder)) FolderTree->SetSelection(*Item);
    }
    RefreshFolderContents();
    if (bChanged) SaveOrganisation();
}

FReply SCrimeFactoryModEntryList::NavigateHistory(int32 Direction)
{
    for (int32 Index = HistoryIndex + Direction; NavigationHistory.IsValidIndex(Index); Index += Direction)
    {
        const FString Folder = NavigationHistory[Index];
        if (Folder == FCrimeFactoryModOrganisation::AllMods || Organisation->HasFolder(Folder))
        { HistoryIndex = Index; NavigateTo(Folder, false); break; }
    }
    return FReply::Handled();
}

FReply SCrimeFactoryModEntryList::NavigateParent()
{
    const auto* Node = Organisation->Find(Organisation->CurrentFolder);
    NavigateTo(Node ? Node->Parent : FCrimeFactoryModOrganisation::AllMods);
    return FReply::Handled();
}

void SCrimeFactoryModEntryList::RebuildBreadcrumbs()
{
    if (!Breadcrumbs) return;
    Breadcrumbs->ClearChildren();
    TArray<TPair<FString, FText>> Path;
    FString Cursor = Organisation->CurrentFolder;
    while (const auto* Folder = Organisation->Find(Cursor))
    {
        Path.Insert({ Cursor, FText::FromString(Folder->Name) }, 0);
        Cursor = Folder->Parent;
    }
    if (Organisation->CurrentFolder != FCrimeFactoryModOrganisation::AllMods)
        Path.Insert({ FString(), LOCTEXT("RootBreadcrumb", "Root") }, 0);
    Path.Insert({ FCrimeFactoryModOrganisation::AllMods, LOCTEXT("AllBreadcrumb", "All Mods") }, 0);
    for (const auto& Part : Path)
    {
        Breadcrumbs->AddSlot().AutoWidth().VAlign(VAlign_Center)
        [ SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(Part.Value)
          .OnClicked_Lambda([this, Id = Part.Key] { NavigateTo(Id); return FReply::Handled(); }) ];
        if (Part.Key != Organisation->CurrentFolder)
            Breadcrumbs->AddSlot().AutoWidth().VAlign(VAlign_Center) [ SNew(STextBlock).Text(FText::FromString(TEXT(" / "))) ];
    }
}

void SCrimeFactoryModEntryList::ShowOrganisationError(const FText& Error)
{
    UE_LOG(LogCrimeFactory, Warning, TEXT("%s"), *Error.ToString());
    FNotificationInfo Info(Error);
    Info.ExpireDuration = 7;
    FSlateNotificationManager::Get().AddNotification(Info);
}

void SCrimeFactoryModEntryList::SaveOrganisation()
{
    FText Error;
    if (!Organisation->Save(Error)) ShowOrganisationError(Error);
}

void SCrimeFactoryModEntryList::EditFolder(const FString& Folder, bool bRename)
{
    if ((bRename && !Organisation->Find(Folder)) || (!bRename && !Organisation->HasFolder(Folder))) return;
    const TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(bRename ? LOCTEXT("RenameTitle", "Rename Virtual Folder") : LOCTEXT("CreateTitle", "New Virtual Folder"))
        .ClientSize(FVector2D(420, 145)).SupportsMaximize(false).SupportsMinimize(false).SizingRule(ESizingRule::FixedSize);
    const TWeakPtr<SWindow> WeakWindow = Window;
    const TWeakPtr<SCrimeFactoryModEntryList> WeakOwner = SharedThis(this);
    TSharedPtr<SEditableTextBox> Name;
    TSharedPtr<STextBlock> ErrorText = SNew(STextBlock).ColorAndOpacity(FLinearColor(1, 0.35f, 0.25f)).AutoWrapText(true);
    auto Submit = [WeakOwner, WeakWindow, Folder, bRename](const FString& Text, const TSharedPtr<STextBlock>& Errors)
    {
        const auto Owner = WeakOwner.Pin();
        if (!Owner) return;
        FText Error;
        FString Id = Folder;
        const bool bSuccess = bRename ? Owner->Organisation->RenameFolder(Folder, Text, Error)
            : Owner->Organisation->CreateFolder(Folder, Text, Id, Error);
        if (!bSuccess) { Errors->SetText(Error); return; }
        Owner->Organisation->ExpandedFolders.Add(bRename ? Owner->Organisation->Find(Id)->Parent : Folder);
        Owner->RefreshFolderTree();
        Owner->RefreshFolderContents();
        Owner->SaveOrganisation();
        if (auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow();
    };
    Window->SetContent(SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(12)
        [ SAssignNew(Name, SEditableTextBox)
          .Text(bRename ? FText::FromString(Organisation->Find(Folder)->Name) : FText::GetEmpty())
          .OnTextCommitted_Lambda([Submit, ErrorText](const FText& Text, ETextCommit::Type Commit)
          { if (Commit == ETextCommit::OnEnter) Submit(Text.ToString(), ErrorText); })
          .HintText(LOCTEXT("FolderName", "Folder name")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(12, 0)
        [ ErrorText.ToSharedRef() ]
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(12)
        [ SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth()
          [ SNew(SButton).Text(LOCTEXT("Cancel", "Cancel")).OnClicked_Lambda([WeakWindow]
            { if (auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow(); return FReply::Handled(); }) ]
          + SHorizontalBox::Slot().AutoWidth().Padding(5, 0, 0, 0)
          [ SNew(SButton).Text(LOCTEXT("SaveFolder", "Save")).OnClicked_Lambda([Submit, Name, ErrorText]
            { Submit(Name->GetText().ToString(), ErrorText); return FReply::Handled(); }) ] ]);
    FSlateApplication::Get().AddModalWindow(Window, SharedThis(this));
}

void SCrimeFactoryModEntryList::DeleteFolder(const FString& Folder)
{
    const auto* Node = Organisation->Find(Folder);
    if (!Node) return;
    const FText Message = FText::Format(LOCTEXT("ConfirmFolderDelete",
        "Delete the virtual folder '{0}' and all its subfolders?\n\nAll mods assigned anywhere inside this hierarchy will be moved to its parent folder. No mods, plugin directories or assets will be deleted."), FText::FromString(Node->Name));
    if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes) return;
    if (Organisation->DeleteFolder(Folder))
    {
        RefreshFolderTree();
        RefreshFolderContents();
        SaveOrganisation();
    }
}

void SCrimeFactoryModEntryList::OpenFolderMenu(const FString& Folder, const FVector2D& Position)
{
    if (auto Menu = BuildFolderMenu(Folder)) FSlateApplication::Get().PushMenu(SharedThis(this), FWidgetPath(), Menu.ToSharedRef(), Position, FPopupTransitionEffect::ContextMenu);
}

TSharedPtr<SWidget> SCrimeFactoryModEntryList::MakeFolderMenu()
{
    const auto Selected = FolderTree->GetSelectedItems();
    if (Selected.IsEmpty()) return nullptr;
    return BuildFolderMenu(Selected[0]->Id);
}

TSharedPtr<SWidget> SCrimeFactoryModEntryList::BuildFolderMenu(const FString& Id)
{
    if (Id != FCrimeFactoryModOrganisation::AllMods && !Organisation->HasFolder(Id)) return nullptr;
    const FString Parent = Id == FCrimeFactoryModOrganisation::AllMods ? FString() : Id;
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(LOCTEXT("OpenFolder", "Open"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this, Id] { NavigateTo(Id); })));
    Menu.AddMenuEntry(LOCTEXT("NewSubfolder", "New Folder"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this, Parent] { EditFolder(Parent, false); })));
    if (Organisation->Find(Id))
    {
        Menu.AddMenuEntry(LOCTEXT("RenameFolder", "Rename"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this, Id] { EditFolder(Id, true); })));
        Menu.AddMenuEntry(LOCTEXT("DeleteFolder", "Delete"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(this, [this, Id] { DeleteFolder(Id); })));
    }
    return Menu.MakeWidget();
}

TArray<FString> SCrimeFactoryModEntryList::ModMoveSelection(const TSharedRef<IPlugin>& Mod) const
{
    const FString Key = FCrimeFactoryModOrganisation::ModKey(*Mod);
    const auto* Clicked = CachedEntries.Find(Key);
    TArray<FString> Keys;
    if (Clicked && (*Clicked)->IsSelected())
    {
        for (const auto& Pair : CachedEntries)
            if (VisibleEntries.Contains(Pair.Value) && Pair.Value->IsSelected()) Keys.Add(Pair.Key);
    }
    else Keys.Add(Key);
    return Keys;
}

FReply SCrimeFactoryModEntryList::BeginModDrag(const TSharedRef<IPlugin>& Mod)
{
    const TArray<FString> Keys = ModMoveSelection(Mod);
    return FReply::Handled().BeginDragDrop(FCrimeFactoryOrganisationDrag::New(SharedThis(this), Keys, FString(),
        FText::Format(LOCTEXT("DragMods", "Move {0} mod(s) to a virtual folder"), Keys.Num())));
}

FReply SCrimeFactoryModEntryList::BeginFolderDrag(const FString& Folder)
{
    const auto* Node = Organisation->Find(Folder);
    if (!Node) return FReply::Unhandled();
    return FReply::Handled().BeginDragDrop(FCrimeFactoryOrganisationDrag::New(SharedThis(this), {}, Folder, FText::FromString(Node->Name)));
}

bool SCrimeFactoryModEntryList::CanAcceptFolderDrop(const FDragDropEvent& Event, const FString& Destination) const
{
    const auto Operation = Event.GetOperationAs<FCrimeFactoryOrganisationDrag>();
    if (!Operation || Operation->Source.Pin().Get() != this || !Organisation->HasFolder(Destination)) return false;
    FText Error;
    return !Operation->Mods.IsEmpty() || Organisation->CanMoveFolder(Operation->Folder, Destination, Error);
}

FReply SCrimeFactoryModEntryList::AcceptFolderDrop(const FDragDropEvent& Event, const FString& Destination)
{
    if (!CanAcceptFolderDrop(Event, Destination)) return FReply::Unhandled();
    const auto Operation = Event.GetOperationAs<FCrimeFactoryOrganisationDrag>();
    if (!Operation->Mods.IsEmpty()) MoveMods(Operation->Mods, Destination);
    else
    {
        FText Error;
        if (!Organisation->MoveFolder(Operation->Folder, Destination, Error)) ShowOrganisationError(Error);
        else
        {
            Organisation->ExpandedFolders.Add(Destination);
            RefreshFolderTree();
            RefreshFolderContents();
            SaveOrganisation();
        }
    }
    return FReply::Handled();
}

void SCrimeFactoryModEntryList::MoveMods(const TArray<FString>& Keys, const FString& Destination)
{
    if (Organisation->AssignMods(Keys, Destination))
    {
        RefreshFolderContents();
        SaveOrganisation();
    }
}

void SCrimeFactoryModEntryList::AddMoveToFolderMenu(FMenuBuilder& Menu, const TSharedRef<IPlugin>& Mod)
{
    const auto Keys = ModMoveSelection(Mod);
    Menu.AddSubMenu(LOCTEXT("MoveToFolder", "Move to Folder"), LOCTEXT("MoveToFolderHelp", "Move this mod, or the visible checked mods, without moving plugin files."),
        FNewMenuDelegate::CreateLambda([WeakOwner = TWeakPtr<SCrimeFactoryModEntryList>(SharedThis(this)), Keys](FMenuBuilder& Submenu)
        {
            if (auto Owner = WeakOwner.Pin())
            {
                Submenu.AddMenuEntry(LOCTEXT("MoveRoot", "Root / Unassigned"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(Owner.ToSharedRef(), [OwnerPtr = Owner.Get(), Keys] { OwnerPtr->MoveMods(Keys, FString()); })));
                Owner->AddFolderDestinations(Submenu, FString(), Keys);
            }
        }));
}

void SCrimeFactoryModEntryList::AddFolderDestinations(FMenuBuilder& Menu, const FString& Parent, const TArray<FString>& Keys)
{
    for (const auto& Folder : Organisation->Children(Parent))
    {
        Menu.AddSubMenu(FText::FromString(Folder.Name), FText::GetEmpty(),
            FNewMenuDelegate::CreateLambda([WeakOwner = TWeakPtr<SCrimeFactoryModEntryList>(SharedThis(this)), Keys, Id = Folder.Id](FMenuBuilder& Submenu)
            {
                if (auto Owner = WeakOwner.Pin(); Owner && Owner->Organisation->HasFolder(Id))
                {
                    Submenu.AddMenuEntry(LOCTEXT("MoveHere", "Move Here"), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSPLambda(Owner.ToSharedRef(), [OwnerPtr = Owner.Get(), Keys, Id] { OwnerPtr->MoveMods(Keys, Id); })));
                    Owner->AddFolderDestinations(Submenu, Id, Keys);
                }
            }));
    }
}

#undef LOCTEXT_NAMESPACE
