@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Trainer - Rebuild Release x64

rem Place this file next to Trainer.vcxproj and run it from any directory.
cd /d "%~dp0"
set "PROJECT=%~dp0Trainer.vcxproj"
set "LOG=%~dp0build.log"
set "EXPECTED_DLL=%~dp0x64\Release\Trainer.dll"
set "MSBUILD="
set "VSINSTALL="

if not exist "%PROJECT%" goto no_project

rem First use the Visual Studio Installer locator. Build Tools are supported.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto try_path
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL goto try_path
if exist "%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if defined MSBUILD goto build

:try_path
for /f "delims=" %%I in ('where MSBuild.exe 2^>nul') do if not defined MSBUILD set "MSBUILD=%%I"
if not defined MSBUILD goto no_msbuild

:build
echo ==============================================================
echo Rebuilding Trainer: Release x64
echo Project: "%PROJECT%"
echo MSBuild: "%MSBUILD%"
echo Full log: "%LOG%"
echo ==============================================================
"%MSBUILD%" "%PROJECT%" /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo /fl "/flp:logfile=%LOG%;verbosity=normal;encoding=UTF-8"
if errorlevel 1 goto build_failed

echo.
echo BUILD SUCCEEDED.
if not exist "%EXPECTED_DLL%" goto output_unknown
echo DLL: "%EXPECTED_DLL%"
goto success

:output_unknown
echo The default DLL path was not found. Check the output location in build.log.
goto success

:no_project
echo ERROR: Trainer.vcxproj was not found next to this BAT file.
goto failure

:no_msbuild
echo ERROR: MSBuild was not found.
echo Install Visual Studio 2022 or Build Tools 2022 with Desktop development with C++.
echo The project also requires the v143 C++ toolset and a Windows 10 or 11 SDK.
goto failure

:build_failed
echo.
echo BUILD FAILED. See "%LOG%" for the complete output.
goto failure

:success
echo.
pause
exit /b 0

:failure
echo.
pause
exit /b 1
