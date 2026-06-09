# Monitour

Windows utility that watches your face via the webcam, infers which monitor you're looking at, and moves foreground focus to the most-recently-used window on that screen.

Native C++23 / C++/WinRT, Windows-only.

The app starts in passive mode until it learns your setup with enough confidence. Settings and calibration are persisted to disk on write but not yet re-read on startup — every launch begins with defaults.

Target latency: <20 ms median (NPU), <40 ms p99. Dwell delay 250 ms.

## Motivation

This app makes `alt + tab = yawing`, that's all it takes to send the focus to another screen. It's design to minimize disrumption and to run to the point it feels natural - like moving the cursor with your eyes. It self-adjust and only actives when it got your setting. 

## How it learns

The app learns where each monitor sits in your head-pose space by watching what you actually do. There is no calibration wizard.

- **Geometric prior from layout.** Windows already knows the physical arrangement, so each screen gets a permanent prior with the angular center *and* angular footprint of the panel as seen from ~60 cm away — a wide screen close to your face gets a wide prior, a small laptop a narrow one. The prior's weight decays relative to evidence as you use the app, but never disappears, so the wide-vs-narrow asymmetry keeps disambiguating overlapping screens forever.
- **Collect evidence passively.** A sample is recorded when a real click or keystroke lands on a known monitor and the face is detected with high confidence — "the user is clearly looking at this screen right now."
- **Teach hotkey for asymmetric layouts (Ctrl+Alt+L).** Park the cursor on the screen you want to teach, press the hotkey, look at it for a second or two, press again. Every confident pose tick is labelled with that screen — much higher SNR than one sample per click, and the only way to teach screens you don't click on often.
- **Model per monitor.** Each monitor keeps a 2-D histogram over (yaw, pitch). New samples deposit a small Gaussian splat into the grid so single clicks generalize without averaging away.
- **Refuse to switch until mature.** Classification stays off until every monitor has accumulated enough samples. Until then, focus does not move automatically.
- **Classify and gate.** Once mature, each observation is assigned to the most-likely monitor (histogram + weighted prior), but only if the runner-up is clearly behind and the observation lies within a few σ of the best screen's prior — σ being the screen's own angular half-extent, so the gate scales naturally with screen size. That rejects "looking at the keyboard / phone / away from every screen" with no hardcoded angle limit — stacked, side-by-side, or asymmetric arrangements all work.
- **Suppress after input.** No focus changes for 1.5 s after a keystroke or click, so the app never yanks focus mid-action.

The model is persisted to `calibration.json` and a tray UI surfaces a 0–1 maturity score so you can see learning progress.

## Build

Requires **Windows 11 24H2** or newer (NPU execution providers ship in that build). Build from a **Developer PowerShell for VS 2022** with `nuget.exe` on `PATH`:

```powershell
$id = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property instanceId
Import-Module "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -InstanceId $id -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

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
- Logs: `%LOCALAPPDATA%\Monitour\monitour.log`
- NPU compile cache: `%LOCALAPPDATA%\Monitour\ov_cache\` (delete this if you replace the model file)

## Hotkeys

- `Ctrl+Alt+M` — pause/resume
