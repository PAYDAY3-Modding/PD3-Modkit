#include "AssetPalette/SCrimeFactoryAssetPalette.h"
#include "AssetPalette/SCrimeFactoryPaletteTile.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryAssetPalette"

namespace
{
    void Notify(const FText& Message)
    {
        FNotificationInfo Info(Message);
        Info.ExpireDuration = 5.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
}

void SCrimeFactoryAssetPalette::Construct(const FArguments& Args)
{
    Registry = &FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
    ThumbnailPool = MakeShared<FAssetThumbnailPool>(128, 0.003, 0);
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(6)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [ SAssignNew(SearchBox, SSearchBox).HintText(LOCTEXT("SearchHint", "Search this category…"))
                .OnTextChanged(this, &SCrimeFactoryAssetPalette::SetSearchText) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
            [ SNew(SButton).Text(LOCTEXT("Configure", "Configure Palette"))
                .OnClicked_Lambda([]
                {
                    FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer(
                        "Project", "CrimeFactory", "AssetPalette");
                    return FReply::Handled();
                }) ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(6, 0, 6, 6)
        [ SNew(SScrollBox).Orientation(Orient_Horizontal)
            + SScrollBox::Slot()[SAssignNew(CategoryTabs, SHorizontalBox)] ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SNew(SOverlay)
            + SOverlay::Slot()
            [ SAssignNew(Grid, STileView<TSharedPtr<FCrimeFactoryPaletteItem>>)
                .ListItemsSource(&VisibleItems).ItemWidth(144).ItemHeight(176)
                .SelectionMode(ESelectionMode::Single)
                .OnGenerateTile(this, &SCrimeFactoryAssetPalette::GenerateTile)
                .OnContextMenuOpening(this, &SCrimeFactoryAssetPalette::OpenContextMenu)
                .OnMouseButtonDoubleClick(this, &SCrimeFactoryAssetPalette::OpenAsset) ]
            + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16)
            [ SNew(STextBlock).Text(this, &SCrimeFactoryAssetPalette::GetEmptyText).AutoWrapText(true)
                .Visibility_Lambda([this] { return VisibleItems.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; }) ]
        ]
    ];
    SettingsChangedHandle = GetMutableDefault<UCrimeFactoryPaletteSettings>()->OnSettingChanged().AddSP(
        this, &SCrimeFactoryAssetPalette::OnSettingsChanged);
    Registry->OnAssetAdded().AddSP(this, &SCrimeFactoryAssetPalette::OnAssetChanged);
    Registry->OnAssetRemoved().AddSP(this, &SCrimeFactoryAssetPalette::OnAssetChanged);
    Registry->OnAssetUpdated().AddSP(this, &SCrimeFactoryAssetPalette::OnAssetChanged);
    Registry->OnAssetRenamed().AddSP(this, &SCrimeFactoryAssetPalette::OnAssetRenamed);
    Registry->OnFilesLoaded().AddSP(this, &SCrimeFactoryAssetPalette::QueueRegistryRefresh);
    RefreshFromSettings();
}

SCrimeFactoryAssetPalette::~SCrimeFactoryAssetPalette()
{
    if (UObjectInitialized())
        GetMutableDefault<UCrimeFactoryPaletteSettings>()->OnSettingChanged().Remove(SettingsChangedHandle);
    if (Registry && FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
    {
        Registry->OnAssetAdded().RemoveAll(this);
        Registry->OnAssetRemoved().RemoveAll(this);
        Registry->OnAssetUpdated().RemoveAll(this);
        Registry->OnAssetRenamed().RemoveAll(this);
        Registry->OnFilesLoaded().RemoveAll(this);
    }
}

void SCrimeFactoryAssetPalette::OnSettingsChanged(UObject* Object, FPropertyChangedEvent& Event)
{
    RefreshFromSettings();
}

void SCrimeFactoryAssetPalette::RefreshFromSettings()
{
    const auto& Categories = GetDefault<UCrimeFactoryPaletteSettings>()->Categories;
    if (!Categories.IsValidIndex(SelectedCategory) || Categories[SelectedCategory].CategoryName != SelectedCategoryName)
    {
        const int32 Previous = Categories.IndexOfByPredicate([this](const auto& C) { return C.CategoryName == SelectedCategoryName; });
        SelectedCategory = Previous != INDEX_NONE ? Previous : (Categories.IsEmpty() ? INDEX_NONE : FMath::Clamp(SelectedCategory, 0, Categories.Num() - 1));
    }
    CategoryTabs->ClearChildren();
    for (int32 Index = 0; Index < Categories.Num(); ++Index)
    {
        CategoryTabs->AddSlot().AutoWidth().Padding(0, 0, 4, 0)
        [ SNew(SCheckBox).Style(FAppStyle::Get(), "ToggleButtonCheckbox")
            .IsChecked_Lambda([this, Index] { return SelectedCategory == Index ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State) { if (State == ECheckBoxState::Checked) SelectCategory(Index); })
            [ SNew(STextBlock).Text(CrimeFactoryPalette::CategoryLabel(Categories[Index], Index)).Margin(FMargin(8, 4)) ] ];
    }
    SelectCategory(SelectedCategory);
}

void SCrimeFactoryAssetPalette::SelectCategory(int32 Index)
{
    const auto& Categories = GetDefault<UCrimeFactoryPaletteSettings>()->Categories;
    SelectedCategory = Categories.IsValidIndex(Index) ? Index : INDEX_NONE;
    SelectedCategoryName = SelectedCategory == INDEX_NONE ? NAME_None : Categories[Index].CategoryName;
    RebuildCategoryItems();
}

void SCrimeFactoryAssetPalette::RebuildCategoryItems()
{
    CategoryItems.Reset();
    WatchedPackages.Reset();
    const auto& Categories = GetDefault<UCrimeFactoryPaletteSettings>()->Categories;
    if (Categories.IsValidIndex(SelectedCategory))
    {
        for (auto Entry : Categories[SelectedCategory].Assets)
        {
            const FSoftObjectPath Original = !Entry.ActorClass.IsNull() ? Entry.ActorClass.ToSoftObjectPath() : Entry.Asset.ToSoftObjectPath();
            FSoftObjectPath ResolvedPath = Original;
            TSet<FSoftObjectPath> Visited;
            while (const auto* NewPath = RenamedAssets.Find(ResolvedPath))
            {
                if (Visited.Contains(ResolvedPath)) break;
                Visited.Add(ResolvedPath);
                ResolvedPath = *NewPath;
            }
            if (!Entry.ActorClass.IsNull()) Entry.ActorClass = TSoftClassPtr<AActor>(ResolvedPath);
            else Entry.Asset = TSoftObjectPtr<UObject>(ResolvedPath);
            auto Item = MakeShared<FCrimeFactoryPaletteItem>(CrimeFactoryPalette::Resolve(Entry, *Registry));
            WatchedPackages.Add(Original.GetLongPackageFName());
            WatchedPackages.Add(Item->Path.GetLongPackageFName());
            CategoryItems.Add(MoveTemp(Item)); // Preserve duplicates and configured ordering.
        }
    }
    ApplySearch();
}

void SCrimeFactoryAssetPalette::SetSearchText(const FText& Text)
{
    Search = Text.ToString().TrimStartAndEnd();
    ApplySearch();
}

void SCrimeFactoryAssetPalette::ApplySearch()
{
    VisibleItems.Reset();
    for (const auto& Item : CategoryItems) if (Item->MatchesSearch(Search)) VisibleItems.Add(Item);
    if (Grid) Grid->RequestListRefresh();
}

void SCrimeFactoryAssetPalette::OnAssetChanged(const FAssetData& Asset)
{
    if (WatchedPackages.Contains(Asset.PackageName)) QueueRegistryRefresh();
}

void SCrimeFactoryAssetPalette::OnAssetRenamed(const FAssetData& Asset, const FString& OldPath)
{
    // Remember renames even while another category is selected. Follow chains on rebuild
    // so several renames in the same frame also resolve before redirectors are saved.
    RenamedAssets.Add(FSoftObjectPath(OldPath), Asset.GetSoftObjectPath());
    RenamedAssets.Add(FSoftObjectPath(OldPath + TEXT("_C")), FSoftObjectPath(Asset.GetSoftObjectPath().ToString() + TEXT("_C")));
    if (WatchedPackages.Contains(FSoftObjectPath(OldPath).GetLongPackageFName()))
    {
        QueueRegistryRefresh();
    }
}

void SCrimeFactoryAssetPalette::QueueRegistryRefresh()
{
    if (!bRegistryRefreshPending)
    {
        bRegistryRefreshPending = true;
        RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrimeFactoryAssetPalette::RefreshRegistry));
    }
}

EActiveTimerReturnType SCrimeFactoryAssetPalette::RefreshRegistry(double Time, float Delta)
{
    bRegistryRefreshPending = false;
    RebuildCategoryItems();
    return EActiveTimerReturnType::Stop;
}

TSharedRef<ITableRow> SCrimeFactoryAssetPalette::GenerateTile(TSharedPtr<FCrimeFactoryPaletteItem> Item,
    const TSharedRef<STableViewBase>& Owner)
{
    return SNew(SCrimeFactoryPaletteTile, Owner).Item(Item).ThumbnailPool(ThumbnailPool)
        .OnAssetDragDetected(this, &SCrimeFactoryAssetPalette::DragTile, Item);
}

FReply SCrimeFactoryAssetPalette::DragTile(const FGeometry& Geometry, const FPointerEvent& Event,
    TSharedPtr<FCrimeFactoryPaletteItem> Item)
{
    FText Error;
    if (auto Operation = CrimeFactoryPalette::CreateDrag(*Item, *Registry, Error))
        return FReply::Handled().BeginDragDrop(Operation.ToSharedRef());
    Notify(Error);
    return FReply::Handled();
}

TSharedPtr<SWidget> SCrimeFactoryAssetPalette::OpenContextMenu()
{
    const auto Selection = Grid->GetSelectedItems();
    if (Selection.IsEmpty()) return nullptr;
    const auto Item = Selection[0];
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(LOCTEXT("Find", "Find in Content Browser"), FText::GetEmpty(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SCrimeFactoryAssetPalette::FindAsset, Item)));
    Menu.AddMenuEntry(LOCTEXT("CopyPath", "Copy Asset Path"), FText::GetEmpty(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([Item] { FPlatformApplicationMisc::ClipboardCopy(*Item->Path.ToString()); })));
    Menu.AddMenuEntry(LOCTEXT("Edit", "Open/Edit Asset"), FText::GetEmpty(), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SCrimeFactoryAssetPalette::OpenAsset, Item)));
    return Menu.MakeWidget();
}

void SCrimeFactoryAssetPalette::FindAsset(TSharedPtr<FCrimeFactoryPaletteItem> Item)
{
    const auto Current = CrimeFactoryPalette::Resolve(Item->Entry, *Registry);
    if (Current.AssetData.IsValid())
    {
        TArray<FAssetData> Assets{Current.AssetData};
        FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
    }
    else Notify(LOCTEXT("CannotFind", "This reference is missing or has no Content Browser entry. Reselect it in Project Settings."));
}

void SCrimeFactoryAssetPalette::OpenAsset(TSharedPtr<FCrimeFactoryPaletteItem> Item)
{
    const auto Current = CrimeFactoryPalette::Resolve(Item->Entry, *Registry);
    if (!Current.Problem.IsEmpty()) { Notify(Current.Problem); return; }
    UObject* Object = Current.bClassEntry ? Current.Path.TryLoad() : Current.AssetData.GetAsset();
    UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!Object || !Editors || !Editors->OpenEditorForAsset(Object))
        Notify(LOCTEXT("CannotEdit", "The asset could not be opened, or no asset editor supports this type. Check the Output Log."));
}

FText SCrimeFactoryAssetPalette::GetEmptyText() const
{
    if (SelectedCategory == INDEX_NONE) return LOCTEXT("NoCategories", "No categories configured. Use Configure Palette to add categories and assets.");
    if (CategoryItems.IsEmpty()) return LOCTEXT("EmptyCategory", "This category has no assets. Add assets in Project Settings.");
    return LOCTEXT("NoMatches", "No assets match this search.");
}

#undef LOCTEXT_NAMESPACE
