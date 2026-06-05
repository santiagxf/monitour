# Monitour

Windows tray utility that watches your face via the webcam, infers which monitor you're looking at, and moves foreground focus to the most-recently-used window on that screen.

Native C++23 / C++/WinRT, Windows-only.

**Status: not functional yet.** The app builds, the tray icon appears, and the camera opens, but focus does not actually follow your gaze. The head-pose model, the face detector, and reading settings/calibration back from disk are still placeholders.

Target latency: <20 ms median (NPU), <40 ms p99. Dwell delay 250 ms.

## Build

From a **Developer PowerShell for VS 2022**:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

Run: `build\x64-release\Monitour.exe`

Presets also available: `x64-debug`, `arm64-release` (Snapdragon X).

## Models

Not in the repo. See `models/README.md` for export steps.

## Paths

- Settings: `%LOCALAPPDATA%\Monitour\settings.json`
- Calibration: `%LOCALAPPDATA%\Monitour\calibration.json`
- Logs (debug): `%LOCALAPPDATA%\Monitour\monitour.log`

## Hotkeys

- `Ctrl+Alt+M` — pause/resume
