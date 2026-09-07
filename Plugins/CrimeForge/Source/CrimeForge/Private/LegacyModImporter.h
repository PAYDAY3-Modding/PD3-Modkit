#pragma once

#include "CoreMinimal.h"

/**
 * Migrates an uncooked legacy UE4 ModLoader content folder from /Game into a
 * modern CrimeForge mod plugin mounted at /<ModName>.
 */
class FLegacyModImporter
{
public:
    /** Opens a folder picker and performs the migration. */
    static bool ImportFromFolderDialog(
        FString& OutImportedModName,
        FString& OutReport);
};
