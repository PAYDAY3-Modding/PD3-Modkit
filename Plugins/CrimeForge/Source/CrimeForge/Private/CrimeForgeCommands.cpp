#include "CrimeForgeCommands.h"
#include "CrimeForgeStyle.h"

#define LOCTEXT_NAMESPACE "FCrimeForgeModule"

FCrimeForgeCommands::FCrimeForgeCommands() : TCommands<FCrimeForgeCommands>(
    TEXT("CrimeForge"),
    NSLOCTEXT("Contexts", "CrimeForge", "CrimeForge Plugin"),
    NAME_None,
    FCrimeForgeStyle::GetStyleSetName()) {}

void FCrimeForgeCommands::RegisterCommands() {
    UI_COMMAND(OpenPluginWindow, "CrimeForge", "Open the crimeforge window", EUserInterfaceActionType::Button, FInputChord{});
}

#undef LOCTEXT_NAMESPACE
