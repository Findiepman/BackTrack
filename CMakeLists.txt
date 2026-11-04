cmake_minimum_required(VERSION 3.25)
project(Backtrack VERSION 0.1.0 LANGUAGES CXX)

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
)
target_include_directories(backtrack_core PUBLIC src)
target_link_libraries(backtrack_core PUBLIC bt_options)

add_executable(backtrack
    src/main.cpp
    src/platform/Log.cpp
    src/platform/ScreenCapture.cpp
    src/platform/VideoEncoder.cpp
    src/platform/AudioCapture.cpp
    src/platform/ClipWriter.cpp
    src/platform/Recorder.cpp
)
target_include_directories(backtrack PRIVATE src)
target_link_libraries(backtrack PRIVATE backtrack_core d3d11 dxgi mfplat mfuuid mfreadwrite winmm avrt wmcodecdspuuid shell32 ole32)

# Development tool for the audio/video sync check (tools/check_sync.ps1). Not shipped.
add_executable(sync_beacon tools/sync_beacon.cpp)
target_link_libraries(sync_beacon PRIVATE bt_options winmm ole32)

enable_testing()
foreach(test_name ConfigTests HotkeyTests ClockTests ClipBufferTests H264Tests ClipPlanTests)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE backtrack_core)
    target_compile_definitions(${test_name} PRIVATE BT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
