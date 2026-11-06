cmake_minimum_required(VERSION 3.25)
project(Backtrack VERSION 0.3.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Backtrack is built with MSVC (Visual Studio 2022 Build Tools).")
endif()

# The clip buffer can hold several GB at the config maximum (300 s at 100 Mbit/s).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Backtrack is 64-bit only. Configure with -A x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the exe must run on a clean Windows install without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(bt_options INTERFACE)
target_compile_options(bt_options INTERFACE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus)
target_compile_definitions(bt_options INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Pure C++ logic. Must never include a Windows header.
add_library(backtrack_core STATIC
    src/core/Config.cpp
    src/core/Hotkey.cpp
    src/core/ClipBuffer.cpp
    src/core/H264.cpp
    src/core/ClipPlan.cpp
    src/core/Mp4.cpp
    src/core/Json.cpp
    src/core/Library.cpp
    src/core/TrimPlan.cpp
    src/core/AudioFifo.cpp
    src/core/AudioMix.cpp
    src/core/Fft.cpp
    src/core/NoiseSuppressor.cpp
    src/core/ExportPlan.cpp
)
target_include_directories(backtrack_core PUBLIC src)
target_link_libraries(backtrack_core PUBLIC bt_options)

# WIN32: a window app, no console window. The manifest turns on modern common controls and
# per-monitor DPI awareness.
add_executable(backtrack WIN32
    src/main.cpp
    src/backtrack.manifest
    src/platform/Log.cpp
    src/platform/ScreenCapture.cpp
    src/platform/VideoEncoder.cpp
    src/platform/AudioCapture.cpp
    src/platform/MicCapture.cpp
    src/platform/ClipWriter.cpp
    src/platform/Recorder.cpp
    src/platform/MediaProbe.cpp
    src/platform/FrameReader.cpp
    src/platform/Filmstrip.cpp
    src/platform/Trim.cpp
    src/platform/Exporter.cpp
    src/platform/Shell.cpp
    src/platform/VideoPlayer.cpp
    src/platform/IniFile.cpp
    src/ui/App.cpp
    src/ui/Widgets.cpp
    src/ui/ClipPage.cpp
    src/ui/RecordPage.cpp
    src/ui/SettingsPage.cpp
    src/ui/Graphics.cpp
    src/ui/Icon.cpp
    src/ui/Logo.cpp
    src/backtrack.rc
)
target_include_directories(backtrack PRIVATE src)
target_compile_definitions(backtrack PRIVATE BT_VERSION="${PROJECT_VERSION}")
target_link_libraries(backtrack PRIVATE backtrack_core d3d11 dxgi mfplat mfuuid mfreadwrite winmm avrt wmcodecdspuuid
                      shell32 ole32 d2d1 dwrite dxguid dwmapi comctl32 windowscodecs propsys)

# The icon file built into the exe, drawn by the same code as the window's logo.
add_executable(make_icon tools/make_icon.cpp src/ui/Icon.cpp src/ui/Logo.cpp)
target_include_directories(make_icon PRIVATE src)
target_link_libraries(make_icon PRIVATE bt_options d2d1 windowscodecs ole32)
set(BT_ICON ${CMAKE_CURRENT_BINARY_DIR}/backtrack.ico)
add_custom_command(OUTPUT ${BT_ICON} COMMAND make_icon ${BT_ICON} DEPENDS make_icon
                   COMMENT "Drawing backtrack.ico from the logo")
add_custom_target(backtrack_icon DEPENDS ${BT_ICON})
add_dependencies(backtrack backtrack_icon)
set_source_files_properties(src/backtrack.rc PROPERTIES OBJECT_DEPENDS ${BT_ICON})
target_include_directories(backtrack PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Development tool for the audio/video sync check (tools/check_sync.ps1). Not shipped.
add_executable(sync_beacon tools/sync_beacon.cpp)
target_link_libraries(sync_beacon PRIVATE bt_options winmm ole32)

# Development tool: runs a trim or a filmstrip from the command line (tools/media_check.cpp). Not shipped.
add_executable(media_check tools/media_check.cpp src/platform/Log.cpp src/platform/ClipWriter.cpp
               src/platform/Trim.cpp src/platform/FrameReader.cpp src/platform/Filmstrip.cpp)
target_include_directories(media_check PRIVATE src)
target_link_libraries(media_check PRIVATE backtrack_core mfplat mfuuid mfreadwrite ole32 propsys)

# Development tool: runs an export from the command line (tools/export_check.cpp). Not shipped.
add_executable(export_check tools/export_check.cpp src/platform/Log.cpp src/platform/Exporter.cpp
               src/platform/FrameReader.cpp)
target_include_directories(export_check PRIVATE src)
target_link_libraries(export_check PRIVATE backtrack_core d3d11 mfplat mfuuid mfreadwrite ole32 propsys windowscodecs)

# Development tool: the recording engine without the window and its single-instance lock, to check
# sound and picture while Backtrack itself runs (tools/record_check.cpp). Not shipped.
add_executable(record_check tools/record_check.cpp src/platform/Log.cpp src/platform/ScreenCapture.cpp
               src/platform/VideoEncoder.cpp src/platform/AudioCapture.cpp src/platform/MicCapture.cpp
               src/platform/ClipWriter.cpp src/platform/Recorder.cpp)
target_include_directories(record_check PRIVATE src)
target_link_libraries(record_check PRIVATE backtrack_core d3d11 dxgi mfplat mfuuid mfreadwrite winmm avrt
                      wmcodecdspuuid shell32 ole32 propsys)

enable_testing()
foreach(test_name ConfigTests HotkeyTests ClockTests ClipBufferTests H264Tests ClipPlanTests Mp4Tests JsonTests LibraryTests TrimPlanTests AudioFifoTests AudioMixTests NoiseSuppressorTests ExportPlanTests)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE backtrack_core)
    target_compile_definitions(${test_name} PRIVATE BT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
