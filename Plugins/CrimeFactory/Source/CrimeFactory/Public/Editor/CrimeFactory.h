#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDelegates.h"
#include "Interfaces/IPluginManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCrimeFactory, Verbose, All);

class FCrimeFactoryModule : public IModuleInterface {
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    TArray<TSharedRef<struct FModTemplateDescription>> GetModTemplates() const { return ModTemplates; }

    /**
     * Ensure this mod is present in the PrimaryAssetLabel scan configuration
     * and persist it before a separate UAT cook starts.
     */
    void EnsureAssetManagerRegistrationForPlugin(IPlugin& Plugin);

    /**
     * Remove all Asset Manager registration owned by a deleted mod.
     * This removes /<ModName> from PrimaryAssetLabel Directories and removes
     * Specific Assets that live under the mod's mount path.
     */
    void RemoveAssetManagerRegistrationForPlugin(
        const FString& PluginName,
        const FString& PluginMountPath);
    
    /** ID name for the mod creator tab */
    static const FName ModCreatorTabName;
private:
    FDelegateHandle CookedEditorSettingsChangedHandle;
    FDelegateHandle PluginMountedHandle, PluginContentMountedHandle;
    TSharedPtr<class FUICommandList> PluginCommands;
    TSharedPtr<class FExtender> ToolbarExtender;

    TSharedRef<SDockTab> HandleSpawnModCreatorTab(const FSpawnTabArgs& SpawnTabArgs);
    
    void RegisterSettings() const;
    void UnregisterSettings() const;

    void RegisterModTemplates();
    void AddModTemplatesFromPlugin(IPlugin& Plugin);
    bool RegisterPrimaryAssetLabelsFromPlugin(IPlugin& Plugin);
    bool PruneDeletedModRegistrations();
    void SaveAndRefreshAssetManagerSettings();
    TArray<TSharedRef<struct FModTemplateDescription>> ModTemplates;
};
