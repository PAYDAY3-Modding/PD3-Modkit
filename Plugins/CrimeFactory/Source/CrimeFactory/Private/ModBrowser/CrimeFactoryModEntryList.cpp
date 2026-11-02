#include "ModBrowser/CrimeFactoryModEntryList.h"
#include "Editor/CrimeFactory.h"
#include "ModBrowser/CrimeFactoryModEntry.h"
#include "ModCreation/LegacyModImporter.h"
#include "Metadata/CrimeFactoryMetadataSync.h"
#include "PluginUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/ScopeExit.h"
#include "Interfaces/IPluginManager.h"
#include "Slate.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Widgets/Layout/SWrapBox.h"
#include "ModBrowser/CrimeFactoryModOrganisation.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryModListEntry"

void SCrimeFactoryModEntryList::Construct(const FArguments& Args) {
    Organisation = MakeShared<FCrimeFactoryModOrganisation>();
    FText OrganisationError;
    if (!Organisation->Load(OrganisationError)) ShowOrganisationError(OrganisationError);
    ChildSlot[
        SNew(SVerticalBox)

        // Selection actions. SWrapBox prevents these from being clipped when
        // CrimeFactory is docked into a narrow side panel.
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(FMargin(0.0f, 2.0f, 0.0f, 4.0f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(3.0f, 3.0f))

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("PackageModCrimeFactoryAll", "Forge Selected!"))
                .OnClicked(this, &SCrimeFactoryModEntryList::PackageAllMods)
            ]

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("PackageModCheckAll", "Check All"))
                .OnClicked(this, &SCrimeFactoryModEntryList::CheckAllMods)
            ]

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("PackageModUncheckAll", "Check None"))
                .OnClicked(this, &SCrimeFactoryModEntryList::UncheckAllMods)
            ]
            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("RefreshMods", "Refresh Mods"))
                .ToolTipText(LOCTEXT("RefreshModsHelp", "Rescan the Mods folder, load new content-only plugins, and refresh metadata and icons. Code plugins may require an editor restart."))
                .OnClicked(this, &SCrimeFactoryModEntryList::RefreshMods)
            ]
        ]

        // Creation/import actions get their own responsive row so they remain
        // reachable even at the narrow dock width shown in the side panel.
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(FMargin(0.0f, 0.0f, 0.0f, 5.0f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(3.0f, 3.0f))

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("ImportLegacyMod", "Import Legacy Mod"))
                .ToolTipText(LOCTEXT(
                    "ImportLegacyModTooltip",
                    "Migrate an uncooked UE4 ModLoader mod folder into a PDML/CrimeFactory mod plugin."))
                .OnClicked(this, &SCrimeFactoryModEntryList::ImportLegacyMod)
            ]

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .Text(LOCTEXT("CreateMod", "Create Mod"))
                .OnClicked(this, &SCrimeFactoryModEntryList::CreateMod)
            ]
        ]

        + SVerticalBox::Slot().AutoHeight()
        [ SNew(STextBlock).Text_Lambda([this] { return RefreshStatus; }).AutoWrapText(true) ]

        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        .Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))
        [
            MakeOrganisationBrowser()
        ]
    ];

    LoadMods();
    IPluginManager::Get().OnNewPluginCreated().AddSP(
        this,
        &SCrimeFactoryModEntryList::OnNewPluginCreated);
}

bool DoesPluginHaveRuntime(const IPlugin& Plugin) {
    //Content plugins always have runtime component
    if (Plugin.GetDescriptor().bCanContainContent) {
        return true;
    }
    //C++ plugins have runtime component as long as one of their modules does
    for (const FModuleDescriptor& Module : Plugin.GetDescriptor().Modules) {
        if (Module.Type == EHostType::Runtime ||
            Module.Type == EHostType::RuntimeNoCommandlet ||
            Module.Type == EHostType::RuntimeAndProgram ||
            Module.Type == EHostType::ClientOnly ||
            Module.Type == EHostType::ClientOnlyNoCommandlet ||
            Module.Type == EHostType::ServerOnly ||
            Module.Type == EHostType::CookedOnly) {
            return true;
        }
    }
    return false;
}

void SCrimeFactoryModEntryList::LoadMods() {
    if (bLoadingMods) return;
    TGuardValue<bool> Guard(bLoadingMods, true);
    int32 LoadErrors = 0;
    Mods.Empty();

    const FString ProjectModsDir =
        FPaths::ConvertRelativePathToFull(
            FPaths::Combine(
                FPaths::ProjectDir(),
                TEXT("Mods")));

    /*
     * /Mods is a custom CrimeFactory plugin root rather than Unreal's normal
     * /Plugins directory. Make it an active IPluginManager search path for the
     * running editor before doing any discovery.
     */
    IPluginManager::Get().AddPluginSearchPath(
        ProjectModsDir,
        false);

    /*
     * A newly-created legacy mod can exist on disk before Unreal considers it
     * "enabled" in the current session. Explicitly register each descriptor
     * found under /Mods; AddToPluginsList() is specifically intended for
     * adding one plugin without relying on a full discovery pass.
     */
    TArray<FString> ModDescriptorFiles;
    IFileManager::Get().FindFilesRecursive(
        ModDescriptorFiles,
        *ProjectModsDir,
        TEXT("*.uplugin"),
        true,
        false,
        false);

    for (const FString& DescriptorFile : ModDescriptorFiles)
    {
        FText AddFailure;
        if (!IPluginManager::Get().AddToPluginsList(
                DescriptorFile,
                &AddFailure))
        {
            ++LoadErrors;
            UE_LOG(
                LogCrimeFactory,
                Warning,
                TEXT(
                    "CrimeFactory could not register mod descriptor '%s': %s"
                ),
                *DescriptorFile,
                *AddFailure.ToString());
        }
    }

    /*
     * Do not use GetEnabledPlugins() for the CrimeFactory browser. A just-created
     * plugin can be fully discovered and mounted but not yet reflected in the
     * enabled-plugin cache until project state is flushed/reloaded.
     *
     * We only render entries physically under <Project>/Mods anyway, so
     * GetDiscoveredPlugins() is the correct source for this UI.
     */
    TArray<TSharedRef<IPlugin>> CandidatePlugins =
        IPluginManager::Get().GetDiscoveredPlugins();

    for (TSharedRef<IPlugin> Plugin : CandidatePlugins) {
        /*
         * Remove deleted mods from the CrimeFactory list immediately, even if
         * IPluginManager still has a stale in-memory entry for a frame.
         */
        FString PluginBaseDir =
            FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());

        FString NormalizedProjectModsDir = ProjectModsDir;
        FPaths::NormalizeDirectoryName(PluginBaseDir);
        FPaths::NormalizeDirectoryName(NormalizedProjectModsDir);

        const FString ProjectModsPrefix =
            NormalizedProjectModsDir.EndsWith(TEXT("/"))
                ? NormalizedProjectModsDir
                : NormalizedProjectModsDir + TEXT("/");

        const bool bLivesInProjectMods =
            PluginBaseDir.StartsWith(
                ProjectModsPrefix,
                ESearchCase::IgnoreCase);

        if (bLivesInProjectMods &&
            !IFileManager::Get().FileExists(*Plugin->GetDescriptorFileName()))
        {
            continue;
        }

        //Only include project plugins for now
        //TODO make sure UAT task supports engine plugins
        if (bLivesInProjectMods ||
            (bShowEngine && Plugin->GetType() == EPluginType::Engine) ||
            (bShowProject && Plugin->GetType() == EPluginType::Project) ||
            Plugin->GetType() == EPluginType::Mod) {
            
            const bool bHasRuntime = DoesPluginHaveRuntime(Plugin.Get());
            if (bHasRuntime) {
                if (bLivesInProjectMods)
                {
                    FString Error;
                    if (!CrimeFactoryMetadataSync::ReloadDescriptor(*Plugin, Error))
                    {
                        ++LoadErrors;
                        UE_LOG(LogCrimeFactory, Error, TEXT("Cannot refresh %s: %s"), *Plugin->GetName(), *Error);
                    }
                }
                if (bLivesInProjectMods && !FPluginUtils::FindLoadedPlugin(Plugin->GetDescriptorFileName()).IsValid() && Plugin->GetDescriptor().Modules.IsEmpty())
                {
                    FText Failure;
                    FPluginUtils::FLoadPluginParams Params;
                    Params.bEnablePluginInProject = true;
                    Params.bUpdateProjectPluginSearchPath = true;
                    Params.bSynchronousAssetsScan = true;
                    Params.OutFailReason = &Failure;
                    if (!Plugin->GetDescriptor().bExplicitlyLoaded)
                    {
                        // LoadPlugin only accepts explicitly-loaded descriptors. Normal content
                        // mods copied into Mods use the same native mount path as newly created mods.
                        IPluginManager::Get().MountNewlyCreatedPlugin(Plugin->GetName());
                        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
                            .ScanPathsSynchronous({Plugin->GetMountedAssetPath()}, true);
                    }
                    else if (!FPluginUtils::LoadPlugin(Plugin->GetDescriptorFileName(), Params))
                    {
                        ++LoadErrors;
                        UE_LOG(LogCrimeFactory, Warning, TEXT("Cannot load mod %s: %s"), *Plugin->GetName(), *Failure.ToString());
                    }
                }
                if (bLivesInProjectMods && FPluginUtils::FindLoadedPlugin(Plugin->GetDescriptorFileName()).IsValid())
                {
                    FString Error;
                    if (!CrimeFactoryMetadataSync::Synchronize(*Plugin, false, Error))
                    {
                        ++LoadErrors;
                        UE_LOG(LogCrimeFactory, Error, TEXT("%s: %s"), *Plugin->GetName(), *Error);
                    }
                }
                Mods.Add(Plugin);
            }
        }
    }
    Mods.Sort([](const TSharedRef<IPlugin> Plugin1, const TSharedRef<IPlugin> Plugin2) {
        return Plugin1->GetName() < Plugin2->GetName();
    });
    RefreshStatus = LoadErrors ? FText::Format(LOCTEXT("RefreshProblems", "{0} mod(s). {1} refresh problem(s); see Output Log. New code plugins may require an editor restart."), Mods.Num(), LoadErrors)
        : FText::Format(LOCTEXT("RefreshCount", "{0} mod(s) discovered."), Mods.Num());
    TMap<FString, FString> Descriptors;
    for (const auto& Mod : Mods) Descriptors.Add(FCrimeFactoryModOrganisation::ModKey(*Mod), Mod->GetDescriptorFileName());
    if (Organisation->ReconcileDescriptors(Descriptors)) SaveOrganisation();
    CachedEntries.Reset();
    Filter(LastFilter);
}

FReply SCrimeFactoryModEntryList::RefreshMods()
{
    // AddToPluginsList discovers exact new descriptors without replacing live IPlugin instances.
    LoadMods();
    return FReply::Handled();
}

bool SCrimeFactoryModEntryList::ContainsMod(const FString& Name) const
{
    return Mods.ContainsByPredicate([&Name](const auto& Plugin) { return Plugin->GetName() == Name; });
}

bool PluginMatchesSearchTokens(const IPlugin& Plugin, const TArray<FString>& Tokens) {
    const FString PluginName = Plugin.GetName();
    const FString FriendlyName = Plugin.GetDescriptor().FriendlyName;
    const FString Description = Plugin.GetDescriptor().Description;

    for (const FString& Token : Tokens) {
        if (PluginName.Contains(Token) ||
            FriendlyName.Contains(Token) ||
            Description.Contains(Token)) {
            return true;
        }
    }
    return false;
}

void SCrimeFactoryModEntryList::Filter(const FString& InFilter) {
    LastFilter = InFilter;
    FilteredMods.Empty();

    if (InFilter.TrimStartAndEnd().IsEmpty()) {
        FilteredMods = Mods;
    } else {
        // tokenize filter keywords
        TArray<FString> FilterTokens;
        InFilter.ParseIntoArray(FilterTokens, TEXT(" "), true);

        // check each mod for it to contain the tokens
        for (TSharedRef<IPlugin> Mod : Mods) {
            // check each token in mod name
            if (PluginMatchesSearchTokens(Mod.Get(), FilterTokens)) {
                FilteredMods.Add(Mod);
            }
        }
    }

    RefreshFolderContents();
}

void SCrimeFactoryModEntryList::RebuildModGrid()
{
    VisibleEntries.Reset();

    if (!ModGrid.IsValid())
    {
        return;
    }

    ModGrid->ClearChildren();
    const bool bSearching = !LastFilter.TrimStartAndEnd().IsEmpty();
    TSet<FString> FoldersWithMatches;
    if (bSearching)
    {
        // Mark ancestors once per matching mod instead of scanning every mod for every tile.
        for (const auto& Mod : FilteredMods)
        {
            FString Folder = Organisation->FolderForMod(FCrimeFactoryModOrganisation::ModKey(*Mod));
            while (const auto* Node = Organisation->Find(Folder))
            {
                if (FoldersWithMatches.Contains(Folder)) break;
                FoldersWithMatches.Add(Folder);
                Folder = Node->Parent;
            }
        }
    }

    // Folder tiles share the same drop handling as the sidebar rows.
    const FString Parent = Organisation->CurrentFolder == FCrimeFactoryModOrganisation::AllMods
        ? FString() : Organisation->CurrentFolder;
    for (const auto& Folder : Organisation->Children(Parent))
    {
        const bool bMatchesSearch = !bSearching || Folder.Name.Contains(LastFilter) || FoldersWithMatches.Contains(Folder.Id);
        if (bMatchesSearch) AddFolderTile(Folder.Id);
    }

    for (const TSharedRef<IPlugin>& Mod : FilteredMods)
    {
        const FString Key = FCrimeFactoryModOrganisation::ModKey(*Mod);
        const FString Assigned = Organisation->FolderForMod(Key);
        if (Organisation->CurrentFolder != FCrimeFactoryModOrganisation::AllMods &&
            !(bSearching ? Organisation->IsWithin(Assigned, Organisation->CurrentFolder) : Assigned == Organisation->CurrentFolder))
        {
            continue;
        }
        TSharedPtr<SCrimeFactoryModEntry>& Cached = CachedEntries.FindOrAdd(Key);
        if (!Cached.IsValid()) Cached = SNew(SCrimeFactoryModEntry, Mod, SharedThis(this));
        TSharedRef<SCrimeFactoryModEntry> Entry = Cached.ToSharedRef();

        VisibleEntries.Add(Entry);

        ModGrid->AddSlot()
        .Padding(FMargin(4.0f))
        [
            Entry
        ];
    }
    if (ModGrid->GetChildren()->Num() == 0)
    {
        ModGrid->AddSlot().Padding(8)
        [ SNew(STextBlock).Text(bSearching ? LOCTEXT("NoFolderMatches", "No mods match this search in the selected folder.")
            : LOCTEXT("EmptyVirtualFolder", "This virtual folder is empty.")).AutoWrapText(true) ];
    }
}

FString SCrimeFactoryModEntryList::GetLastFilter() const {
    return LastFilter;
}

void SCrimeFactoryModEntryList::SetShowEngine(bool bInShowEngine) {
    bShowEngine = bInShowEngine;
    LoadMods();
}

void SCrimeFactoryModEntryList::SetShowProject(bool bInShowProject) {
    bShowProject = bInShowProject;
    LoadMods();
}

void SCrimeFactoryModEntryList::OnNewPluginCreated(IPlugin& Plugin)
{
    LoadMods();
}

FReply SCrimeFactoryModEntryList::PackageAllMods() {
    TSharedPtr<SCrimeFactoryModEntry> First;
    TArray<TSharedPtr<SCrimeFactoryModEntry>> NextEntries;

    UE_LOG(LogCrimeFactory, Display, TEXT("Forge Selected!"));

    for (const TSharedPtr<SCrimeFactoryModEntry>& ModEntry : VisibleEntries) {
        if (!ModEntry.IsValid() || !ModEntry->IsSelected()) {
            continue;
        }

        if (!First.IsValid()) {
            First = ModEntry;
        } else {
            NextEntries.Add(ModEntry);
        }
    }

    if (First.IsValid()) {
        First->PackageMod(NextEntries);
    }

    return FReply::Handled();
}

FReply SCrimeFactoryModEntryList::CheckAllMods() {
    for (const TSharedPtr<SCrimeFactoryModEntry>& ModEntry : VisibleEntries) {
        if (ModEntry.IsValid()) {
            ModEntry->SetSelected(true);
        }
    }

    return FReply::Handled();
}

FReply SCrimeFactoryModEntryList::UncheckAllMods() {
    for (const TSharedPtr<SCrimeFactoryModEntry>& ModEntry : VisibleEntries) {
        if (ModEntry.IsValid()) {
            ModEntry->SetSelected(false);
        }
    }

    return FReply::Handled();
}

FReply SCrimeFactoryModEntryList::ImportLegacyMod()
{
    FString ImportedModName;
    FString ImportReport;

    if (FLegacyModImporter::ImportFromFolderDialog(
            ImportedModName,
            ImportReport))
    {
        /*
         * FPluginUtils::CreateAndLoadNewPlugin() gives the importer a valid
         * plugin immediately, but IPluginManager::GetEnabledPlugins() can still
         * be one refresh/frame behind when the modal migration dialog returns.
         *
         * Force PluginManager discovery first, then rebuild the CrimeFactory rows.
         */
        const FString ProjectModsDir =
            FPaths::ConvertRelativePathToFull(
                FPaths::Combine(
                    FPaths::ProjectDir(),
                    TEXT("Mods")));

        IPluginManager::Get().AddPluginSearchPath(
            ProjectModsDir,
            true);

        LoadMods();

        /*
         * Do a second refresh on the next game-thread task as well. This covers
         * the UE5.5 case where the plugin descriptor/search-path update is
         * finalized after the import call unwinds.
         */
        TWeakPtr<SCrimeFactoryModEntryList> WeakEntryList =
            SharedThis(this);

        AsyncTask(
            ENamedThreads::GameThread,
            [WeakEntryList, ImportedModName]()
            {
                const TSharedPtr<SCrimeFactoryModEntryList> PinnedThis =
                    WeakEntryList.Pin();

                if (!PinnedThis.IsValid())
                {
                    return;
                }

                const FString ProjectModsDir =
                    FPaths::ConvertRelativePathToFull(
                        FPaths::Combine(
                            FPaths::ProjectDir(),
                            TEXT("Mods")));

                IPluginManager::Get().AddPluginSearchPath(
                    ProjectModsDir,
                    true);

                PinnedThis->LoadMods();

                UE_LOG(
                    LogCrimeFactory,
                    Display,
                    TEXT(
                        "Legacy import UI refresh complete for mod '%s'."
                    ),
                    *ImportedModName);
            });
    }

    return FReply::Handled();
}

FReply SCrimeFactoryModEntryList::CreateMod()
{
    FGlobalTabmanager::Get()->TryInvokeTab(FCrimeFactoryModule::ModCreatorTabName);
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
