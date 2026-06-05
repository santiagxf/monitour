# Monitour

Windows tray utility that watches your face via the webcam, infers which monitor you're looking at, and moves foreground focus to the most-recently-used window on that screen.

Native C++23 / C++/WinRT, Windows-only.

The app starts in passive mode until it learns your setup with enough confidence. Settings and calibration are persisted to disk on write but not yet re-read on startup — every launch begins with defaults.

Target latency: <20 ms median (NPU), <40 ms p99. Dwell delay 250 ms.

## How it learns your setup

There is no calibration wizard. The app learns where each monitor sits in your head-pose space by watching what you actually do.

- **Seed from layout.** At startup, Windows already knows the physical monitor arrangement. Each monitor gets a wide soft prior — rightmost screen → positive yaw, topmost → positive pitch (looking up) — with ~15° stddev. This is a guess, not a commitment; it stops the app from being completely blind on the first launch.
- **Collect evidence passively.** A sample is recorded only when all of these hold at once: the cursor has been parked on a monitor for ≥500 ms, a real click or keystroke just landed there, and the face is detected with high confidence. That triple-gate is the "the user is clearly looking at this screen right now" signal.
- **Model per monitor.** Each monitor keeps an independent Gaussian over (yaw, pitch), updated with an EMA (α = 0.05). Variance is floored at 2° so a few unusually still moments can't collapse it.
- **Refuse to switch until mature.** Classification stays off until every monitor has ≥30 real samples AND the closest pair of means is ≥8° apart in the combined yaw+pitch plane. Until then, focus does not move automatically.
- **Classify and gate.** Once mature, an observation is assigned by argmax log-likelihood across the per-monitor Gaussians, but only if (a) the runner-up is at least 0.7 nats behind and (b) the observation is within ~4 σ of the winner. The second check rejects "looking at the keyboard / phone / away from every screen" without any hardcoded angle limit — stacked, side-by-side, or mixed arrangements all work.
- **Suppress after input.** No focus changes in the 1.5 s after a keystroke or click, so the app never yanks focus mid-action.

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
