@echo off
setlocal EnableExtensions

rem =============================================================================
rem  Lynx : moteur compile en Release (optimise, sans infos de debug)
rem -----------------------------------------------------------------------------
rem  Dossier : build-release\ (a cote de build\, qui reste la version Debug de
rem  l'editeur). Ship Game prend LynxRuntime.exe et les DLL du moteur dans ce
rem  dossier quand il existe : le jeu livre est donc optimise.
rem
rem  Meme generateur CMake que build\ (lu dans build\CMakeCache.txt), sinon
rem  "MinGW Makefiles".
rem =============================================================================

cd /d "%~dp0" || exit /b 1

set "BUILD_DIR=%~dp0build-release"
set "GENERATOR="

if exist "%~dp0build\CMakeCache.txt" (
    for /f "tokens=2 delims==" %%G in ('findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "%~dp0build\CMakeCache.txt"') do set "GENERATOR=%%G"
)
if not defined GENERATOR set "GENERATOR=MinGW Makefiles"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo.
echo === Configuration Release (%GENERATOR%) ===
if exist "%BUILD_DIR%\CMakeCache.txt" (
    cmake -S "%~dp0." -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release -DCOMPILE_VULKAN=ON
) else (
    cmake -S "%~dp0." -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 (
    echo.
    echo Erreur pendant la configuration CMake.
    pause
    exit /b 1
)

echo.
echo === Compilation Release ===
cmake --build "%BUILD_DIR%" --config Release -j16
if errorlevel 1 (
    echo.
    echo Erreur pendant la compilation.
    pause
    exit /b 1
)

echo.
echo Release compilee dans : %BUILD_DIR%
pause
exit /b 0
