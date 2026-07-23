# Rebuild + inject in one shot.
# Force-unloads any currently-loaded copy of momentum_menu.dll from momentum.exe
# BEFORE building, so the linker can overwrite the file.

$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
Push-Location $root

Write-Host "==> Force-unload any prior copies from Momentum" -ForegroundColor Cyan
& powershell -File "$root\unload-only.ps1"

Write-Host ""
Write-Host "==> cmake --build build --config Release" -ForegroundColor Cyan
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed - not injecting." -ForegroundColor Red
    Pop-Location
    exit 1
}

Write-Host ""
Write-Host "==> copy fresh DLL next to the launcher" -ForegroundColor Cyan
Copy-Item "$root\build\Release\momentum_menu.dll" "$root\momentum_menu.dll" -Force

Write-Host ""
Write-Host "==> inject fresh DLL" -ForegroundColor Cyan
& powershell -File "$root\inject.ps1"

Pop-Location
