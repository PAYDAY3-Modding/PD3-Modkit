@echo off
setlocal

set "PD3_REBUILD_SCRIPT=%~f0"
set "PD3_REBUILD_ENGINE=%~1"

powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$lines = Get-Content -LiteralPath $env:PD3_REBUILD_SCRIPT; $marker = [Array]::IndexOf($lines, '# POWERSHELL SCRIPT'); & ([scriptblock]::Create(($lines[($marker + 1)..($lines.Length - 1)] -join [Environment]::NewLine)))"
set "PD3_REBUILD_RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %PD3_REBUILD_RESULT%

# POWERSHELL SCRIPT
$ErrorActionPreference = 'Stop'

function Resolve-PD3EngineRoot {
    param([string]$ProjectFile, [string]$Override)

    if (-not [string]::IsNullOrWhiteSpace($Override)) {
        return [IO.Path]::GetFullPath($Override).TrimEnd('\', '/')
    }

    $project = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json
    $association = [string]$project.EngineAssociation
    if ([string]::IsNullOrWhiteSpace($association)) {
        throw 'PAYDAY3.uproject has no EngineAssociation. Associate it with your UE5-PD3 installation, or pass the engine root as the first argument.'
    }

    # Custom/source/installed engines registered by UnrealVersionSelector.
    $registered = Get-ItemPropertyValue -LiteralPath 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -Name $association -ErrorAction SilentlyContinue
    if (-not [string]::IsNullOrWhiteSpace($registered)) {
        return [IO.Path]::GetFullPath($registered).TrimEnd('\', '/')
    }

    # Launcher installations use a version association and InstalledDirectory.
    foreach ($registryRoot in @('HKLM:\SOFTWARE\EpicGames\Unreal Engine', 'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine')) {
        $registered = Get-ItemPropertyValue -LiteralPath ($registryRoot + '\' + $association) -Name InstalledDirectory -ErrorAction SilentlyContinue
        if (-not [string]::IsNullOrWhiteSpace($registered)) {
            return [IO.Path]::GetFullPath($registered).TrimEnd('\', '/')
        }
    }

    throw "No registered engine matches EngineAssociation '$association'. Right-click PAYDAY3.uproject and select Switch Unreal Engine version to choose your UE5-PD3 installation, or pass the engine root as the first argument."
}

try {
    $projectFile = Join-Path (Split-Path -Parent $env:PD3_REBUILD_SCRIPT) 'PAYDAY3.uproject'
    $projectFile = (Resolve-Path -LiteralPath $projectFile).ProviderPath
    if ([IO.Path]::GetFileName($projectFile) -ne 'PAYDAY3.uproject') {
        throw 'Expected a PAYDAY3.uproject file.'
    }

    $projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $projectFile)).TrimEnd('\', '/')
    $engineRoot = Resolve-PD3EngineRoot -ProjectFile $projectFile -Override $env:PD3_REBUILD_ENGINE
    $build = Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat'
    if (-not (Test-Path -LiteralPath $build -PathType Leaf)) {
        throw "Engine Build.bat was not found under '$engineRoot'. Repair the engine registration or pass the correct engine root as the first argument. Nothing has been cleaned."
    }

    Set-Location -LiteralPath $projectRoot
    [Environment]::CurrentDirectory = $projectRoot

    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'Source\PAYDAY3Editor.Target.cs'))) {
        throw 'The PAYDAY3Editor target is missing. Nothing has been cleaned.'
    }

    $active = @(Get-Process -Name UnrealEditor, UnrealEditor-Cmd, UnrealBuildTool -ErrorAction SilentlyContinue)
    if ($active.Count -gt 0) {
        throw 'Close Unreal Editor and wait for Unreal builds to finish before running this file.'
    }

    $relativeFolders = @(
        'Binaries',
        'Intermediate',
        'Plugins\CrimeFactory\Binaries',
        'Plugins\CrimeFactory\Intermediate',
        'Plugins\Suzie\Binaries',
        'Plugins\Suzie\Intermediate'
    )

    # Validate every absolute target before deleting any folder. Reject junctions
    # and symlinks along the path so cleanup cannot escape the project.
    $targets = foreach ($relative in $relativeFolders) {
        $target = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
        if (-not $target.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Cleanup path is outside the project: $target"
        }

        $cursor = $target
        while ($cursor) {
            if (Test-Path -LiteralPath $cursor) {
                $item = Get-Item -LiteralPath $cursor -Force
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Cleanup refused because the path contains a junction or symlink: $cursor"
                }
            }
            $cursor = Split-Path -Parent $cursor
        }

        if (Test-Path -LiteralPath $target) {
            $linked = Get-ChildItem -LiteralPath $target -Force -Recurse -Attributes ReparsePoint | Select-Object -First 1
            if ($linked) { throw "Cleanup refused because the folder contains a junction or symlink: $($linked.FullName)" }
        }

        $target
    }

    Write-Host ''
    Write-Host "Engine:  $engineRoot"
    Write-Host "Project: $projectFile"
    Write-Host ''

    foreach ($target in $targets) {
        if (Test-Path -LiteralPath $target) {
            Write-Host "Cleaning $target"
            Remove-Item -LiteralPath $target -Recurse -Force
            Write-Host ''
        }
    }

    Write-Host 'Building PAYDAY3Editor Development / Win64...'
    Write-Host ''

    & $build PAYDAY3Editor Win64 Development "-Project=$projectFile" -WaitMutex -NoHotReloadFromIDE
    $buildResult = $LASTEXITCODE

    Write-Host ''
    if ($buildResult -ne 0) {
        Write-Host "Build failed (exit code $buildResult)." -ForegroundColor Red
        exit $buildResult
    }

    Write-Host 'Build completed successfully.' -ForegroundColor Green
    exit 0
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
