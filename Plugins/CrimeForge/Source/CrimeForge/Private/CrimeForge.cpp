#include "CrimeForge.h"
#include "CrimeForgeStyle.h"
#include "CrimeForgeCommands.h"
#include "CrimeForgeSettings.h"
#include "CrimeForgeWidget.h"
#include "ContentBrowserModule.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "IPluginBrowser.h"
#include "ModWizardDefinition.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/AssetManager.h"
#include "Engine/AssetManagerSettings.h"
#include "Engine/Engine.h"
#include "Engine/PrimaryAssetLabel.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

static const FName CrimeForgeTabName("CrimeForge");

#define LOCTEXT_NAMESPACE "FCrimeForgeModule"

DEFINE_LOG_CATEGORY(LogCrimeForge)

const FName FCrimeForgeModule::ModCreatorTabName(TEXT("ModCreator"));

void FCrimeForgeModule::StartupModule() {
    //Register editor settings
    RegisterSettings();

    //Register mod templates
    RegisterModTemplates();

    //Initialize Slate stuff, including commands
    FCrimeForgeStyle::Initialize();
    FCrimeForgeStyle::ReloadTextures();
    FCrimeForgeCommands::Register();
    
    PluginCommands = MakeShareable(new FUICommandList);
    PluginCommands->MapAction(
        FCrimeForgeCommands::Get().OpenPluginWindow,
        FExecuteAction::CreateLambda([](){
            FGlobalTabmanager::Get()->TryInvokeTab(CrimeForgeTabName);
        }),
        FCanExecuteAction());
    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
    
    // Register CrimeForge in the editor's Window menu instead of File.
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

            Section.AddMenuEntryWithCommandList(
                FCrimeForgeCommands::Get().OpenPluginWindow,
                PluginCommands);
        }
    }

    //Register CrimeForge Settings in Editor's Toolbar
    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(TEXT("Play"), EExtensionHook::After, PluginCommands,
        FToolBarExtensionDelegate::CreateLambda([](FToolBarBuilder& Builder) {
            Builder.AddToolBarButton(FCrimeForgeCommands::Get().OpenPluginWindow);
        }));
    LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);

    //Register normal crimeforge settings tab spawner
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(CrimeForgeTabName,
        FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&){
            const TSharedRef<SCrimeForgeWidget> CrimeForgeWidget = SNew(SCrimeForgeWidget);
            return SNew(SDockTab)
                .TabRole(NomadTab)
                // TODO: Save settings when tab is closed automatically
                //.OnTabClosed_Lambda([CrimeForgeWidget](TSharedRef<SDockTab>) { CrimeForgeWidget->SaveSettings(); })
                [ CrimeForgeWidget ];
        }))
        .SetDisplayName(LOCTEXT("FCrimeForgeTabTitle", "CrimeForge"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
    
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ModCreatorTabName,
        FOnSpawnTab::CreateRaw(this, &FCrimeForgeModule::HandleSpawnModCreatorTab))
        .SetDisplayName(LOCTEXT("NewPluginTabHeader", "New Mod"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FCrimeForgeModule::ShutdownModule() {
    if (UObjectInitialized()) {
        UnregisterSettings();
    }
    
    UToolMenus::UnregisterOwner(this);

    FCrimeForgeStyle::Shutdown();
    FCrimeForgeCommands::Unregister();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CrimeForgeTabName);
}

TSharedRef<SDockTab> FCrimeForgeModule::HandleSpawnModCreatorTab(const FSpawnTabArgs& SpawnTabArgs)
{
    IPluginBrowser& PluginBrowser = FModuleManager::Get().GetModuleChecked<IPluginBrowser>(TEXT("PluginBrowser"));
    return PluginBrowser.SpawnPluginCreatorTab(SpawnTabArgs, MakeShared<FModWizardDefinition>());
}

void FCrimeForgeModule::RegisterSettings() const {
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"))) {
        // Create the new category
        ISettingsContainerPtr SettingsContainer = SettingsModule->GetContainer(TEXT("Project"));
        SettingsContainer->DescribeCategory(TEXT("CrimeForge Settings"),
            LOCTEXT("RuntimeWDCategoryName", "CrimeForge Settings"),
            LOCTEXT("RuntimeWDCategoryDescription", "CrimeForge Settings. Can also be edited in the CrimeForge menu"));

        // Register the settings
        ISettingsSectionPtr SettingsSection = SettingsModule->RegisterSettings(TEXT("Project"), TEXT("CrimeForge"), TEXT("General"),
            LOCTEXT("RuntimeGeneralSettingsName", "General"),
            LOCTEXT("RuntimeGeneralSettingsDescription", "CrimeForge Settings. Can also be edited in the CrimeForge menu"),
            UCrimeForgeSettings::Get()
        );
        // Register the save handler to your settings, you might want to use it to
        // validate those or just act to settings changes.
        if (SettingsSection.IsValid()) {
            SettingsSection->OnModified().BindLambda([](){
                UCrimeForgeSettings::Get()->SaveSettings();
                return true;
            });
        }
    }
}

void FCrimeForgeModule::UnregisterSettings() const {
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings"))) {
        SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("CrimeForge"), TEXT("General"));
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

bool FCrimeForgeModule::RegisterPrimaryAssetLabelsFromPlugin(IPlugin& Plugin)
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
        UE_LOG(LogCrimeForge, Error,
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
        UE_LOG(LogCrimeForge, Error,
            TEXT("Asset Manager has no PrimaryAssetLabel entry in Primary Asset Types to Scan. "
                 "Create that entry once; CrimeForge will manage Directories and Specific Assets automatically."));
        return false;
    }

    FString PluginMountPath = Plugin.GetMountedAssetPath();
    PluginMountPath.RemoveFromEnd(TEXT("/"));

    if (PluginMountPath.IsEmpty())
    {
        UE_LOG(LogCrimeForge, Warning,
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

        UE_LOG(LogCrimeForge, Display,
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

            UE_LOG(LogCrimeForge, Display,
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
            UE_LOG(LogCrimeForge, Error,
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
        UE_LOG(LogCrimeForge, Verbose,
            TEXT("No conventional PAL found for %s at %s on this scan. "
                 "The directory is still registered; newly-mounted mods get a deferred rescan."),
            *Plugin.GetName(),
            *ConventionalPALPackage);
    }

    return bChanged;
}

void FCrimeForgeModule::SaveAndRefreshAssetManagerSettings()
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
     * CrimeForge can reach this function from StartupModule(), before GEngine and
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
            LogCrimeForge,
            Verbose,
            TEXT("Asset Manager already initialized; refreshed Primary Asset configuration.")
        );
    }
    else
    {
        UE_LOG(
            LogCrimeForge,
            Display,
            TEXT("Asset Manager is not initialized yet; PAL configuration was saved and will be picked up during normal initialization.")
        );
    }
#endif

    UE_LOG(LogCrimeForge, Display,
        TEXT("Asset Manager PAL registration saved to %s"),
        *DefaultGameIni);
}

void FCrimeForgeModule::EnsureAssetManagerRegistrationForPlugin(IPlugin& Plugin)
{
    if (RegisterPrimaryAssetLabelsFromPlugin(Plugin))
    {
        SaveAndRefreshAssetManagerSettings();
    }
}

void FCrimeForgeModule::RemoveAssetManagerRegistrationForPlugin(
    const FString& PluginName,
    const FString& PluginMountPath)
{
    UAssetManagerSettings* AssetManagerSettings =
        GetMutableDefault<UAssetManagerSettings>();

    if (!AssetManagerSettings)
    {
        UE_LOG(
            LogCrimeForge,
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
            LogCrimeForge,
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
            LogCrimeForge,
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
            LogCrimeForge,
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
        LogCrimeForge,
        Display,
        TEXT(
            "Removed Asset Manager registration for mod %s "
            "(Directories removed: %d, Specific Assets removed: %d)"
        ),
        *PluginName,
        RemovedDirectoryCount,
        RemovedSpecificAssetCount);
}

void FCrimeForgeModule::RegisterModTemplates()
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

    IPluginManager::Get().OnNewPluginMounted().AddLambda(
        [this](IPlugin& Plugin)
        {
            const FString PluginName = Plugin.GetName();

            if (Plugin.GetType() == EPluginType::Mod)
            {
                // Adds /<ModName> immediately even if PAL discovery is one tick early.
                EnsureAssetManagerRegistrationForPlugin(Plugin);

                // In UE5.5 the mount notification is observed before the package
                // mount is fully visible. Retry once after the callback returns.
                AsyncTask(
                    ENamedThreads::GameThread,
                    [this, PluginName]()
                    {
                        TSharedPtr<IPlugin> MountedPlugin =
                            IPluginManager::Get().FindPlugin(PluginName);

                        if (!MountedPlugin.IsValid() ||
                            MountedPlugin->GetType() != EPluginType::Mod)
                        {
                            return;
                        }

                        UE_LOG(
                            LogCrimeForge,
                            Verbose,
                            TEXT("Deferred Asset Manager PAL rescan for newly-mounted mod %s"),
                            *PluginName);

                        EnsureAssetManagerRegistrationForPlugin(
                            *MountedPlugin);
                    });
            }

            if (IsPluginAMod(Plugin))
            {
                AddModTemplatesFromPlugin(Plugin);
            }
        });
}

void FCrimeForgeModule::AddModTemplatesFromPlugin(IPlugin& Plugin) {
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
        UE_LOG(LogCrimeForge, Error, TEXT("Invalid templates for mod %s: invalid json"), *Plugin.GetName());
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* TemplatesArrayPtr;
    if(!TemplatesJSONObj->TryGetArrayField(TEXT("templates"), TemplatesArrayPtr)) {
        UE_LOG(LogCrimeForge, Error, TEXT("Invalid templates for mod %s: \"templates\" not in json"), *Plugin.GetName());
        return;
    }
    int i = 0;
    for(TSharedPtr<FJsonValue> Item : *TemplatesArrayPtr)
    {
        i++;
        const TSharedPtr<FJsonObject>* ItemObjPtr;
        if(!Item->TryGetObject(ItemObjPtr)) {
            UE_LOG(LogCrimeForge, Error, TEXT("Invalid template for mod %s: template %d: not an object"), *Plugin.GetName(), i);
            continue;
        }

        FString Error;
        TSharedPtr<FModTemplateDescription> ModTemplate = FModTemplateDescription::Load(*ItemObjPtr, TemplatesPath, Error);
        if(!ModTemplate.IsValid())
        {
            UE_LOG(LogCrimeForge, Error, TEXT("Invalid templates for mod %s: template %d: %s"), *Plugin.GetName(), i, *Error);
            continue;
        }
        ModTemplates.Add(ModTemplate.ToSharedRef());
    }    
}

#undef LOCTEXT_NAMESPACE
    
IMPLEMENT_MODULE(FCrimeForgeModule, CrimeForge)