@echo off
setlocal DisableDelayedExpansion
set "GAME_ISO="
if not "%~1"=="" set "GAME_ISO=%~f1"
cd /d "%~dp0"
if not exist "pspreco-sdl3.exe" (
    echo The game executable is missing. Extract the complete Windows download first.
    pause
    exit /b 1
)
if not exist "SDL3.dll" (
    echo SDL3.dll is missing. Extract the complete Windows ZIP before launching.
    pause
    exit /b 1
)
set "PSPRECO_FONTDIR=font"
set "PSPRECO_MEMSTICK=memstick"
set "PSPRECO_SETTINGS=pspreco.toml"
set "PSPRECO_MODS=mods"
set "PSPRECO_FPS="
set "PSPRECO_WATCHDOG_S=0"
set "PSPRECO_MAX_FRAMES=0"
set "PSPRECO_QUIET=1"
set "PSPRECO_AUTOPRESS="
set "PSPRECO_TEST_INPUT="
set "PSPRECO_TEST_VIDEO_HASH="
set "PSPRECO_TIME_SCALE=1"
set "PSPRECO_UNTHROTTLED="
if not exist logs mkdir logs
if not exist logs (
    echo Cannot write here. Extract the game to a folder you can write to.
    pause
    exit /b 1
)
echo Starting Ace Combat X...
if defined GAME_ISO (
    pspreco-sdl3.exe --iso "%GAME_ISO%" >"logs\last-run.log" 2>&1
) else (
    pspreco-sdl3.exe --iso >"logs\last-run.log" 2>&1
)
set "GAME_RESULT=%ERRORLEVEL%"
if not "%GAME_RESULT%"=="0" (
    type "logs\last-run.log"
    echo.
    echo The game could not start. Details are in logs\last-run.log.
    pause
)
exit /b %GAME_RESULT%
