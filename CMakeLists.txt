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

# Windows SDK target: Windows 11 24H2 (build 26100). Required for the NPU
# execution providers exposed via Microsoft.Windows.AI.MachineLearning 2.x.
# Older Windows builds will not link or run; that's intentional — the whole
# point of this binary is NPU dispatch on Copilot+ hardware.
add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000F
)

# --- Microsoft.Windows.AI.MachineLearning (Windows ML 2.x) via NuGet ---
# We resolve the package at configure time via `nuget restore packages.config`
# and add its include + lib directories to the Monitour target. The package
# ships the ONNX Runtime C++ headers, an import lib, and the loader DLL.
# nuget.exe is needed to restore Microsoft.Windows.AI.MachineLearning. We
# check the usual places: PATH, the well-known package-manager locations
# (chocolatey, winget Links shim, scoop shim), and winget's per-package
# install root (which winget does NOT add to PATH by default).
find_program(NUGET_EXE
    NAMES nuget nuget.exe
    HINTS
        "$ENV{NUGET_HOME}"
        "$ENV{ProgramData}/chocolatey/bin"
        "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links"
        "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/Microsoft.NuGet_Microsoft.Winget.Source_8wekyb3d8bbwe"
        "$ENV{USERPROFILE}/scoop/shims"
    DOC "Path to nuget.exe (required to restore Microsoft.Windows.AI.MachineLearning)")
if(NOT NUGET_EXE)
    # Last resort: glob winget's per-package install root for any nuget.exe.
    # winget puts each package under a versioned subfolder that we can't
    # know up front, so this beats hardcoding a version.
    file(GLOB_RECURSE _winget_nuget
        "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/*/nuget.exe")
    if(_winget_nuget)
        list(GET _winget_nuget 0 NUGET_EXE)
        message(STATUS "Found nuget.exe via winget install root: ${NUGET_EXE}")
    endif()
endif()
if(NOT NUGET_EXE)
    message(FATAL_ERROR
        "nuget.exe not found. Tried PATH, chocolatey, winget Links and "
        "package roots, and scoop shims. Install with one of:\n"
        "  winget install Microsoft.NuGet     (then restart the shell so PATH is refreshed)\n"
        "  choco install nuget.commandline\n"
        "  scoop install nuget\n"
        "Or download nuget.exe from https://www.nuget.org/downloads and "
        "set the NUGET_HOME environment variable to its directory.")
endif()
message(STATUS "Using nuget: ${NUGET_EXE}")

set(MONITOUR_PACKAGES_DIR "${CMAKE_BINARY_DIR}/packages")
execute_process(
    COMMAND "${NUGET_EXE}" restore "${CMAKE_SOURCE_DIR}/packages.config"
            -PackagesDirectory "${MONITOUR_PACKAGES_DIR}"
            -NonInteractive
    RESULT_VARIABLE _nuget_rc
    OUTPUT_VARIABLE _nuget_out
    ERROR_VARIABLE  _nuget_out)
if(NOT _nuget_rc EQUAL 0)
    message(FATAL_ERROR "nuget restore failed:\n${_nuget_out}")
endif()

# Locate the restored package. NuGet writes to
# <packages>/Microsoft.Windows.AI.MachineLearning.<version>/...
file(GLOB _winml_pkg LIST_DIRECTORIES true
     "${MONITOUR_PACKAGES_DIR}/Microsoft.Windows.AI.MachineLearning.*")
if(NOT _winml_pkg)
    message(FATAL_ERROR "Microsoft.Windows.AI.MachineLearning package not found "
                        "under ${MONITOUR_PACKAGES_DIR} after restore.")
endif()
list(GET _winml_pkg 0 WINML_PKG_DIR)

# Package layout (Microsoft.Windows.AI.MachineLearning 2.1.x):
#   include/winml/onnxruntime_cxx_api.h, ...
#   lib/native/x64/onnxruntime.lib, Microsoft.Windows.AI.MachineLearning.lib
#   runtimes/win-x64/native/onnxruntime.dll, Microsoft.Windows.AI.MachineLearning.dll, DirectML.dll
# Two header roots: include/ for the WinML EP catalog (WinMLEpCatalog.h),
# include/winml/ for the bundled ONNX Runtime C++ API.
set(WINML_INCLUDE_DIR      "${WINML_PKG_DIR}/include/winml")
set(WINML_ROOT_INCLUDE_DIR "${WINML_PKG_DIR}/include")
set(WINML_LIB_DIR          "${WINML_PKG_DIR}/lib/native/x64")
set(WINML_BIN_DIR          "${WINML_PKG_DIR}/runtimes/win-x64/native")

if(NOT EXISTS "${WINML_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "ONNX Runtime headers missing under ${WINML_INCLUDE_DIR}. "
        "Package layout may have changed; check ${WINML_PKG_DIR} contents.")
endif()

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
    src/overlay/FocusGlow.cpp
    src/util/ScopedMmcss.cpp
    src/util/Logging.cpp
)

add_executable(Monitour WIN32 ${MONITOUR_SOURCES})

target_include_directories(Monitour PRIVATE
    src
    "${WINML_INCLUDE_DIR}"
    "${WINML_ROOT_INCLUDE_DIR}"
)

target_link_directories(Monitour PRIVATE "${WINML_LIB_DIR}")

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
    # Runtime / WinRT (still needed for FaceAnalysis and base WinRT)
    windowsapp
    runtimeobject
    # Windows ML 2.x — two import libs shipped in the NuGet. onnxruntime.lib
    # forwards every ORT C API call; Microsoft.Windows.AI.MachineLearning.lib
    # provides the EpCatalog/loader entry points that bind ORT to the
    # Windows-Update-delivered execution providers.
    onnxruntime
    Microsoft.Windows.AI.MachineLearning
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

# Copy the FP16 model next to the exe for dev runs. The model is optional and
# not in the repo, so copy only if present (build must succeed without it).
add_custom_command(TARGET Monitour POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/models/6drepnet.fp16.onnx
        -DDST=$<TARGET_FILE_DIR:Monitour>/6drepnet.fp16.onnx
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_OUT}
        $<TARGET_FILE_DIR:Monitour>/Nv12ToTensor.cso
    # Copy the ONNX Runtime loader DLL from the WinML package next to the exe
    # so it can be found at runtime without modifying PATH.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${WINML_BIN_DIR}"
        $<TARGET_FILE_DIR:Monitour>
)

enable_testing()
add_subdirectory(tests)
