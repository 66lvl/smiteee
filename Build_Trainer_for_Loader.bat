@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\update_sdk_build.ps1" -ProjectRoot "%~dp0."
if errorlevel 1 goto failure
echo Trainer package built. Check the SDK version matches the running game.
pause
exit /b 0
:failure
echo Trainer build failed. See messages above and build_sdk_update.log if present.
pause
exit /b 1
