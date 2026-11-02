#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Interfaces/IPluginManager.h"
#include "Widgets/Views/STreeView.h"

class FMenuBuilder;
class FCrimeFactoryModOrganisation;
struct FCrimeFactoryFolderTreeItem;

/**
 * Widget that lists all plugins that might be able to get packed as mods
 */
class SCrimeFactoryModEntryList : public SCompoundWidget {
	SLATE_BEGIN_ARGS(SCrimeFactoryModEntryList) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args);

	/**
	 * Updates the mod list.
	 */
	void LoadMods();
    FReply RefreshMods();
    int32 GetModCount() const { return Mods.Num(); }
    bool ContainsMod(const FString& Name) const;

	/**
	 * Filters the mods for the given string
	 */
	void Filter(const FString& InFilter);

	/**
	 * Returns the string which was use previously to filter
	 */
	FString GetLastFilter() const;

	/**
	 * True if engine plugins are considered being mods
	 */
	void SetShowEngine(bool bInShowEngine);
	void SetShowProject(bool bInShowProject);

	void OnNewPluginCreated(IPlugin& Plugin);

    /** Organisation uses the existing card checkboxes for multi-mod moves. */
    FReply BeginModDrag(const TSharedRef<IPlugin>& Mod);
    void AddMoveToFolderMenu(FMenuBuilder& Menu, const TSharedRef<IPlugin>& Mod);
private:
	friend class SCrimeFactoryFolderTile;
	void RebuildModGrid();
    TSharedRef<SWidget> MakeOrganisationBrowser();
    void RefreshFolderTree();
    void RefreshFolderContents();
    void AddFolderTile(const FString& Folder);
    void RebuildBreadcrumbs();
    void NavigateTo(const FString& Folder, bool bAddHistory = true);
    FReply NavigateHistory(int32 Direction);
    FReply NavigateParent();
    void SaveOrganisation();
    void ShowOrganisationError(const FText& Error);
    void EditFolder(const FString& Folder, bool bRename);
    void DeleteFolder(const FString& Folder);
    void OpenFolderMenu(const FString& Folder, const FVector2D& Position);
    TSharedPtr<SWidget> MakeFolderMenu();
    TSharedPtr<SWidget> BuildFolderMenu(const FString& Folder);
    TSharedRef<ITableRow> GenerateFolderRow(TSharedPtr<FCrimeFactoryFolderTreeItem> Item, const TSharedRef<STableViewBase>& Table);
    void GetFolderChildren(TSharedPtr<FCrimeFactoryFolderTreeItem> Item, TArray<TSharedPtr<FCrimeFactoryFolderTreeItem>>& Children) const;
    FReply BeginFolderDrag(const FString& Folder);
    bool CanAcceptFolderDrop(const FDragDropEvent& Event, const FString& Destination) const;
    FReply AcceptFolderDrop(const FDragDropEvent& Event, const FString& Destination);
    TArray<FString> ModMoveSelection(const TSharedRef<IPlugin>& Mod) const;
    void AddFolderDestinations(FMenuBuilder& Menu, const FString& Parent, const TArray<FString>& Keys);
    void MoveMods(const TArray<FString>& Keys, const FString& Destination);

    TSharedPtr<FCrimeFactoryModOrganisation> Organisation;
    TSharedPtr<STreeView<TSharedPtr<FCrimeFactoryFolderTreeItem>>> FolderTree;
    TArray<TSharedPtr<FCrimeFactoryFolderTreeItem>> FolderRoots;
    TMap<FString, TSharedPtr<FCrimeFactoryFolderTreeItem>> FolderItems;
    TSharedPtr<class SHorizontalBox> Breadcrumbs;
    TSharedPtr<class SSplitter> BrowserSplitter;
    TMap<FString, TSharedPtr<class SCrimeFactoryModEntry>> CachedEntries;
    TArray<FString> NavigationHistory;
    int32 HistoryIndex = INDEX_NONE;
    bool bUpdatingTree = false;

	TSharedPtr<class SWrapBox> ModGrid;
	TArray<TSharedPtr<class SCrimeFactoryModEntry>> VisibleEntries;
	TArray<TSharedRef<IPlugin>> Mods;
	TArray<TSharedRef<IPlugin>> FilteredMods;
	FString LastFilter;
	bool bShowEngine = false;
	bool bShowProject = false;
    bool bLoadingMods = false;
    FText RefreshStatus;

	FReply PackageAllMods();
	FReply CheckAllMods();
	FReply UncheckAllMods();
	FReply CreateMod();
	FReply ImportLegacyMod();
};
