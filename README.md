# Monitour

Webcam-driven focus follower for multi-monitor Windows. Looks at your face, infers which monitor you're facing via head-pose, and moves foreground focus to the most-recently-used window on that screen — so your keystrokes always land where you're looking.

Target latency: <20 ms median (NPU), <40 ms p99. Dwell delay 250 ms.

## Status

Scaffolding stage. The architecture is in `plans/` and stub modules are in place; the hot-loop, calibrator, and focus logic are TODOs called out in each file.

## Build prerequisites (Windows)

- Windows 11 22H2 or later (24H2+ recommended for Windows ML)
- Windows App SDK 1.7 runtime
- Visual Studio 2022 17.10+ with **Desktop development with C++**, **C++/WinRT**, **Windows 11 SDK (10.0.22621.0+)**
- CMake 3.27+, Ninja
- A webcam, two or more monitors, and (ideally) a Copilot+ PC with an NPU

## Build

From a **Developer PowerShell for VS 2022**:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

Run: `build\x64-release\Monitour.exe`

## Models

The `models/` directory is empty in the repo. See `models/README.md` for the export steps to produce `6drepnet.int8.onnx` (NPU path) and `6drepnet.fp16.onnx` (DirectML fallback).

## Settings & logs

- Settings: `%LOCALAPPDATA%\Monitour\settings.json`
- Calibration: `%LOCALAPPDATA%\Monitour\calibration.json`
- Logs (debug build): `%LOCALAPPDATA%\Monitour\monitour.log`

## Hotkeys

- `Ctrl+Alt+M` — pause/resume focus following
