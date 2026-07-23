@echo off
title Momentum TAS Tool - Updater
color 0b
cd /d "%~dp0"

echo ============================================
echo    Momentum Movement Recorder - Updater
echo ============================================
echo.

where git >nul 2>nul
if errorlevel 1 (
    echo   Git isn't installed, so I can't auto-update.
    echo.
    echo   Either install Git from https://git-scm.com and run this again,
    echo   or just re-download the latest ZIP from the repo page.
    echo.
    pause
    exit /b 1
)

if not exist ".git" (
    echo   This folder isn't a git clone ^(no .git folder found^).
    echo.
    echo   You probably downloaded the ZIP. To get one-click updates, grab it
    echo   with git instead:
    echo       git clone ^<repo-url^> momentum-menu
    echo.
    echo   Otherwise just re-download the newest ZIP whenever you want to update.
    echo.
    pause
    exit /b 1
)

echo   Pulling the latest version...
echo.
git pull
echo.

if errorlevel 1 (
    echo ============================================
    echo    Update FAILED - see the message above.
    echo    ^(Usually: no internet, or local changes.^)
    echo ============================================
) else (
    echo ============================================
    echo    Up to date!
    echo    Run "Inject Tool.bat" to load the latest
    echo    into your game.
    echo ============================================
)
echo.
pause
