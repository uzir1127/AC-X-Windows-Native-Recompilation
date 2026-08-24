@echo off
setlocal
cd /d "%~dp0"

set "EXE=build\pspreco-sdl3.exe"
set "IMAGE=generated\image.bin"

if not exist "%EXE%" (
    echo [capture] %EXE% not found.
    echo [capture] Build it first:   python build.py --sdl3
    echo.
    pause
    exit /b 1
)

if not exist "%IMAGE%" (
    echo [capture] %IMAGE% not found.
    echo [capture] Produce it with:
    echo [capture]   python tools\prxload\prxload.py EBOOT.elf --emit-image generated\image.bin
    echo.
    pause
    exit /b 1
)

set "DISC="
if exist "game\disc.manifest" set "DISC=game"
if not defined DISC if defined PSPRECO_ISO set "DISC=%PSPRECO_ISO%"
if not defined DISC for %%F in ("*.iso") do set "DISC=%%~fF"

if not defined DISC (
    echo [capture] No disc found.
    echo [capture] Put an .iso in this folder, or extract one to game\, or set PSPRECO_ISO.
    echo.
    pause
    exit /b 1
)

if not exist logs mkdir logs
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
set "LOG=logs\capture-%STAMP%.log"

set PSPRECO_QUIET=1
set SR_GEPROF=1
set PSPRECO_WATCHDOG_S=0
set SR_TEXVERIFY=1

if /i "%~1"=="detail" (
    set SR_GESTAT=1
    echo [capture] detail mode on: per-60-frame GE breakdown included
)

echo.
echo   disc   %DISC%
echo   log    %LOG%
echo.
echo   What makes this capture useful:
echo.
echo     1. Get into an actual MISSION. The menus are 99%% game code and
echo        tell me nothing about the renderer.
echo     2. Once flying, hold each graphics setting for ~20 seconds so it
echo        gets several samples. Press F4 for the menu.
echo     3. Walk the resolution scale: 1, then 2, then 4. That is the one
echo        setting that reliably makes it GPU-bound.
echo     4. Fly low over terrain, into smoke and explosions, and past the
echo        sun. Heavy alpha and the sun glare are the known stutter cases.
echo     5. If it stutters, keep doing whatever caused it for a few seconds
echo        so it lands in more than one sample.
echo.
echo   Close the game window when you are done.
echo.

"%EXE%" "%IMAGE%" "%DISC%" > "%LOG%" 2>&1

echo.
echo [capture] done, wrote %LOG%
echo.
powershell -NoProfile -Command "$f='%LOG%'; $n=(Get-Content $f | Measure-Object -Line).Lines; Write-Host ('[capture] ' + $n + ' lines, ' + [math]::Round((Get-Item $f).Length/1KB,1) + ' KB')"
echo.
echo [capture] Send me that file.
echo.
pause
endlocal
