@echo off
setlocal EnableExtensions

REM Always run from the folder containing Setup.bat.
cd /d "%~dp0" || goto :failed

echo =====================================
echo       PD3-Modkit Setup
echo =====================================
echo.

where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: Git was not found in PATH.
    echo Install Git for Windows, then run Setup.bat again.
    goto :failed
)

REM A normal Git clone has a .git directory (or a .git file in a worktree).
if exist ".git" (
    echo Git repository detected. Initializing submodules...
    git submodule update --init --recursive
    if errorlevel 1 goto :failed
    echo Pulling latest JukeboxMod from its configured upstream branch...
    git submodule update --remote Mods/JukeboxMod
    if errorlevel 1 goto :failed
) else (
    REM GitHub Download ZIP archives do not contain the .git metadata needed
    REM by git submodule. Clone JukeboxMod directly as a fallback.
    echo ZIP/downloaded copy detected. Setting up JukeboxMod directly...
    if exist "Mods\JukeboxMod\.git" (
        echo JukeboxMod is already cloned. Checking for updates...
        git -C "Mods\JukeboxMod" pull --ff-only origin master
        if errorlevel 1 goto :failed
    ) else (
        git clone --branch master https://github.com/braidy08/JukeboxMod.git "Mods\JukeboxMod"
        if errorlevel 1 (
            echo.
            echo If Mods\JukeboxMod contains files already, move or remove
            echo that folder before running Setup.bat again.
            goto :failed
        )
    )
)

echo.
echo Setup completed successfully.
echo.
pause
exit /b 0

:failed
echo.
echo Setup failed. Review the message above.
echo.
pause
exit /b 1
