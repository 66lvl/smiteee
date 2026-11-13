@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if not exist "%VSWHERE%" goto missing
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do if not defined VSINSTALL set "VSINSTALL=%%I"
if not defined VSINSTALL goto missing
set "MSBUILD=%VSINSTALL%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" goto missing
"%MSBUILD%" "%~dp0tools\loader\SmiteLoader.csproj" /t:Rebuild /p:Configuration=Release /p:Platform=AnyCPU /m /nologo /v:minimal
if errorlevel 1 goto failure
if not exist "%~dp0dist" mkdir "%~dp0dist"
copy /y "%~dp0tools\loader\bin\Release\SmiteLoader.exe" "%~dp0dist\SmiteLoader.exe" >nul
if errorlevel 1 goto failure
echo Built: %~dp0dist\SmiteLoader.exe
pause
exit /b 0
:missing
echo MSBuild not found. Install Visual Studio with .NET Framework 4.8 targeting pack and C# build tools.
:failure
echo Loader build failed. See errors above.
pause
exit /b 1
