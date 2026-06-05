# Monitour — agent instructions

Windows tray utility that infers which monitor the user faces (webcam head-pose) and moves foreground focus to the most-recently-used window on that screen. Native C++23 / C++/WinRT, Windows-only.

For project overview and run/settings/log paths see [README.md](README.md).

## Build & test

Run from a **Developer PowerShell for VS 2022** (the toolchain — `fxc.exe`, MSVC, the Windows SDK — must be on `PATH`):

```powershell
cmake --preset x64-release      # or x64-debug, arm64-release (Snapdragon X)
cmake --build --preset x64-release
ctest --preset x64-release      # or run build\x64-release\tests\Monitour_tests.exe
```

- Presets are in [CMakePresets.json](CMakePresets.json); they use the Ninja generator.
- Tests are Catch2, pulled via vcpkg. They are **skipped silently** if Catch2 isn't found — set `VCPKG_ROOT` + `CMAKE_TOOLCHAIN_FILE` to build them.
- The build compiles [shaders/Nv12ToTensor.hlsl](shaders/Nv12ToTensor.hlsl) with `fxc` and copies the `.cso` + ONNX models next to the exe. Models are not in the repo (see [models/README.md](models/README.md)); the app logs a warning and runs the rest of the pipeline without them.

## Tech choices — why, and what to preserve

Every choice below was made to hit **<20 ms median / <40 ms p99** on a typical webcam frame. If you swap any of them out, the budget almost certainly breaks. Read the rationale before reaching for a "simpler" alternative.

- **C++23 / C++/WinRT, no managed runtime.** No GC pauses, no JIT warmup, no marshalling cost to call Win32 / Media Foundation / Windows ML. C++/WinRT is header-only — projection is zero-cost vs. raw COM, but with RAII (`winrt::com_ptr`) instead of manual `AddRef`/`Release`. Don't introduce a .NET or Python sidecar for "convenience": each interop hop costs more than the inference itself.
- **Media Foundation `IMFSourceReader` (async) + `IMFDXGIDeviceManager` for zero-copy.** Camera frames land directly in a GPU `ID3D11Texture2D` — no CPU copy, no staging buffer round-trip. Switching to DirectShow or a CPU-side capture API would add a per-frame memcpy of an NV12 image (~MB/s) and cache thrash. Keep frames on the GPU end-to-end.
- **NV12 → planar CHW fp16 via HLSL compute shader (`Nv12ToTensor.hlsl`).** Color conversion, resize, and ImageNet normalization happen in one GPU pass writing straight into the model's input buffer. Doing this on the CPU (or as three separate passes) would burn most of the latency budget on preprocessing. If you change the model input layout, update the shader — don't add a CPU fixup step.
- **Single-slot latest-frame (`FrameSlot`), SRWLock-guarded.** Real-time means "always work on the freshest frame, drop the rest." A queue would build a tail of stale frames during transient GPU stalls and inflate p99. Do **not** replace `FrameSlot` with a ring buffer or `std::queue`.
- **Windows ML with `LearningModelDeviceKind::DirectXHighPerformance`.** Routes to the NPU when present (Copilot+ PCs), DirectML on the iGPU/dGPU otherwise, CPU as a last resort — without us writing three code paths. ONNX Runtime EPs would work but duplicate that routing logic. The model ships as **INT8 for NPU** and **FP16 for DirectML** because that's what each backend executes natively; FP32 forces fallback to a slower path.
- **6DRepNet (MIT) for head pose.** Single small CNN, direct 3×3 rotation-matrix regression — no landmark step, no multi-stage pipeline. A landmark-based approach (e.g. MediaPipe) adds a detector + tracker + PnP solve, each with its own latency tail. Stay with a one-shot regressor.
- **Windows.Media.FaceAnalysis (`FaceDetector`) for the crop.** It's the OS-native detector — accelerated on supported hardware and already on disk. Pulling in a separate face model (BlazeFace, RetinaFace) doubles the inference cost without improving the crop for a single-user webcam.
- **MMCSS (`AvSetMmThreadCharacteristics` "Pro Audio"/"Capture") on capture and inference threads.** Tells the Windows scheduler this thread is latency-critical; otherwise a background process can preempt us and spike p99 by tens of ms. The `ScopedMmcss` RAII wrapper is mandatory on any new hot-path thread.
- **`SetWinEventHook(EVENT_SYSTEM_FOREGROUND, ..., WINEVENT_OUTOFCONTEXT)` for MRU tracking.** Out-of-context = OS delivers events to our message loop without injecting a DLL into every process. In-context hooks would be faster per-event but require a separate DLL and break under UAC boundaries. Stay out-of-context.
- **`WH_KEYBOARD_LL` + `WH_MOUSE_LL` for input gating.** Low-level hooks are the only way to observe global input without focus. They have a ~300 ms timeout — the callback must return fast; do not block or call back into the pipeline from it.
- **Raw Win32 (`Shell_NotifyIcon`, `RegisterHotKey`) for tray + hotkey.** Adding XAML / WinUI for the tray would pull in the Windows App SDK runtime and a UI thread message pump we don't need. The tray is twelve calls; keep it that way.
- **Per-monitor Gaussian + EMA (α = 0.05) in `PassiveCalibrator`.** Constant-time update, constant memory, no training loop. A neural classifier would need labeled data we don't have and per-user fine-tuning we won't ship. The Gaussian is the right altitude for the problem.

If a future task requires changing one of these, call it out in the commit message and update both this section and the latency target in [README.md](README.md).

## Conventions

- **C++23, MSVC only.** Builds fail on non-Windows / non-MSVC by design. Warnings are `/W4 /permissive-`; keep new code warning-clean.
- **Namespaces mirror directories:** `monitour::capture`, `monitour::focus`, `monitour::inference`, `monitour::d3d`, `monitour::calibration`, `monitour::input`, `monitour::tray`, `monitour::util`, `monitour::log`. Header guard is `#pragma once`.
- **HRESULT handling:** wrap every COM/Win32 HRESULT call in `MONITOUR_CHECK_HR(expr)` from [src/util/ComUtil.h](src/util/ComUtil.h); it throws `std::system_error`. Don't swallow failures silently.
- **Logging:** use the `std::format`-based wide-string helpers in [src/util/Logging.h](src/util/Logging.h) (`monitour::log::info/warn/error/debug`). Logs go to file + `OutputDebugString`.
- **COM/WinRT:** C++/WinRT is header-only via the Windows SDK; use `winrt::com_ptr` / RAII (`ScopedMmcss`) rather than manual `AddRef`/`Release`.
- New `.cpp` files must be added to both `MONITOUR_SOURCES` in [CMakeLists.txt](CMakeLists.txt) and, if unit-tested, the test target in [tests/CMakeLists.txt](tests/CMakeLists.txt).

## Pitfalls

- The capture → preprocess → inference → focus pipeline is functional. The remaining `TODO`s are the JSON **load** paths in `src/tray/Settings.cpp` and `src/calibration/PassiveCalibrator.cpp` — writes work, reads fall back to defaults. Don't mistake those `TODO`s for broken inference.
- Persisted `HMONITOR` values are **not stable across launches**; re-key by monitor device rect / `szDevice`, not the raw handle.
- This is real-time, latency-sensitive code. Current target: **<20 ms median (NPU), <40 ms p99**, plus a 250 ms dwell delay (see README.md). Avoid allocations and locking on the capture/inference hot path; prefer the existing single-slot `FrameSlot` pattern.
- **Whenever a change affects the pipeline's latency profile** (e.g. adds work to capture/preprocess/inference/focus, changes the inference path, alters the dwell delay, or measurably shifts the budget), update the latency target in [README.md](README.md) — and here if the numbers in this section change — in the same commit. Don't let the documented target drift from reality.
