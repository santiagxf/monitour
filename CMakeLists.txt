cmake_minimum_required(VERSION 3.27)

project(Monitour
    VERSION 0.1.0
    DESCRIPTION "Webcam-driven focus follower for multi-monitor Windows setups"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Monitour is Windows-only (Media Foundation, Windows ML, Win32).")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Monitour requires MSVC (C++/WinRT, D3D11, MMCSS).")
endif()

add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Oi> $<$<CONFIG:Release>:/GL>)
add_link_options($<$<CONFIG:Release>:/LTCG>)

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINRT_LEAN_AND_MEAN
)

# Windows SDK target version: Windows 11 22H2 (10.0.22621.0) for Windows ML APIs.
add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000010
)

find_package(unofficial-onnxruntime CONFIG QUIET)

set(MONITOUR_SOURCES
    src/main.cpp
    src/tray/TrayIcon.cpp
    src/tray/Settings.cpp
    src/capture/MfCaptureSession.cpp
    src/capture/FrameSlot.cpp
    src/inference/HeadPoseModel.cpp
    src/inference/DeviceFactory.cpp
    src/inference/FaceDetector.cpp
    src/focus/WindowEnumerator.cpp
    src/focus/MonitorMru.cpp
    src/focus/MonitorLayout.cpp
    src/focus/ForegroundSetter.cpp
    src/calibration/PassiveCalibrator.cpp
    src/input/ActivityHooks.cpp
    src/d3d/D3DContext.cpp
    src/d3d/Nv12ToTensor.cpp
    src/overlay/DebugOverlay.cpp
    src/util/ScopedMmcss.cpp
    src/util/Logging.cpp
)

add_executable(Monitour WIN32 ${MONITOUR_SOURCES})

target_include_directories(Monitour PRIVATE src)

target_link_libraries(Monitour PRIVATE
    # Win32
    user32 gdi32 shell32 advapi32 ole32 oleaut32 uuid
    # Media Foundation
    mf mfplat mfreadwrite mfuuid
    # D3D / DXGI
    d3d11 dxgi d3dcompiler
    # MMCSS
    avrt
    # Multimedia (PlaySound)
    winmm
    # Power
    powrprof
    # UI Automation (for SetForegroundWindow fallback)
    uiautomationcore
    # Shcore (DPI)
    shcore
    # Runtime / WinRT
    windowsapp
    runtimeobject
)

# C++/WinRT: header-only via Windows SDK. Force the latest projection.
target_compile_definitions(Monitour PRIVATE
    WINRT_NO_MAKE_DETECTION
)

# Compile HLSL shader at build time. Output goes next to the exe.
set(SHADER_SRC ${CMAKE_SOURCE_DIR}/shaders/Nv12ToTensor.hlsl)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/Nv12ToTensor.cso)
add_custom_command(
    OUTPUT ${SHADER_OUT}
    COMMAND fxc.exe /nologo /T cs_5_0 /E main /Fo ${SHADER_OUT} ${SHADER_SRC}
    DEPENDS ${SHADER_SRC}
    COMMENT "Compiling Nv12ToTensor.hlsl"
    VERBATIM
)
add_custom_target(MonitourShaders DEPENDS ${SHADER_OUT})
add_dependencies(Monitour MonitourShaders)

# Copy models next to the exe for dev runs. Models are optional and not in the
# repo, so copy each only if present (build must succeed without them).
add_custom_command(TARGET Monitour POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/models/6drepnet.int8.onnx
        -DDST=$<TARGET_FILE_DIR:Monitour>/6drepnet.int8.onnx
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/models/6drepnet.fp16.onnx
        -DDST=$<TARGET_FILE_DIR:Monitour>/6drepnet.fp16.onnx
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_OUT}
        $<TARGET_FILE_DIR:Monitour>/Nv12ToTensor.cso
)

enable_testing()
add_subdirectory(tests)
