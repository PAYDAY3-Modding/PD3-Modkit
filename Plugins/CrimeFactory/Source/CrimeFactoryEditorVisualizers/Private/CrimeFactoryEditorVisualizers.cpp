#include "CrimeFactoryActorVisualisationSettings.h"
#include "CrimeFactoryBillboardManager.h"
#include "ISettingsModule.h"
#include "Engine/Engine.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

#define LOCTEXT_NAMESPACE "CrimeFactoryEditorVisualizers"

class FCrimeFactoryEditorVisualizersModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (!GEngine || IsRunningCommandlet() || IsRunningGame())
        {
            return;
        }

        FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").RegisterSettings(
            "Project", "CrimeFactory", "ActorVisualisation",
            LOCTEXT("SettingsName", "Actor Visualisation"),
            LOCTEXT("SettingsDescription", "Configure editor-only actor billboards and pawn spawn previews. The closest configured ancestor takes priority. Duplicate class definitions are ignored with a warning."),
            GetMutableDefault<UCrimeFactoryActorVisualisationSettings>());
        Manager = MakeUnique<FCrimeFactoryBillboardManager>();
        Manager->Initialize();
        bSettingsRegistered = true;
        // Module shutdown runs after UObject teardown on normal editor exit.
        // Release delegates and strong preview references while their owners live.
        EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this,
            &FCrimeFactoryEditorVisualizersModule::StopVisualizers);
    }

    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
        EnginePreExitHandle.Reset();
        StopVisualizers();
    }

private:
    /** Used by both engine pre-exit and an explicit module unload; safe to repeat. */
    void StopVisualizers()
    {
        if (Manager)
        {
            Manager->Shutdown();
            Manager.Reset();
        }
        if (bSettingsRegistered)
        {
            bSettingsRegistered = false;
            if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
            {
                Settings->UnregisterSettings("Project", "CrimeFactory", "ActorVisualisation");
            }
        }
    }

    TUniquePtr<FCrimeFactoryBillboardManager> Manager;
    FDelegateHandle EnginePreExitHandle;
    bool bSettingsRegistered = false;
};

IMPLEMENT_MODULE(FCrimeFactoryEditorVisualizersModule, CrimeFactoryEditorVisualizers)

#undef LOCTEXT_NAMESPACE
