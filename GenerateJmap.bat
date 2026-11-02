@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Always run from the project root, regardless of how this batch file was launched.
cd /d "%~dp0"

set "DUMPER=%~dp0Tools\jmap_dumper.exe"
set "PROCESS_CONTROL=%~dp0Tools\SuspendResumeProcess.ps1"
set "OUTPUT=%~dp0PD3.jmap"
set "DESTDIR=%~dp0Content\DynamicClasses"
set "DEST=%DESTDIR%\PD3.jmap"
set "PD3_SUSPENDED="

echo.
echo ============================================
echo        PD3-Modkit Jmap Generator
echo ============================================
echo.

if not exist "%DUMPER%" (
    echo [ERROR] jmap_dumper.exe was not found:
    echo         "%DUMPER%"
    echo.
    echo Place the modified dumper at:
    echo         Tools\jmap_dumper.exe
    echo.
    pause
    exit /b 1
)

if not exist "%PROCESS_CONTROL%" (
    echo [ERROR] SuspendResumeProcess.ps1 was not found:
    echo         "%PROCESS_CONTROL%"
    echo.
    echo Place it at:
    echo         Tools\SuspendResumeProcess.ps1
    echo.
    pause
    exit /b 1
)

if not exist "%DESTDIR%" (
    echo [INFO] Creating Content\DynamicClasses...
    mkdir "%DESTDIR%" >nul 2>&1

    if errorlevel 1 (
        echo [ERROR] Failed to create:
        echo         "%DESTDIR%"
        pause
        exit /b 1
    )
)

rem Remove a stale temporary dump from a previous failed run.
if exist "%OUTPUT%" (
    del /q "%OUTPUT%" >nul 2>&1
)

rem Check whether PAYDAY 3 is already running.
call :FindPD3Process

if defined PD3_PID (
    echo [INFO] PAYDAY 3 is already running. PID: !PD3_PID!
    goto GameFound
)

echo Enter your PAYDAY 3 installation directory.
echo You can also paste the Content, PAYDAY3, or Binaries\Win64 / Binaries\WinGDK folder.
echo Examples:
echo   Steam/Epic:  C:\Program Files (x86)\Steam\steamapps\common\PAYDAY3
echo   Xbox/MS:     D:\Games\Payday 3
echo   Xbox/MS bin: D:\Games\Payday 3\Content\PAYDAY3\Binaries\WinGDK
echo.

set "GAME_PATH="
set /p "GAME_PATH=PAYDAY 3 path: "

rem Remove surrounding quotes if the path was pasted as "C:\...".
set "GAME_PATH=!GAME_PATH:"=!"

if not defined GAME_PATH (
    echo.
    echo [ERROR] No PAYDAY 3 installation directory was entered.
    echo.
    pause
    exit /b 1
)

if not exist "!GAME_PATH!\" (
    echo.
    echo [ERROR] The directory does not exist:
    echo         "!GAME_PATH!"
    echo.
    pause
    exit /b 1
)

set "GAME_EXE="

rem Prefer the launcher at an install root, then check all supported PC layouts.
if exist "!GAME_PATH!\PAYDAY3.exe" (
    set "GAME_EXE=!GAME_PATH!\PAYDAY3.exe"
)

rem Accept an install root, Xbox Content folder, PAYDAY3 folder, or binaries folder.
for %%D in ("." "PAYDAY3\Binaries\Win64" "PAYDAY3\Binaries\WinGDK" "Content\PAYDAY3\Binaries\Win64" "Content\PAYDAY3\Binaries\WinGDK" "Binaries\Win64" "Binaries\WinGDK") do (
    for %%E in (PAYDAY3Client-Win64-Shipping.exe PAYDAY3-Win64-Shipping.exe PAYDAY3Client-WinGDK-Shipping.exe PAYDAY3-WinGDK-Shipping.exe) do (
        if not defined GAME_EXE if exist "!GAME_PATH!\%%~D\%%E" (
            set "GAME_EXE=!GAME_PATH!\%%~D\%%E"
        )
    )
)

if not defined GAME_EXE (
    echo.
    echo [ERROR] Could not find a PAYDAY 3 executable under:
    echo         "!GAME_PATH!"
    echo.
    echo [INFO] Steam/Epic expects a Win64 executable.
    echo [INFO] Xbox/Microsoft Store expects a WinGDK executable.
    echo [INFO] You can also start the game in its store app, then run this script again.
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] Launching:
echo        "!GAME_EXE!"
echo.

start "" "!GAME_EXE!"

echo [INFO] Waiting for the PAYDAY 3 game process...
set /a WAIT_SECONDS=0

:WaitForGame
timeout /t 1 /nobreak >nul
set /a WAIT_SECONDS+=1
call :FindPD3Process

if defined PD3_PID goto GameFound

if !WAIT_SECONDS! GEQ 180 (
    echo.
    echo [ERROR] Timed out waiting for PAYDAY 3 to start.
    echo.
    pause
    exit /b 1
)

goto WaitForGame

:GameFound

echo(!PD3_PID!| findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 (
    echo.
    echo [ERROR] PAYDAY 3 process detection returned an invalid PID:
    echo         "!PD3_PID!"
    echo.
    pause
    exit /b 1
)

echo [INFO] Found PAYDAY 3. PID: !PD3_PID!
echo [INFO] Waiting briefly for Unreal Engine initialization...
timeout /t 5 /nobreak >nul

echo [INFO] Suspending PAYDAY 3 while the Jmap is generated...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROCESS_CONTROL%" -ProcessId !PD3_PID! -Action Suspend

if errorlevel 1 (
    echo.
    echo [WARNING] Failed to suspend PAYDAY 3.
    echo [WARNING] Continuing without suspension.
    echo.
) else (
    set "PD3_SUSPENDED=1"
    echo [INFO] PAYDAY 3 suspended.
)

echo.
echo [INFO] Dumping reflection data...
echo.

"%DUMPER%" --pid !PD3_PID! --engine-version 5.5 --case-preserving --all --infer-bitmasks --infer-exec-pins -vv PD3.jmap
set "DUMPER_EXIT=!ERRORLEVEL!"

if defined PD3_SUSPENDED (
    echo.
    echo [INFO] Resuming PAYDAY 3...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROCESS_CONTROL%" -ProcessId !PD3_PID! -Action Resume

    if errorlevel 1 (
        echo [WARNING] Failed to resume PAYDAY 3 automatically.
        echo [WARNING] If the game remains frozen, close it in Task Manager.
        echo.
    ) else (
        echo [INFO] PAYDAY 3 resumed.
    )
)

if not "!DUMPER_EXIT!"=="0" (
    echo.
    echo [ERROR] jmap_dumper exited with code !DUMPER_EXIT!.
    if exist "%OUTPUT%" (
        echo [INFO] A partial PD3.jmap was produced but will not be installed.
    )
    echo.
    pause
    exit /b !DUMPER_EXIT!
)

if not exist "%OUTPUT%" (
    echo.
    echo [ERROR] jmap_dumper completed but PD3.jmap was not created.
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] Installing PD3.jmap...
move /Y "%OUTPUT%" "%DEST%" >nul

if errorlevel 1 (
    echo.
    echo [ERROR] Failed to move PD3.jmap to:
    echo         "%DEST%"
    echo.
    pause
    exit /b 1
)

echo.
echo [SUCCESS] Jmap updated:
echo           Content\DynamicClasses\PD3.jmap
echo.
pause
exit /b 0


:FindPD3Process
set "PD3_PID="

rem Steam/Epic process names.
for /f "tokens=2 delims=," %%P in ('tasklist /FI "IMAGENAME eq PAYDAY3-Win64-Shipping.exe" /FO CSV /NH 2^>nul') do (
    set "PD3_PID=%%~P"
    goto ValidatePD3Pid
)

for /f "tokens=2 delims=," %%P in ('tasklist /FI "IMAGENAME eq PAYDAY3Client-Win64-Shipping.exe" /FO CSV /NH 2^>nul') do (
    set "PD3_PID=%%~P"
    goto ValidatePD3Pid
)

rem Xbox app / Microsoft Store (GDK) process names.
for /f "tokens=2 delims=," %%P in ('tasklist /FI "IMAGENAME eq PAYDAY3Client-WinGDK-Shipping.exe" /FO CSV /NH 2^>nul') do (
    set "PD3_PID=%%~P"
    goto ValidatePD3Pid
)

for /f "tokens=2 delims=," %%P in ('tasklist /FI "IMAGENAME eq PAYDAY3-WinGDK-Shipping.exe" /FO CSV /NH 2^>nul') do (
    set "PD3_PID=%%~P"
    goto ValidatePD3Pid
)

exit /b 0

:ValidatePD3Pid
echo(!PD3_PID!| findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 (
    set "PD3_PID="
)
exit /b 0
