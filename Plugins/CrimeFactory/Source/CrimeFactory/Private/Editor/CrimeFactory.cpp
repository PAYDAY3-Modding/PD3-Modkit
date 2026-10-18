#include "Editor/CrimeFactory.h"
#include "Editor/CrimeFactoryStyle.h"
#include "Editor/CrimeFactoryCommands.h"
#include "Settings/CrimeFactorySettings.h"
#include "ModBrowser/CrimeFactoryWidget.h"
#include "AssetPalette/CrimeFactoryAssetPalette.h"
#include "Updater/PD3ModkitUpdater.h"
#include "Metadata/CrimeFactoryMetadataSync.h"
#include "ContentBrowserModule.h"
#include "ContentBrowser/CrimeFactoryCookStateFilter.h"
#include "ISettingsModule.h"
#include "ISettingsContainer.h"
#include "ISettingsSection.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "IPluginBrowser.h"
#include "ModCreation/ModWizardDefinition.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManager.h"
#include "Engine/AssetManagerSettings.h"
#include "Engine/Engine.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#if __has_include("PD3CookedEditorSettings.h")
#include "PD3CookedEditorSettings.h"
#endif

static const FName CrimeFactoryTabName("CrimeFactory");

#define LOCTEXT_NAMESPACE "FCrimeFactoryModule"

DEFINE_LOG_CATEGORY(LogCrimeFactory)

const FName FCrimeFactoryModule::ModCreatorTabName(TEXT("ModCreator"));

void FCrimeFactoryModule::StartupModule() {
    // Native Content Browser filter discovery enumerates extension class defaults.
    GetDefault<UCrimeFactoryCookStateFilterExtension>();
    CrimeFactoryMetadataSync::Initialize();
#if __has_include("PD3CookedEditorSettings.h")
    CookedEditorSettingsChangedHandle = GetMutableDefault<UPD3CookedEditorSettings>()->OnSettingChanged().AddStatic(
        &UCrimeFactorySettings::OnCookedEditorSettingsChanged);
#endif
    //Register editor settings
    UCrimeFactorySettings::Get();
    RegisterSettings();

    //Register mod templates
    RegisterModTemplates();

    //Initialize Slate stuff, including commands
    FCrimeFactoryStyle::Initialize();
    FCrimeFactoryStyle::ReloadTextures();
    FCrimeFactoryCommands::Register();
    
    PluginCommands = MakeShareable(new FUICommandList);
    PluginCommands->MapAction(
        FCrimeFactoryCommands::Get().OpenPluginWindow,
        FExecuteAction::CreateLambda([](){
            FGlobalTabmanager::Get()->TryInvokeTab(CrimeFactoryTabName);
        }),
        FCanExecuteAction());
    PluginCommands->MapAction(
        FCrimeFactoryCommands::Get().OpenAssetPalette,
        FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(CrimeFactoryAssetPalette::TabName); }),
        FCanExecuteAction());
    CrimeFactoryAssetPalette::Register();
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));

    // Start the delayed project update check. This compares the local Git checkout
    // against the official PD3-Modkit remote without modifying the working tree.
    FPD3ModkitUpdater::Initialize();

    
    // Register CrimeFactory in the editor's Window menu instead of File.
    {
        FToolMenuOwnerScoped OwnerScoped(this);

        UToolMenu* WindowMenu =
            UToolMenus::Get()->ExtendMenu(
                TEXT("LevelEditor.MainMenu.Window"));

        if (WindowMenu)
        {
            FToolMenuSection& Section =
                WindowMenu->FindOrAddSection(
                    TEXT("WindowLayout"));

            Section.AddSubMenu("CrimeFactory", LOCTEXT("ToolsMenu", "CrimeFactory"),
                LOCTEXT("ToolsMenuTooltip", "CrimeFactory tools"),
                FNewToolMenuDelegate::CreateLambda([Commands = PluginCommands](UToolMenu* Menu)
                {
                    FToolMenuSection& Tools = Menu->AddSection("CrimeFactoryTools");
                    Tools.AddMenuEntryWithCommandList(FCrimeFactoryCommands::Get().OpenPluginWindow, Commands);
                    Tools.AddMenuEntryWithCommandList(FCrimeFactoryCommands::Get().OpenAssetPalette, Commands);
                }));
        }
    }

    // Add a manual updater action to the editor Help menu.
    {
        FToolMenuOwnerScoped OwnerScoped(this);

        UToolMenu* HelpMenu = UToolMenus::Get()->ExtendMenu(
            TEXT("LevelEditor.MainMenu.Help"));

        if (HelpMenu)
        {
            FToolMenuSection& Section = HelpMenu->FindOrAddSection(
                TEXT("PD3Modkit"),
                LOCTEXT("PD3ModkitHelpSection", "PD3 MODKIT"));

            Section.AddMenuEntry(
                TEXT("PD3Modkit_CheckForUpdates"),
                LOCTEXT("PD3ModkitCheckForUpdates", "Check for Modkit Updates"),
                LOCTEXT("PD3ModkitCheckForUpdatesTooltip", "Check for PD3 Modkit and compatible UE5-PD3 engine updates."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FPD3ModkitUpdater::CheckForUpdates(true);
                })));
        }
    }

    //Register CrimeFactory Settings in Editor's Toolbar
    ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(TEXT("Play"), EExtensionHook::After, PluginCommands,
        FToolBarExtensionDelegate::CreateLambda([](FToolBarBuilder& Builder) {
            Builder.AddToolBarButton(FCrimeFactoryCommands::Get().OpenPluginWindow);
            Builder.AddToolBarButton(FCrimeFactoryCommands::Get().OpenAssetPalette, NAME_None, TAttribute<FText>(),
                TAttribute<FText>(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.PlaceActors"));
        }));
    LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

    //Register normal crimefactory settings tab spawner
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(CrimeFactoryTabName,
        FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&){
            const TSharedRef<SCrimeFactoryWidget> CrimeFactoryWidget = SNew(SCrimeFactoryWidget);
            return SNew(SDockTab)
                .TabRole(NomadTab)
                // TODO: Save settings when tab is closed automatically
                //.OnTabClosed_Lambda([CrimeFactoryWidget](TSharedRef<SDockTab>) { CrimeFactoryWidget->SaveSettings(); })
                [ CrimeFactoryWidget ];
        }))
        .SetDisplayName(LOCTEXT("FCrimeFactoryTabTitle", "CrimeFactory"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ModCreatorTabName,
        FOnSpawnTab::CreateRaw(this, &FCrimeFactoryModule::HandleSpawnModCreatorTab))
        .SetDisplayName(LOCTEXT("NewPluginTabHeader", "New Mod"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FCrimeFactoryModule::ShutdownModule() {
    IPluginManager::Get().OnNewPluginMounted().Remove(PluginMountedHandle);
    IPluginManager::Get().OnNewPluginContentMounted().Remove(PluginContentMountedHandle);
    CrimeFactoryMetadataSync::Shutdown();
    CrimeFactoryAssetPalette::Unregister();
    if (ToolbarExtender.IsValid())
    {
        if (auto* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
            LevelEditor->GetToolBarExtensibilityManager()->RemoveExtender(ToolbarExtender);
        ToolbarExtender.Reset();
    }
#if __has_include("PD3CookedEditorSettings.h")
    if (UObjectInitialized())
    {
        GetMutableDefault<UPD3CookedEditorSettings>()->OnSettingChanged().Remove(CookedEditorSettingsChangedHandle);
    }
#endif
    FPD3ModkitUpdater::Shutdown();

    if (UObjectInitialized()) {
        UnregisterSettings();
    }
    
    UToolMenus::UnregisterOwner(this);

    FCrimeFactoryStyle::Shutdown();
    FCrimeFactoryCommands::Unregister();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CrimeFactoryTabName);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ModCreatorTabName);
    PluginCommands.Reset();
}

TSharedRef<SDockTab> FCrimeFactoryModule::HandleSpawnModCreatorTab(const FSpawnTabArgs& SpawnTabArgs)
{
    IPluginBrowser& PluginBrowser = FModuleManager::Get().GetModuleChecked<IPluginBrowser>(TEXT("PluginBrowser"));
    return PluginBrowser.SpawnPluginCreatorTab(SpawnTabArgs, MakeShared<FModWizardDefinition>());
}

void FCrimeFactoryModule::RegisterSettings() const {
    ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings"));
    // Both settings pages share the CrimeFactory category. Remove the former Plugins entry.
    SettingsModule.UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("CrimeFactory"));
    if (auto Container = SettingsModule.GetContainer(TEXT("Project")))
        Container->DescribeCategory(TEXT("CrimeFactory"), LOCTEXT("CrimeFactoryCategory", "CrimeFactory"),
            LOCTEXT("CrimeFactoryCategoryDescription", "CrimeFactory mod packaging and level authoring tools."));

    ISettingsSectionPtr SettingsSection = SettingsModule.RegisterSettings(
        TEXT("Project"), TEXT("CrimeFactory"), TEXT("Settings"),
        LOCTEXT("CrimeFactorySettingsName", "General"),
        LOCTEXT("CrimeFactorySettingsDescription", "CrimeFactory settings for PAYDAY 3 modding and packaging."),
        UCrimeFactorySettings::Get()
    );

    // Persist changes made through Project Settings.
    if (SettingsSection.IsValid()) {
        SettingsSection->OnModified().BindLambda([](){
            UCrimeFactorySettings::Get()->SaveSettings();
            return true;
        });
    }
}

void FCrimeFactoryModule::UnregisterSettings() const {
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"))) {
        SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("CrimeFactory"));
        // General page registered alongside Asset Palette in the CrimeFactory category.
        SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("CrimeFactory"), TEXT("Settings"));
    }
}

bool IsPluginAMod(IPlugin& Plugin) {
    //Mod plugins are always considered mods
    if (Plugin.GetType() == EPluginType::Mod) {
        return true;
    }
    //Project plugins are considered mods too, for now
    if (Plugin.GetType() == EPluginType::Project) {
        return true;
    }
    return false;
}

bool FCrimeFactoryModule::RegisterPrimaryAssetLabelsFromPlugin(IPlugin& Plugin)
{
    if (!Plugin.CanContainContent())
    {
        return false;
    }

    // Only actual Mod plugins belong in automatic PAL scan directories.
    if (Plugin.GetType() != EPluginType::Mod)
    {
        return false;
    }

    UAssetManagerSettings* AssetManagerSettings =
        GetMutableDefault<UAssetManagerSettings>();

    if (!AssetManagerSettings)
    {
        UE_LOG(LogCrimeFactory, Error,
            TEXT("Unable to get UAssetManagerSettings while registering PALs for %s"),
            *Plugin.GetName());
        return false;
    }

    FPrimaryAssetTypeInfo* PrimaryAssetLabelType =
        AssetManagerSettings->PrimaryAssetTypesToScan.FindByPredicate(
            [](const FPrimaryAssetTypeInfo& TypeInfo)
            {
                return TypeInfo.PrimaryAssetType ==
                    FName(TEXT("PrimaryAssetLabel"));
            });

    if (!PrimaryAssetLabelType)
    {
        UE_LOG(LogCrimeFactory, Error,
            TEXT("Asset Manager has no PrimaryAssetLabel entry in Primary Asset Types to Scan. "
                 "Create that entry once; CrimeFactory will manage Directories and Specific Assets automatically."));
        return false;
    }

    FString PluginMountPath = Plugin.GetMountedAssetPath();
    PluginMountPath.RemoveFromEnd(TEXT("/"));

    if (PluginMountPath.IsEmpty())
    {
        UE_LOG(LogCrimeFactory, Warning,
            TEXT("Plugin %s has no mounted asset path; PAL registration skipped."),
            *Plugin.GetName());
        return false;
    }

    const FPrimaryAssetTypeInfo& CurrentTypeInfo = *PrimaryAssetLabelType;
    TArray<FDirectoryPath> Directories = CurrentTypeInfo.GetDirectories();
    TArray<FSoftObjectPath> SpecificAssets = CurrentTypeInfo.GetSpecificAssets();

    bool bChanged = false;

    // Make /<ModName> visible in Project Settings -> Asset Manager -> Directories.
    const bool bDirectoryAlreadyRegistered =
        Directories.ContainsByPredicate(
            [&PluginMountPath](const FDirectoryPath& ExistingDirectory)
            {
                return ExistingDirectory.Path.Equals(
                    PluginMountPath,
                    ESearchCase::IgnoreCase);
            });

    if (!bDirectoryAlreadyRegistered)
    {
        FDirectoryPath NewDirectory;
        NewDirectory.Path = PluginMountPath;
        Directories.Add(NewDirectory);
        bChanged = true;

        UE_LOG(LogCrimeFactory, Display,
            TEXT("Asset Manager: registered mod directory for PrimaryAssetLabel scan: %s"),
            *PluginMountPath);
    }

    // Force discovery of PALs under the mod mount when the mount is usable.
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    TArray<FString> PathsToScan;
    PathsToScan.Add(PluginMountPath);
    AssetRegistry.ScanPathsSynchronous(PathsToScan, true);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*PluginMountPath));
    Filter.ClassPaths.Add(UPrimaryAssetLabel::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> PrimaryAssetLabels;
    AssetRegistry.GetAssets(Filter, PrimaryAssetLabels);

    for (const FAssetData& AssetData : PrimaryAssetLabels)
    {
        const FSoftObjectPath PALPath = AssetData.GetSoftObjectPath();

        if (!SpecificAssets.Contains(PALPath))
        {
            SpecificAssets.Add(PALPath);
            bChanged = true;

            UE_LOG(LogCrimeFactory, Display,
                TEXT("Asset Manager: registered mod PAL as a Specific Asset: %s"),
                *PALPath.ToString());
        }
    }

    if (bChanged)
    {
        UClass* AssetBaseClass =
            CurrentTypeInfo.GetAssetBaseClass().LoadSynchronous();

        if (!AssetBaseClass)
        {
            UE_LOG(LogCrimeFactory, Error,
                TEXT("Could not load Asset Base Class for PrimaryAssetLabel settings; PAL registration was not saved."));
            return false;
        }

        FPrimaryAssetTypeInfo UpdatedTypeInfo(
            CurrentTypeInfo.PrimaryAssetType,
            AssetBaseClass,
            CurrentTypeInfo.bHasBlueprintClasses,
            CurrentTypeInfo.bIsEditorOnly,
            MoveTemp(Directories),
            MoveTemp(SpecificAssets));

        UpdatedTypeInfo.Rules = CurrentTypeInfo.Rules;
        *PrimaryAssetLabelType = MoveTemp(UpdatedTypeInfo);
    }

    const FString ConventionalPALPackage = FString::Printf(
        TEXT("%s/PAL_%s"),
        *PluginMountPath,
        *Plugin.GetName());

    const bool bFoundConventionalPAL = PrimaryAssetLabels.ContainsByPredicate(
        [&ConventionalPALPackage](const FAssetData& AssetData)
        {
            return AssetData.PackageName.ToString().Equals(
                ConventionalPALPackage,
                ESearchCase::IgnoreCase);
        });

    if (!bFoundConventionalPAL)
    {
        UE_LOG(LogCrimeFactory, Verbose,
            TEXT("No conventional PAL found for %s at %s on this scan. "
                 "The directory is still registered; newly-mounted mods get a deferred rescan."),
            *Plugin.GetName(),
            *ConventionalPALPackage);
    }

    return bChanged;
}

void FCrimeFactoryModule::SaveAndRefreshAssetManagerSettings()
{
    UAssetManagerSettings* AssetManagerSettings =
        GetMutableDefault<UAssetManagerSettings>();

    if (!AssetManagerSettings)
    {
        return;
    }

    // Persist to the project's source DefaultGame.ini, not only the generated
    // editor Game.ini. This is important because PASS 2 runs in a separate
    // cook commandlet process and must see the same Specific Assets list.
    const FString DefaultGameIni = FPaths::Combine(
        FPaths::ProjectConfigDir(),
        TEXT("DefaultGame.ini"));

    AssetManagerSettings->SaveConfig(CPF_Config, *DefaultGameIni);

    if (GConfig)
    {
        GConfig->Flush(false, DefaultGameIni);
    }

#if WITH_EDITOR
    /*
     * CrimeFactory can reach this function from StartupModule(), before GEngine and
     * UAssetManager have finished initializing. Calling ReinitializeFromConfig()
     * that early crashes inside Engine.
     *
     * The config/CDO has already been updated above, so during early startup the
     * normal Asset Manager initialization will consume the new settings. For
     * mods mounted later in an already-running editor, refresh immediately.
     */
    if (GEngine != nullptr && UAssetManager::IsInitialized())
    {
        UAssetManager::Get().ReinitializeFromConfig();

        UE_LOG(
            LogCrimeFactory,
            Verbose,
            TEXT("Asset Manager already initialized; refreshed Primary Asset configuration.")
        );
    }
    else
    {
        UE_LOG(
            LogCrimeFactory,
            Display,
            TEXT("Asset Manager is not initialized yet; PAL configuration was saved and will be picked up during normal initialization.")
        );
    }
#endif

    UE_LOG(LogCrimeFactory, Display,
        TEXT("Asset Manager PAL registration saved to %s"),
        *DefaultGameIni);
}

void FCrimeFactoryModule::EnsureAssetManagerRegistrationForPlugin(IPlugin& Plugin)
{
    if (RegisterPrimaryAssetLabelsFromPlugin(Plugin))
    {
        SaveAndRefreshAssetManagerSettings();
    }
}

void FCrimeFactoryModule::RemoveAssetManagerRegistrationForPlugin(
    const FString& PluginName,
    const FString& PluginMountPath)
{
    UAssetManagerSettings* AssetManagerSettings =
        GetMutableDefault<UAssetManagerSettings>();

    if (!AssetManagerSettings)
    {
        UE_LOG(
            LogCrimeFactory,
            Error,
            TEXT("Unable to get UAssetManagerSettings while removing mod %s"),
            *PluginName);
        return;
    }

    FPrimaryAssetTypeInfo* PrimaryAssetLabelType =
        AssetManagerSettings->PrimaryAssetTypesToScan.FindByPredicate(
            [](const FPrimaryAssetTypeInfo& TypeInfo)
            {
                return TypeInfo.PrimaryAssetType ==
                    FName(TEXT("PrimaryAssetLabel"));
            });

    if (!PrimaryAssetLabelType)
    {
        UE_LOG(
            LogCrimeFactory,
            Warning,
            TEXT("Asset Manager has no PrimaryAssetLabel entry while removing mod %s"),
            *PluginName);
        return;
    }

    FString NormalizedMountPath = PluginMountPath;
    NormalizedMountPath.RemoveFromEnd(TEXT("/"));

    if (NormalizedMountPath.IsEmpty())
    {
        NormalizedMountPath =
            FString::Printf(TEXT("/%s"), *PluginName);
    }

    const FPrimaryAssetTypeInfo& CurrentTypeInfo = *PrimaryAssetLabelType;

    TArray<FDirectoryPath> Directories =
        CurrentTypeInfo.GetDirectories();

    TArray<FSoftObjectPath> SpecificAssets =
        CurrentTypeInfo.GetSpecificAssets();

    const int32 RemovedDirectoryCount =
        Directories.RemoveAll(
            [&NormalizedMountPath](const FDirectoryPath& Directory)
            {
                return Directory.Path.Equals(
                    NormalizedMountPath,
                    ESearchCase::IgnoreCase);
            });

    const FString MountPrefix = NormalizedMountPath + TEXT("/");

    const int32 RemovedSpecificAssetCount =
        SpecificAssets.RemoveAll(
            [&NormalizedMountPath, &MountPrefix](
                const FSoftObjectPath& AssetPath)
            {
                if (AssetPath.IsNull())
                {
                    return false;
                }

                const FString PathString = AssetPath.ToString();

                return PathString.Equals(
                           NormalizedMountPath,
                           ESearchCase::IgnoreCase) ||
                       PathString.StartsWith(
                           MountPrefix,
                           ESearchCase::IgnoreCase);
            });

    if (RemovedDirectoryCount == 0 &&
        RemovedSpecificAssetCount == 0)
    {
        UE_LOG(
            LogCrimeFactory,
            Verbose,
            TEXT("No Asset Manager registration found for deleted mod %s"),
            *PluginName);
        return;
    }

    UClass* AssetBaseClass =
        CurrentTypeInfo.GetAssetBaseClass().LoadSynchronous();

    if (!AssetBaseClass)
    {
        UE_LOG(
            LogCrimeFactory,
            Error,
            TEXT("Could not load Asset Base Class while removing Asset Manager registration for %s"),
            *PluginName);
        return;
    }

    FPrimaryAssetTypeInfo UpdatedTypeInfo(
        CurrentTypeInfo.PrimaryAssetType,
        AssetBaseClass,
        CurrentTypeInfo.bHasBlueprintClasses,
        CurrentTypeInfo.bIsEditorOnly,
        MoveTemp(Directories),
        MoveTemp(SpecificAssets));

    UpdatedTypeInfo.Rules = CurrentTypeInfo.Rules;
    *PrimaryAssetLabelType = MoveTemp(UpdatedTypeInfo);

    SaveAndRefreshAssetManagerSettings();

    UE_LOG(
        LogCrimeFactory,
        Display,
        TEXT(
            "Removed Asset Manager registration for mod %s "
            "(Directories removed: %d, Specific Assets removed: %d)"
        ),
        *PluginName,
        RemovedDirectoryCount,
        RemovedSpecificAssetCount);
}

void FCrimeFactoryModule::RegisterModTemplates()
{
    bool bAssetManagerChanged = false;

    TArray<TSharedRef<IPlugin>> EnabledPlugins =
        IPluginManager::Get().GetEnabledPlugins();

    for (TSharedRef<IPlugin> Plugin : EnabledPlugins)
    {
        if (Plugin->GetType() == EPluginType::Mod)
        {
            bAssetManagerChanged |=
                RegisterPrimaryAssetLabelsFromPlugin(Plugin.Get());
        }

        // Preserve existing support for project plugins providing templates.
        if (IsPluginAMod(Plugin.Get()))
        {
            AddModTemplatesFromPlugin(Plugin.Get());
        }
    }

    if (bAssetManagerChanged)
    {
        SaveAndRefreshAssetManagerSettings();
    }

    PluginMountedHandle = IPluginManager::Get().OnNewPluginMounted().AddLambda(
        [this](IPlugin& Plugin)
        {
            if (IsPluginAMod(Plugin)) AddModTemplatesFromPlugin(Plugin);
        });
    PluginContentMountedHandle = IPluginManager::Get().OnNewPluginContentMounted().AddLambda(
        [this](IPlugin& Plugin)
        {
            // OnNewPluginMounted fires before RegisterMountPoint in UE5.5.
            // PAL discovery must wait until the content mount is actually available.
            if (Plugin.GetType() == EPluginType::Mod) EnsureAssetManagerRegistrationForPlugin(Plugin);
        });
}

void FCrimeFactoryModule::AddModTemplatesFromPlugin(IPlugin& Plugin) {
    FString TemplatesPath = Plugin.GetBaseDir() / TEXT("Templates");
    if (!FPaths::DirectoryExists(TemplatesPath)) {
        return;
    }
    FString TemplatesJSONPath = TemplatesPath / TEXT("templates.json");
    if (!FPaths::FileExists(TemplatesJSONPath)) {
        return;
    }
    FString TemplatesJSON;
    if (!FFileHelper::LoadFileToString(TemplatesJSON, *TemplatesJSONPath)) {
        return;
    }
    const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(TemplatesJSON);
    TSharedPtr<FJsonObject> TemplatesJSONObj;
    if (!FJsonSerializer::Deserialize(JsonReader, TemplatesJSONObj)) {
        UE_LOG(LogCrimeFactory, Error, TEXT("Invalid templates for mod %s: invalid json"), *Plugin.GetName());
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* TemplatesArrayPtr;
    if(!TemplatesJSONObj->TryGetArrayField(TEXT("templates"), TemplatesArrayPtr)) {
        UE_LOG(LogCrimeFactory, Error, TEXT("Invalid templates for mod %s: \"templates\" not in json"), *Plugin.GetName());
        return;
    }
    int i = 0;
    for(TSharedPtr<FJsonValue> Item : *TemplatesArrayPtr)
    {
        i++;
        const TSharedPtr<FJsonObject>* ItemObjPtr;
        if(!Item->TryGetObject(ItemObjPtr)) {
            UE_LOG(LogCrimeFactory, Error, TEXT("Invalid template for mod %s: template %d: not an object"), *Plugin.GetName(), i);
            continue;
        }

        FString Error;
        TSharedPtr<FModTemplateDescription> ModTemplate = FModTemplateDescription::Load(*ItemObjPtr, TemplatesPath, Error);
        if(!ModTemplate.IsValid())
        {
            UE_LOG(LogCrimeFactory, Error, TEXT("Invalid templates for mod %s: template %d: %s"), *Plugin.GetName(), i, *Error);
            continue;
        }
        ModTemplates.Add(ModTemplate.ToSharedRef());
    }    
}

#undef LOCTEXT_NAMESPACE
    
IMPLEMENT_MODULE(FCrimeFactoryModule, CrimeFactory)
