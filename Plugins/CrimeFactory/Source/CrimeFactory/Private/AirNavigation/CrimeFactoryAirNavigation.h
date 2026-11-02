#pragma once

namespace CrimeFactoryAirNavigation
{
    // Generates native Starbreeze-compatible reflected data for the currently loaded authored map.
    // Does not save packages; the user must review and save the affected sublevels.
    void GenerateForEditorWorld();
}
