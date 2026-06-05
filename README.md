# Monitour

Windows tray utility that watches your face via the webcam, infers which monitor you're looking at, and moves foreground focus to the most-recently-used window on that screen.

Native C++23 / C++/WinRT, Windows-only. **Scaffolding stage** — pipeline is wired end-to-end, but the head-pose model, face detector, and JSON load paths are stubbed.

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
