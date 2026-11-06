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

cl %FLAGS% /c src\core\Config.cpp src\core\Hotkey.cpp src\core\ClipBuffer.cpp src\core\H264.cpp src\core\ClipPlan.cpp src\core\Mp4.cpp src\core\Json.cpp src\core\Library.cpp src\core\AudioFifo.cpp src\core\AudioMix.cpp src\core\Fft.cpp src\core\NoiseSuppressor.cpp src\core\ExportPlan.cpp /Fo%OUT%\ >nul || (echo Compiling core failed & exit /b 1)

set FAILED=0
for %%t in (ConfigTests HotkeyTests ClockTests ClipBufferTests H264Tests ClipPlanTests Mp4Tests JsonTests LibraryTests AudioFifoTests AudioMixTests NoiseSuppressorTests ExportPlanTests) do (
    if exist %OUT%\%%t.exe del /q %OUT%\%%t.exe
    cl %FLAGS% "/DBT_SOURCE_DIR=\"%ROOT%\"" tests\%%t.cpp %OUT%\Config.obj %OUT%\Hotkey.obj %OUT%\ClipBuffer.obj %OUT%\H264.obj %OUT%\ClipPlan.obj %OUT%\Mp4.obj %OUT%\Json.obj %OUT%\Library.obj %OUT%\AudioFifo.obj %OUT%\AudioMix.obj %OUT%\Fft.obj %OUT%\NoiseSuppressor.obj %OUT%\ExportPlan.obj /Fo%OUT%\ /Fe%OUT%\%%t.exe >nul || (echo Compiling %%t failed & set FAILED=1)
    if exist %OUT%\%%t.exe %OUT%\%%t.exe || set FAILED=1
)
if %FAILED%==1 (
    echo TESTS FAILED
    exit /b 1
)
echo All tests passed.
