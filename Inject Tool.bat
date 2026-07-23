@echo off
title Momentum TAS Tool - Injector
color 0b

echo ============================================
echo    Momentum Movement Recorder - Injector
echo ============================================
echo.
echo Make sure Momentum Mod is running and you are
echo loaded into a map, then this will inject the tool.
echo.

:retry
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0inject.ps1"
if %errorlevel% equ 0 goto done

echo.
echo   ^> Momentum isn't running yet (or not ready).
echo   ^> Start the game, load a map, and I'll keep trying...
echo.
timeout /t 3 /nobreak >NUL
goto retry

:done
echo.
echo ============================================
echo    Injected. Press INSERT in-game for the menu.
echo ============================================
echo.
echo You can close this window.
pause >NUL
