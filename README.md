# Monitour

Windows utility that watches your face via the webcam, infers which monitor you're looking at, and moves foreground focus to the most-recently-used window on that screen.

Native C++23 / C++/WinRT, Windows-only.

The app starts in passive mode until it learns your setup with enough confidence. Settings and calibration are persisted to disk on write but not yet re-read on startup — every launch begins with defaults.

Target latency: <20 ms median (NPU), <40 ms p99. Dwell delay 250 ms.

## Motivation

Do you find yourself having an agent working on one screen while doing something else on a different one? When the agent needs help, does switching to the terminal where the agent is running, type something, and then switching back result annoying? This app makes `yawing = alt + tab`. It's design to minimize disrumption and to run to the point it feels natural. It self-adjust and only actives when it got your setting. 

## How it learns

There is no calibration wizard. The app learns where each monitor sits in your head-pose space by watching what you actually do.

- **Seed from layout.** Windows already knows the physical monitor arrangement. Each monitor gets a wide, soft starting guess — rightmost screen → looking right, topmost → looking up. Just enough so the app isn't blind on the first launch.
- **Collect evidence passively.** A sample is recorded only when three things line up: the cursor is parked on a monitor, a real click or keystroke just landed there, and the face is detected with high confidence. That's the "the user is clearly looking at this screen right now" signal.
- **Model per monitor.** Each monitor keeps an independent Gaussian over head yaw and pitch, updated incrementally as evidence arrives.
- **Refuse to switch until mature.** Classification stays off until every monitor has accumulated enough samples and the learned means are clearly separated from each other. Until then, focus does not move automatically.
- **Classify and gate.** Once mature, each observation is assigned to the most-likely monitor, but only if the runner-up is clearly behind and the observation isn't an outlier to even the best monitor. That outlier check rejects "looking at the keyboard / phone / away from every screen" without any hardcoded angle limit — stacked, side-by-side, or mixed arrangements all work.
- **Suppress after input.** No focus changes for 1.5 s after a keystroke or click, so the app never yanks focus mid-action.

The model is persisted to `calibration.json` and a tray UI surfaces a 0–1 maturity score so you can see learning progress.

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
