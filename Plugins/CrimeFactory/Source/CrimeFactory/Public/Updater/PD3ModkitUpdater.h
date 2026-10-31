#pragma once

#include "CoreMinimal.h"

/**
 * Editor-side updater for the official PD3 Modkit and compatible UE5-PD3 releases.
 *
 * Modkit checks use Git, so the updater compares the
 * current checkout with the official remote branch after a fetch. Uncommitted
 * working-tree changes do not affect version detection.
 * Engine checks independently use the existing installer's authenticated GitHub CLI
 * release catalogue and per-installation release metadata. Engine actions open the
 * installer release page; engine files are never patched by this editor module.
 *
 * When the user accepts an update, Unreal saves dirty packages and launches a
 * visible PowerShell/WPF helper before closing. The helper waits for the editor
 * process to exit, creates a full safety backup of the project's Content folder,
 * snapshots tracked changes and staging under a unique recovery ref and backs up
 * untracked/ignored data, performs a fast-forward update, restores compatible local
 * changes, then regenerates, cleans and rebuilds the editor target before reopening.
 * Ambiguous changes require review; checkpoints allow rebuild/recovery retries.
 * Local commits and unrelated stashes are never rewritten.
 */
class FPD3ModkitUpdater
{
public:
    /** Start the delayed, silent startup update check. */
    static void Initialize();

    /** Remove any pending startup callback and prevent late async UI work. */
    static void Shutdown();

    /**
     * Check the canonical PD3 Modkit remote and compatible engine releases for updates.
     *
     * @param bUserInitiated When true, "up to date" and error messages are
     * shown to the user. Startup checks stay silent unless an update is found.
     */
    static void CheckForUpdates(bool bUserInitiated);

private:
    static bool HandleStartupCheck(float DeltaTime);
};
