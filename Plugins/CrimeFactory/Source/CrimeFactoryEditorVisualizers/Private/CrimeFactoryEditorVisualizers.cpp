#include "CrimeFactoryActorVisualisationSettings.h"
#include "CrimeFactoryBillboardManager.h"
#include "ISettingsModule.h"
#include "Engine/Engine.h"
#include "Modules/ModuleManager.h"

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
    }

    virtual void ShutdownModule() override
    {
        if (Manager)
        {
            Manager->Shutdown();
            Manager.Reset();
        }
        if (ISettingsModule* Settings = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
        {
            Settings->UnregisterSettings("Project", "CrimeFactory", "ActorVisualisation");
        }
    }

private:
    TUniquePtr<FCrimeFactoryBillboardManager> Manager;
};

IMPLEMENT_MODULE(FCrimeFactoryEditorVisualizersModule, CrimeFactoryEditorVisualizers)

#undef LOCTEXT_NAMESPACE
