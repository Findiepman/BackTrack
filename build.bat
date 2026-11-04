@echo off
rem Configures and builds Backtrack (Release) into build\, then runs the unit tests.
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
        set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
    ) else (
        echo CMake was not found. Install it with: winget install Kitware.CMake
        exit /b 1
    )
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release --parallel || exit /b 1

rem Only copy the default settings if there are none yet, so edits to the built copy survive a rebuild.
if not exist "build\Release\backtrack.ini" copy /y "backtrack.ini" "build\Release\backtrack.ini" >nul

ctest --test-dir build -C Release --output-on-failure || exit /b 1

echo.
echo Built build\Release\backtrack.exe
