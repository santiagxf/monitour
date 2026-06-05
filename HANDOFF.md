# Monitour — handoff notes

State of the repo as of the end of the WSL session, written to make it easy to resume on Windows.

## What Monitour is

A Windows tray utility that watches the user through the laptop webcam, infers which monitor they're looking at via head-pose estimation, and moves foreground focus to the most-recently-used window on that monitor — so keystrokes always land on the screen the user is facing.

Latency target: **<20 ms median (NPU path), <40 ms p99**, plus a deliberate 250 ms dwell delay.

The full plan, with rationale for every choice, lives at:
`~/.claude/plans/i-wan-to-create-linear-giraffe.md` (WSL) — copy it into the repo if useful.

## Tech stack (decided)

| Layer | Choice |
|---|---|
| Language | C++23, C++/WinRT (header-only) |
| Build | CMake 3.27+ / Ninja / MSVC 2022 / vcpkg |
| Camera | Media Foundation `IMFSourceReader` async, zero-copy via `IMFDXGIDeviceManager` |
| Preprocess | HLSL CS 5.0 compute shader (NV12 → planar CHW fp16, ImageNet-normalized) |
| Inference | Windows ML (`Windows.AI.MachineLearning`), `LearningModelDeviceKind::DirectXHighPerformance` for NPU-preferred routing, with DirectX / CPU fallbacks |
| Model | 6DRepNet (MIT) — INT8 for NPU, FP16 for DirectML fallback |
| Window control | Win32 `SetForegroundWindow` + `AttachThreadInput` workaround, `TOPMOST` flicker as backup |
| MRU per monitor | `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, ..., WINEVENT_OUTOFCONTEXT)` |
| Input gating | `WH_KEYBOARD_LL` + `WH_MOUSE_LL` low-level hooks |
| Thread scheduling | MMCSS (`AvSetMmThreadCharacteristics`) on capture + inference threads |
| Tray | Raw `Shell_NotifyIcon` |
| Hotkey | `RegisterHotKey` — `Ctrl+Alt+M` toggles pause |
| Persistence | JSON in `%LOCALAPPDATA%\Monitour\` |

## Behavioral decisions (already baked in)

- **Trigger:** dwell-based (≈250 ms steady gaze) auto-switch, plus the pause hotkey.
- **Window choice on switch:** per-monitor most-recently-used (tracked by the foreground hook).
- **Calibration:** pure passive — Gaussian per monitor over yaw, updated only on high-confidence cursor evidence (cursor parked ≥500 ms + a real click/keystroke on that monitor + face confident). No prompts. Classifier refuses to switch until every monitor has ≥30 samples AND closest pair of means is ≥8° apart.
- **Suppress-after-input:** no focus changes for 1.5 s after the user types or clicks.
- **Face lost:** hold current focus + throttle inference to ~1 FPS (the capture session exposes `setInterFrameDelayMs`).

## Repository layout

```
monitour/
├── CMakeLists.txt                         # build + shader compile + model copy
├── CMakePresets.json                      # x64-debug | x64-release | arm64-release
├── vcpkg.json                             # Catch2 (tests only)
├── README.md
├── HANDOFF.md                             # ← this file
├── models/
│   ├── README.md                          # how to export 6DRepNet to ONNX + quantize
│   └── .gitkeep                           # actual .onnx files NOT in repo
├── shaders/
│   └── Nv12ToTensor.hlsl                  # NV12 → planar RGB fp16 CHW
├── src/
│   ├── main.cpp                           # WinMain, wires everything
│   ├── tray/
│   │   ├── TrayIcon.{h,cpp}               # Shell_NotifyIcon + context menu
│   │   └── Settings.{h,cpp}               # JSON write (load = stub)
│   ├── capture/
│   │   ├── FrameSlot.{h,cpp}              # single-slot SRWLock latest-frame
│   │   └── MfCaptureSession.{h,cpp}       # async IMFSourceReader → ID3D11Texture2D
│   ├── inference/
│   │   ├── HeadPose.h                     # yaw/pitch/roll/confidence POD
│   │   ├── DeviceFactory.{h,cpp}          # NPU → DirectX → CPU selection + logging
│   │   └── HeadPoseModel.{h,cpp}          # session + binding (evaluate is STUB)
│   ├── focus/
│   │   ├── WindowEnumerator.{h,cpp}       # candidates, elevation check
│   │   ├── MonitorMru.{h,cpp}             # EVENT_SYSTEM_FOREGROUND hook + per-HMONITOR stacks
│   │   └── ForegroundSetter.{h,cpp}       # AttachThreadInput dance, TOPMOST backup
│   ├── input/
│   │   └── ActivityHooks.{h,cpp}          # WH_*_LL hooks, cursor-parked tracking
│   ├── calibration/
│   │   └── PassiveCalibrator.{h,cpp}      # per-monitor Gaussian, ambiguity-margin classifier
│   ├── d3d/
│   │   ├── D3DContext.{h,cpp}             # shared D3D11 device + IMFDXGIDeviceManager
│   │   └── Nv12ToTensor.{h,cpp}           # loads CSO, dispatches NV12 → tensor
│   └── util/
│       ├── ComUtil.h                      # MONITOUR_CHECK_HR
│       ├── Logging.{h,cpp}                # std::format → file + OutputDebugString
│       └── ScopedMmcss.{h,cpp}            # RAII MMCSS opt-in
└── tests/
    ├── CMakeLists.txt                     # Catch2 via vcpkg
    └── PassiveCalibrator_tests.cpp        # 6 cases covering classifier behavior
```

## Status — what's done vs. what's still stubbed

### Done (real code, should compile and run)

- **Build glue:** CMake + presets + shader compile step (fxc → `.cso`) + model-copy step. ARM64 preset for Snapdragon X.
- **D3D + MF setup:** shared D3D11 device with `VIDEO_SUPPORT`, multi-thread protection, `IMFDXGIDeviceManager` reset — Media Foundation will hand back GPU textures.
- **Camera capture:** enumerates the default video device, picks the best NV12 media type (closest fps to 60, largest resolution ≤ preferred), drives it asynchronously via `IMFSourceReaderCallback`, publishes each frame as `ID3D11Texture2D` + subresource + QPC timestamp to a single-slot SRWLock holder.
- **HLSL compute shader:** NV12 (Y + UV planes) → BT.601 limited→full → ImageNet-normalized → planar CHW fp16, with center-square crop + bilinear resize from a face box (or full-frame fallback).
- **Inference scaffolding:** `LearningModelDevice` selection with logged fallback chain, model loading via `StorageFile`, batch override, reusable `LearningModelBinding`.
- **Window enumeration + MRU:** candidate filter (visible, non-tool, non-cloaked, non-shell), elevation check via `OpenProcessToken`, per-`HMONITOR` MRU stacks updated from a real `SetWinEventHook` (also primes from current Z-order at startup, rebuilds on `WM_DISPLAYCHANGE`).
- **Foreground setter:** `AttachThreadInput` trick + `AllowSetForegroundWindow` + `BringWindowToTop`, with `HWND_TOPMOST` flicker as a fallback when AttachThreadInput is blocked. Refuses gracefully on elevated targets the process can't focus.
- **Input hooks:** WH_KEYBOARD_LL + WH_MOUSE_LL, tracking last-input QPC, cursor-monitor changes, and on-which-monitor the last input fell.
- **Passive calibrator:** EMA-updated per-monitor μ/σ, log-PDF classifier with ambiguity-margin gate, maturity gate (n ≥ minSamples AND closest mean separation ≥ minMeanSeparationDeg), unit-tested.
- **Tray + hotkey + message loop:** hidden message window, context menu, `Ctrl+Alt+M` global hotkey, `WM_DISPLAYCHANGE` handling, clean shutdown that joins worker thread.
- **MMCSS:** RAII wrapper applied to the inference worker thread.
- **Logging:** file + `OutputDebugString`, std::format-based.
- **Unit tests:** 6 Catch2 cases for the calibrator covering cold start, ambiguity, multi-monitor convergence, confidence gating.

### Stubbed / TODO (intentional)

These are clearly marked in the code as `TODO`:

1. **`src/inference/HeadPoseModel.cpp::evaluate`** — the cross-API D3D11→D3D12 texture-sharing bind via `ITensorNative::CreateFromD3D12Resource` is not written; it currently returns `confidence = 0`. This is the **largest single piece left**. Without it the pipeline runs end-to-end but the classifier never gets useful evidence. Expected shape:
   1. Open a shared NT handle on the preprocessed buffer (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE` is already set in `Nv12ToTensor`).
   2. Open it as a D3D12 resource on the LearningModelDevice's queue.
   3. Wrap via `ITensorNative::CreateFromD3D12Resource` → bind to `LearningModelBinding`.
   4. `EvaluateAsync` and decode the 6D rotation matrix output to yaw/pitch/roll (6DRepNet returns a 6D rotation, not Euler angles — use Gram-Schmidt + atan2).

2. **Face detector** — `Nv12ToTensor::Dispatch` is called with an empty face box and falls back to the centered square of the frame, which is brittle. Add a tiny detector (e.g. RFB-320 / Ultra-Light) running before 6DRepNet to produce a real `faceBox`.

3. **JSON load paths** — `Settings::load` and `PassiveCalibrator::load` are stubs returning `false`. The *save* paths produce valid JSON; pick any header-only parser (e.g. `nlohmann/json` already on vcpkg) and complete the read side. The calibrator load also needs a re-keying step: persisted `HMONITOR` bit patterns are not stable across launches; re-key by monitor device-rect identity (`EnumDisplayMonitors` + `MONITORINFOEXW::szDevice`).

4. **Per-FPS budget control** — `MfCaptureSession::setInterFrameDelayMs` exists but nothing currently calls it. Wire to face-lost / on-battery state from the orchestrator in `main.cpp`.

5. **`HeadPoseModel::evaluate` warning** — the parameter is intentionally unused in the stub; you'll get a `[[maybe_unused]]`-style warning until the real implementation lands. Not a bug.

6. **6DRepNet output decoding** — the model returns a 6D continuous rotation representation (Zhou et al. 2019). The decode is straightforward but needs to live in `HeadPoseModel.cpp`; reference Python: `https://github.com/thohemp/6DRepNet/blob/master/sixdrepnet/utils.py` (`compute_rotation_matrix_from_ortho6d`).

## Build prerequisites (Windows side)

- Windows 11 22H2 or newer (24H2+ for full Windows ML NPU routing)
- Visual Studio 2022 17.10+ — Desktop development with C++, **C++/WinRT**, **Windows 11 SDK 10.0.22621.0** or newer
- CMake 3.27+, Ninja (both come with the VS installer)
- vcpkg (only needed if you want the unit tests built — Catch2)
- Windows App SDK 1.7+ runtime
- A webcam and ≥2 monitors

## Build steps

From a **Developer PowerShell for VS 2022**, in the repo root:

```powershell
# Optional: connect vcpkg if you want to run the tests
$env:VCPKG_ROOT = "C:\vcpkg"
$env:CMAKE_TOOLCHAIN_FILE = "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"

cmake --preset x64-release
cmake --build --preset x64-release
```

Outputs: `build\x64-release\Monitour.exe` plus `Nv12ToTensor.cso` and the model files copied next to it.

## Models — get these in place before first run

Two ONNX variants belong in `models/`:

- `6drepnet.int8.onnx` — for the NPU path
- `6drepnet.fp16.onnx` — for the DirectML fallback

See `models/README.md` for the export + Olive quantization steps.

If models are missing, `main.cpp` logs a warning and runs the rest of the pipeline (capture + preprocess + decision plumbing) without inference — useful for verifying everything else first.

## Suggested order of work to resume on Windows

This order means you can verify each layer before depending on it:

1. **First clean build.** `cmake --preset x64-release && cmake --build --preset x64-release`. Expect a couple of unused-parameter warnings (see "TODO" notes above). Resolve any actual errors before moving on.
2. **Run with no model.** Should reach the message loop, register the hotkey, log "Capture: selected NV12 …", and the `EVENT_SYSTEM_FOREGROUND` hook should start populating MRU stacks. Verify in Task Manager that camera is "in use".
3. **Verify camera path alone.** Tiny debug helper: in `MfCaptureSession::OnReadSample`, save the first frame's `ID3D11Texture2D` to a PNG (use `DirectXTK`'s `SaveDDSTextureToFile` then convert, or staging-copy + `WIC`). Confirms the zero-copy NV12 path.
4. **Build models.** Follow `models/README.md`. Drop both ONNX files in `models/`, rebuild (or just copy them next to the exe).
5. **Implement `HeadPoseModel::evaluate`.** This is the chunky one. The D3D11→D3D12 sharing path is the canonical Windows ML zero-copy story; reference docs: `Microsoft.AI.MachineLearning.ITensorNative` + `IDXGIResource1::CreateSharedHandle`. Verify yaw output on a stable head pose by logging it for ~30 seconds.
6. **Add a face detector.** Smallest practical addition — without it, off-center webcam framing breaks 6DRepNet. A 256-KB RFB-320 ONNX runs in <1 ms on any iGPU.
7. **End-to-end loop.** Sit in front of the rig, look between monitors for ~5 minutes to seed the calibrator. After it matures (≥30 samples per monitor, ≥8° separation), it should start switching. Inspect `%LOCALAPPDATA%\Monitour\calibration.json` to confirm.
8. **Verification per the plan §Verification** — manual tests for dwell behavior, pause hotkey, face-lost throttle, display-change handling.

## Things deliberately *not* done yet

- No installer / signing — `Monitour.exe` runs from wherever it's built.
- No telemetry, no auto-update.
- No multi-camera support (uses default `IMFActivate`).
- No autostart shortcut creation — add a Startup-folder `.lnk` when you're happy with the v1.

## Files worth re-reading first when you sit down on Windows

In rough order of importance for getting unstuck:

- `src/main.cpp` — the wiring; everything else hangs off it.
- `src/inference/HeadPoseModel.cpp` — where the largest TODO lives.
- `src/d3d/Nv12ToTensor.{h,cpp}` and `shaders/Nv12ToTensor.hlsl` — the GPU bridge between camera and model.
- `src/calibration/PassiveCalibrator.cpp` — the only algorithmically subtle piece; unit tests should pass.
- `src/focus/ForegroundSetter.cpp` — the fragile-but-known Win32 trick; expect to iterate here when you hit a window it can't focus.

Good luck on the other side. The whole pipeline is wired; the model bind is the real cliff to climb.
