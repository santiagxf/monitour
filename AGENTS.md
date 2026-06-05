# Monitour — agent instructions

Windows tray utility that infers which monitor the user faces (webcam head-pose) and moves foreground focus to the most-recently-used window on that screen. Native C++23 / C++/WinRT, Windows-only.

For project overview and run/settings/log paths see [README.md](README.md). For detailed architecture, the done-vs-stubbed status of every module, and the suggested order of work, see [HANDOFF.md](HANDOFF.md) — read it before changing pipeline code.

## Build & test

Run from a **Developer PowerShell for VS 2022** (the toolchain — `fxc.exe`, MSVC, the Windows SDK — must be on `PATH`):

```powershell
cmake --preset x64-release      # or x64-debug, arm64-release (Snapdragon X)
cmake --build --preset x64-release
ctest --preset x64-release      # or run build\x64-release\tests\Monitour_tests.exe
```

- Presets are in [CMakePresets.json](CMakePresets.json); they use the Ninja generator.
- Tests are Catch2, pulled via vcpkg. They are **skipped silently** if Catch2 isn't found — set `VCPKG_ROOT` + `CMAKE_TOOLCHAIN_FILE` (see [HANDOFF.md](HANDOFF.md)) to build them.
- The build compiles [shaders/Nv12ToTensor.hlsl](shaders/Nv12ToTensor.hlsl) with `fxc` and copies the `.cso` + ONNX models next to the exe. Models are not in the repo (see [models/README.md](models/README.md)); the app logs a warning and runs the rest of the pipeline without them.

## Conventions

- **C++23, MSVC only.** Builds fail on non-Windows / non-MSVC by design. Warnings are `/W4 /permissive-`; keep new code warning-clean.
- **Namespaces mirror directories:** `monitour::capture`, `monitour::focus`, `monitour::inference`, `monitour::d3d`, `monitour::calibration`, `monitour::input`, `monitour::tray`, `monitour::util`, `monitour::log`. Header guard is `#pragma once`.
- **HRESULT handling:** wrap every COM/Win32 HRESULT call in `MONITOUR_CHECK_HR(expr)` from [src/util/ComUtil.h](src/util/ComUtil.h); it throws `std::system_error`. Don't swallow failures silently.
- **Logging:** use the `std::format`-based wide-string helpers in [src/util/Logging.h](src/util/Logging.h) (`monitour::log::info/warn/error/debug`). Logs go to file + `OutputDebugString`.
- **COM/WinRT:** C++/WinRT is header-only via the Windows SDK; use `winrt::com_ptr` / RAII (`ScopedMmcss`) rather than manual `AddRef`/`Release`.
- New `.cpp` files must be added to both `MONITOUR_SOURCES` in [CMakeLists.txt](CMakeLists.txt) and, if unit-tested, the test target in [tests/CMakeLists.txt](tests/CMakeLists.txt).

## Pitfalls

- The pipeline is wired end-to-end but key pieces are intentionally stubbed (notably `HeadPoseModel::evaluate`, the face detector, and the JSON load paths). Each is marked `TODO` in code and explained in [HANDOFF.md](HANDOFF.md) — don't mistake a stub for a bug.
- Persisted `HMONITOR` values are **not stable across launches**; re-key by monitor device rect / `szDevice`, not the raw handle.
- This is real-time, latency-sensitive code (target <20 ms median). Avoid allocations and locking on the capture/inference hot path; prefer the existing single-slot `FrameSlot` pattern.
