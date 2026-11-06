@echo off
rem Compiles the unit tests with cl.exe directly (no CMake, no framework) and runs them.
setlocal
cd /d "%~dp0\.."
set "ROOT=%CD:\=/%"

where cl >nul 2>nul && goto :have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
rem vcvars64 calls vswhere by bare name and complains when it is not on PATH; that message is harmless.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:have_cl

set "OUT=build\quick-tests"
if not exist "%OUT%" mkdir "%OUT%"
set "FLAGS=/nologo /std:c++20 /W4 /WX /permissive- /utf-8 /EHsc /MT /Isrc"

rem Every core file, so a new one needs no change here. Their objects get a folder of their own:
rem linking %OUT%\*.obj would also pick up the test objects, each with a main().
if not exist "%OUT%\core" mkdir "%OUT%\core"
cl %FLAGS% /c src\core\*.cpp /Fo%OUT%\core\ >nul || (echo Compiling core failed & exit /b 1)

set FAILED=0
for %%t in (ConfigTests HotkeyTests ClockTests ClipBufferTests H264Tests ClipPlanTests Mp4Tests JsonTests LibraryTests AudioFifoTests AudioMixTests NoiseSuppressorTests ThemeColorsTests CaptureMathTests UpdateCheckTests ExportPlanTests ShareUploadTests) do (
    if exist %OUT%\%%t.exe del /q %OUT%\%%t.exe
    cl %FLAGS% "/DBT_SOURCE_DIR=\"%ROOT%\"" tests\%%t.cpp %OUT%\core\*.obj /Fo%OUT%\ /Fe%OUT%\%%t.exe >nul || (echo Compiling %%t failed & set FAILED=1)
    if exist %OUT%\%%t.exe %OUT%\%%t.exe || set FAILED=1
)
if %FAILED%==1 (
    echo TESTS FAILED
    exit /b 1
)
echo All tests passed.
