#pragma once
#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

class FCrimeForgeCommands : public TCommands<FCrimeForgeCommands> {
public:
    FCrimeForgeCommands();

    virtual void RegisterCommands() override;
public:
    TSharedPtr<FUICommandInfo> OpenPluginWindow;
};