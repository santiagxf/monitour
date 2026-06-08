// Monitour — webcam-driven focus follower
//
// WinMain wires together capture → inference → decision → focus. The capture
// and inference threads run with MMCSS scheduling. The main thread runs the
// Win32 message loop for tray, global hotkey, SetWinEventHook callbacks, and
// the low-level input hooks.

#include <Windows.h>
#include <mfapi.h>
#include <shellapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

#include "calibration/PassiveCalibrator.h"
#include "capture/FrameSlot.h"
#include "capture/MfCaptureSession.h"
#include "d3d/D3DContext.h"
#include "d3d/Nv12ToTensor.h"
#include "focus/ForegroundSetter.h"
#include "focus/MonitorLayout.h"
#include "focus/MonitorMru.h"
#include "inference/DeviceFactory.h"
#include "inference/HeadPoseModel.h"
#include "inference/FaceDetector.h"
#include "input/ActivityHooks.h"
#include "overlay/DebugOverlay.h"
#include "overlay/FocusGlow.h"
#include "tray/Settings.h"
#include "tray/TrayIcon.h"
#include "util/Logging.h"
#include "util/ScopedMmcss.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// The debug stats overlay is compiled and shown only in Debug builds.
#if !defined(NDEBUG)
#define MONITOUR_DEBUG_OVERLAY 1
#endif

namespace {

constexpr UINT kHotkeyId = 1;
constexpr UINT kModelInputSize = 224;

std::atomic<bool> g_quit{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_focusGlowEnabled{true};

}  // namespace

static int wmain_inner() {
    using namespace std::chrono_literals;
    using namespace monitour;

    auto dataDir = tray::appDataDir();
    log::init((dataDir / L"monitour.log").wstring());
    log::info(L"Monitour starting");

    tray::Settings settings;
    tray::load(settings, dataDir / L"settings.json");
    g_focusGlowEnabled.store(settings.focusGlowEnabled);

    // --- D3D + Media Foundation ---
    MFStartup(MF_VERSION);

    d3d::D3DContext d3d;
    capture::FrameSlot slot;
    capture::CaptureConfig cfg;
    cfg.preferredFps = settings.activeFps;
    capture::MfCaptureSession capture{d3d, slot, cfg};

    // --- Inference paths ---
    // CMake POST_BUILD copies models and the compiled shader next to the exe,
    // so the exe directory is the source of truth at runtime. The
    // repo-relative fallback lets a dev run from the source tree without
    // copying models around.
    std::filesystem::path exeDir;
    {
        wchar_t exeBuf[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
            exeDir = std::filesystem::path{exeBuf}.parent_path();
        }
        if (exeDir.empty()) exeDir = std::filesystem::current_path();
    }
    std::filesystem::path modelDir = exeDir;
    if (!std::filesystem::exists(modelDir / L"6drepnet.fp16.onnx")) {
        auto repoModels = exeDir / L".." / L"models";
        if (std::filesystem::exists(repoModels / L"6drepnet.fp16.onnx")) {
            modelDir = std::filesystem::weakly_canonical(repoModels);
        }
    }
    const auto modelPath = inference::modelPathFor(modelDir);
    const bool modelPresent = std::filesystem::exists(modelPath);
    if (!modelPresent) {
        log::warn(L"Model file missing: {} — running without inference.",
                  modelPath.wstring());
    }

    // OpenVINO NPU compile cache — survives across launches. First run pays
    // the multi-second NPU compile; subsequent runs hit the cache.
    const auto cacheDir = dataDir / L"ov_cache";

    d3d::Nv12ToTensor preprocessor{
        d3d,
        exeDir / L"Nv12ToTensor.cso",
        kModelInputSize,
    };

    // --- Focus, MRU, input hooks ---
    focus::MonitorMru mru;
    mru.start();

    input::ActivityHooks hooks;
    hooks.install();

    calibration::PassiveCalibrator calibrator;
    calibrator.load(dataDir / L"calibration.json");

    // Auto-detect the physical monitor arrangement from Windows and seed soft
    // (yaw,pitch) priors from each screen's position: screens to the right get a
    // positive yaw prior, screens higher up a positive pitch prior. This only
    // seeds; it never biases classification, which stays data-driven.
    {
        const auto layout = focus::enumerateMonitorsLeftToRight();
        log::info(L"Detected {} monitor(s), left→right:", layout.size());

        // Bounding box of monitor centers, so we can normalise each center to
        // [-1,1] on each axis and fan the priors across ±kFanHalfDeg. With a
        // single monitor the box is degenerate and everything maps to 0°.
        LONG minCx = (std::numeric_limits<LONG>::max)();
        LONG maxCx = (std::numeric_limits<LONG>::min)();
        LONG minCy = (std::numeric_limits<LONG>::max)();
        LONG maxCy = (std::numeric_limits<LONG>::min)();
        for (const auto& m : layout) {
            const LONG cx = (m.rect.left + m.rect.right) / 2;
            const LONG cy = (m.rect.top + m.rect.bottom) / 2;
            minCx = (std::min)(minCx, cx); maxCx = (std::max)(maxCx, cx);
            minCy = (std::min)(minCy, cy); maxCy = (std::max)(maxCy, cy);
        }
        const double spanX = static_cast<double>(maxCx - minCx);
        const double spanY = static_cast<double>(maxCy - minCy);
        constexpr double kFanHalfDeg = 25.0;

        std::vector<calibration::PassiveCalibrator::LayoutHint> hints;
        hints.reserve(layout.size());
        for (size_t i = 0; i < layout.size(); ++i) {
            const auto& m = layout[i];
            log::info(L"  [{}] {} [{},{} {}x{}]{}", i + 1, m.device, m.rect.left,
                      m.rect.top, m.rect.right - m.rect.left,
                      m.rect.bottom - m.rect.top, m.primary ? L" (primary)" : L"");

            const LONG cx = (m.rect.left + m.rect.right) / 2;
            const LONG cy = (m.rect.top + m.rect.bottom) / 2;
            // Normalise to [-1,1]; degenerate span → 0 (centered).
            const double nx = spanX > 0.0 ? (2.0 * (cx - minCx) / spanX - 1.0) : 0.0;
            const double ny = spanY > 0.0 ? (2.0 * (cy - minCy) / spanY - 1.0) : 0.0;

            calibration::PassiveCalibrator::LayoutHint h{};
            h.monitor  = m.handle;
            h.yawDeg   = nx * kFanHalfDeg;   // right screen → look right → +yaw
            h.pitchDeg = -ny * kFanHalfDeg;  // lower screen (larger y) → look down → −pitch
            hints.push_back(h);
        }
        calibrator.seedLayout(hints);
    }

#if defined(MONITOUR_DEBUG_OVERLAY)
    // Debug-only: a small semi-transparent stats box in the top-left corner.
    // Created hidden; the tray menu's "Show debug stats" item reveals it.
    overlay::DebugOverlay debugOverlay;
    overlay::DebugOverlay* overlay = &debugOverlay;
    if (!debugOverlay.create(GetModuleHandleW(nullptr))) {
        log::warn(L"DebugOverlay::create failed; continuing without overlay.");
        overlay = nullptr;
    }
#endif

    // Focus-change confirmation: brief magenta→violet glow on the border of
    // the monitor that just received Monitour-driven focus.
    overlay::FocusGlow focusGlow;
    overlay::FocusGlow* glow = &focusGlow;
    if (!focusGlow.create(GetModuleHandleW(nullptr))) {
        log::warn(L"FocusGlow::create failed; continuing without focus glow.");
        glow = nullptr;
    }

    // --- Tray + hotkey ---
    tray::TrayIcon tray;
    tray::TrayIcon::Callbacks trayCb{
        .onTogglePause = []{
            bool was = g_paused.exchange(!g_paused.load());
            log::info(L"Pause toggled: {} -> {}", was, !was);
        },
        .onQuit = []{ g_quit.store(true); PostQuitMessage(0); },
        .isPaused = []{ return g_paused.load(); },
    };
#if defined(MONITOUR_DEBUG_OVERLAY)
    if (overlay) {
        trayCb.onToggleDebugStats = [overlay]{
            overlay->setVisible(!overlay->isVisible());
        };
        trayCb.isDebugStatsVisible = [overlay]{ return overlay->isVisible(); };
    }
#endif
    if (glow) {
        trayCb.onToggleFocusGlow = [&settings]{
            const bool now = !g_focusGlowEnabled.load();
            g_focusGlowEnabled.store(now);
            settings.focusGlowEnabled = now;
            log::info(L"Focus glow toggled: {}", now ? L"on" : L"off");
        };
        trayCb.isFocusGlowEnabled = []{ return g_focusGlowEnabled.load(); };
    }
    tray.create(GetModuleHandleW(nullptr), std::move(trayCb));

    if (!RegisterHotKey(nullptr, kHotkeyId,
                        settings.hotkeyModifiers, settings.hotkeyVk)) {
        log::warn(L"RegisterHotKey failed: {}", GetLastError());
    }

    // --- Worker threads ---

    // Inference worker: blocks on FrameSlot, preprocesses on D3D, evaluates
    // the model, classifies, dwells, then dispatches a focus command back to
    // the main thread (via SendMessage to the tray window for serialization).
    std::thread inferThread{[&]{
        // MTA so any WinRT call from FaceDetector (still used for cropping)
        // doesn't try to marshal back to the main STA.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // "Capture" — not "Pro Audio". Pro Audio is reserved for sub-10ms
        // realtime audio; using it here pegged the inference thread above the
        // desktop compositor and the LowLevelHook timeout (~300ms), making
        // every keystroke and mouse move freeze for the duration of an
        // evaluate. "Capture" still tells the scheduler this is latency-
        // sensitive without starving the rest of the system.
        util::ScopedMmcss mmcss{L"Capture"};

        // Minimum interval between inference calls. 30 fps × full evaluate is
        // overkill for human-reaction head pose, and on the GPU/DML fallback
        // path it backpressures the desktop. ~12 Hz keeps the calibrator fed
        // and dwell logic responsive while leaving headroom on the device.
        constexpr auto kMinInferInterval = 80ms;
        // Subtract the interval so the first iteration always passes the
        // throttle. Using time_point::min() here overflows the duration
        // arithmetic (steady_clock uses int64 nanoseconds) — the difference
        // wraps to a tiny / negative value, the throttle check then always
        // succeeds, and every captured frame is silently dropped.
        auto lastInferAt = std::chrono::steady_clock::now() - kMinInferInterval;

        // Build the head-pose model on this (MTA) thread. First-run NPU
        // compile can take many seconds; the OpenVINO cache_dir we passed to
        // makeSessionOptions makes subsequent launches near-instant.
        std::unique_ptr<inference::HeadPoseModel> model;
        if (modelPresent) {
            try {
                model = std::make_unique<inference::HeadPoseModel>(
                    d3d, modelPath, inference::DeviceChoice::Auto,
                    cacheDir, kModelInputSize);
            } catch (std::exception const& e) {
                const std::string what = e.what();
                log::error(L"HeadPoseModel load failed: {} — running without "
                           L"inference.",
                           std::wstring(what.begin(), what.end()));
            }
        }

        HMONITOR lastTarget = nullptr;
        auto lastTargetSince = std::chrono::steady_clock::now();
        HMONITOR lastFlashed = nullptr;  // suppresses re-flash on stable target
        bool wasActive = false;
        int lastPct = -1;
        uint64_t lastInputSeq = 0;

        // Cheap face detection (Windows built-in, CPU). The cadence adapts to
        // head motion: detect often mid-turn so the crop tracks the moving head
        // (keeping the pose accurate exactly when the user is switching
        // screens), and back off when settled to save CPU. Between detections
        // the last box is cached and reused until it goes stale.
        inference::FaceDetector faceDet;
        const bool faceDetReady = faceDet.create();
        constexpr auto kFaceIntervalIdle = 250ms;   // ~4 Hz when settled
        constexpr auto kFaceIntervalFast = 50ms;    // ~20 Hz mid-turn
        constexpr auto kFaceTtl          = 1500ms;  // cached box validity window
        auto lastFaceDetect = std::chrono::steady_clock::now() - kFaceIntervalIdle;
        auto faceSeenAt     = std::chrono::steady_clock::time_point::min();
        RECT cachedFace{};
        bool faceValid = false;

        // Head angular velocity (deg/s, smoothed) drives predictive switching:
        // a settled gaze commits quickly while a fast sweep defers, so focus
        // doesn't latch onto a monitor the head is merely panning across.
        double velEmaDegPerSec = 0.0;
        double yawPrev = 0.0, pitchPrev = 0.0;
        auto   poseTimePrev = std::chrono::steady_clock::now();
        bool   havePrevPose = false;
        constexpr double kSettledVelDeg = 20.0;  // at/below: fully settled
        constexpr double kTransitVelDeg = 70.0;  // at/above: still sweeping
        constexpr auto   kDwellSettled  = 70ms;  // dwell once the gaze settles

        // --- Diagnostics: explain *why* learning is or isn't progressing ---
        const bool inferenceAvailable = (model != nullptr);
        uint64_t evidenceAttempts = 0;   // input events seen while running
        uint64_t evidenceAccepted = 0;   // samples that passed the gates
        std::wstring lastDetail;         // last tooltip 2nd line (de-dupe posts)
        auto lastDiagLog = std::chrono::steady_clock::now();

        // Human-readable reason convergence is stalled, or nullptr when the
        // calibrator is genuinely making progress. Ordered by what the user
        // should fix first.
        auto stuckReason = [&](const calibration::PassiveCalibrator::MaturityProgress& p)
            -> const wchar_t* {
            if (!inferenceAvailable)
                return L"No head-tracking model — learning paused (see models\\README)";
            if (evidenceAttempts == 0)
                return L"Click or type on each screen so I can learn your setup";
            if (evidenceAccepted == 0)
                return L"No face detected — center your face in the webcam view";
            if (p.monitorsSeen < 2)
                return L"Use your other screen(s) too so I can tell them apart";
            return nullptr;  // making progress normally
        };

        // Worker heartbeat — every 5s log how many frames we took, dropped,
        // and how many wait-timeouts we hit. A pile of timeouts with zero
        // taken frames means capture isn't publishing.
        uint64_t framesTaken = 0;
        uint64_t framesTimeout = 0;
        uint64_t framesDroppedThrottle = 0;
        auto lastHeartbeat = std::chrono::steady_clock::now();
        constexpr auto kHeartbeatEvery = 5s;

        while (!g_quit.load(std::memory_order_acquire)) {
            const auto hbNow = std::chrono::steady_clock::now();
            if (hbNow - lastHeartbeat >= kHeartbeatEvery) {
                lastHeartbeat = hbNow;
                log::info(L"Worker heartbeat: taken={} timeouts={} "
                          L"throttled={} (paused={})",
                          framesTaken, framesTimeout, framesDroppedThrottle,
                          g_paused.load() ? L"true" : L"false");
            }

            capture::Frame f;
            if (!slot.waitAndTake(f, 200ms)) { ++framesTimeout; continue; }
            ++framesTaken;

            if (g_paused.load(std::memory_order_acquire)) continue;

            // Drop frames that arrive faster than kMinInferInterval. The
            // FrameSlot only keeps the freshest one so dropping here is safe.
            const auto loopNow = std::chrono::steady_clock::now();
            if (loopNow - lastInferAt < kMinInferInterval) {
                ++framesDroppedThrottle;
                continue;
            }
            lastInferAt = loopNow;

            try {

            // Throttled face detection: run a few times a second and cache the
            // box. Between detections, reuse the last box until it goes stale.
            // Speed the cadence up while the head is moving so the crop tracks
            // the turn (accurate pose when it matters), idle otherwise.
            const auto frameNow = std::chrono::steady_clock::now();
            const auto faceInterval = (velEmaDegPerSec > kSettledVelDeg)
                                          ? kFaceIntervalFast
                                          : kFaceIntervalIdle;
            if (faceDetReady && frameNow - lastFaceDetect >= faceInterval) {
                lastFaceDetect = frameNow;
                if (auto box = faceDet.detect(d3d, f.texture.get(),
                                              f.subresource)) {
                    // Pad the tight detector box ~1.4x for a looser head crop
                    // (6DRepNet expects some margin around the face).
                    const LONG cx = (box->left + box->right) / 2;
                    const LONG cy = (box->top + box->bottom) / 2;
                    const LONG half =
                        std::max(box->right - box->left, box->bottom - box->top)
                        * 7 / 10;  // 1.4x / 2
                    cachedFace = RECT{cx - half, cy - half, cx + half, cy + half};
                    faceSeenAt = frameNow;
                    faceValid = true;
                } else if (frameNow - faceSeenAt > kFaceTtl) {
                    faceValid = false;
                }
            }

            // Crop to the detected face when we have one; otherwise the
            // preprocessor falls back to a centered square (empty RECT).
            RECT faceBox = (faceDetReady && faceValid) ? cachedFace : RECT{};
            preprocessor.Dispatch(f.texture.get(), f.subresource, faceBox);

            inference::HeadPose pose{};
            if (model) {
                pose = model->evaluate(preprocessor.outputBuffer());
            }
            // No face → no trustworthy pose: drop confidence so the calibrator
            // ignores this frame (an empty chair must not train the model).
            if (faceDetReady && !faceValid) {
                pose.confidence = 0.f;
            }
            // Note: there is intentionally no hardcoded pitch limit here. The
            // calibrator learns each monitor's (yaw,pitch) range and rejects
            // observations that fall outside every learned range, which adapts
            // to any physical layout (side-by-side or vertically stacked).

            // --- Head angular velocity (deg/s, smoothed) ---
            // Measured only from trusted poses; a no-face gap resets history so
            // a stale jump can't masquerade as a fast turn. Drives the
            // predictive dwell and the motion-gated face cadence below.
            if (pose.confidence > 0.f) {
                if (havePrevPose) {
                    const double dt =
                        std::chrono::duration<double>(frameNow - poseTimePrev).count();
                    if (dt > 1e-4 && dt < 0.2) {
                        const double dyaw = pose.yaw - yawPrev;
                        const double dpitch = pose.pitch - pitchPrev;
                        const double inst =
                            std::sqrt(dyaw * dyaw + dpitch * dpitch) / dt;
                        velEmaDegPerSec = 0.5 * velEmaDegPerSec + 0.5 * inst;
                    }
                }
                yawPrev = pose.yaw;
                pitchPrev = pose.pitch;
                poseTimePrev = frameNow;
                havePrevPose = true;
            } else {
                velEmaDegPerSec *= 0.5;  // decay toward "settled" when blind
                havePrevPose = false;
            }

            // --- Evidence: one sample per real input event ---
            // Typing lands on the foreground window's monitor and clicking on
            // the monitor under the cursor; both are strong evidence of which
            // screen the user faces. Record the current head-pose yaw against
            // that monitor exactly once per keystroke / click.
            const uint64_t seq = hooks.inputSeq();
            if (seq != lastInputSeq) {
                lastInputSeq = seq;
                if (HMONITOR inputMon = hooks.lastInputMonitor()) {
                    ++evidenceAttempts;
                    if (calibrator.recordEvidence(inputMon, pose.yaw, pose.pitch,
                                                  pose.confidence))
                        ++evidenceAccepted;
                }
            }

            // Reflect passive (learning) vs. active (calibrated) in the tray,
            // and surface convergence progress + a reason if it's stalled.
            const auto progress = calibrator.maturityProgress();
            const bool active = progress.mature;
            const int pct = static_cast<int>(progress.overall * 100.0 + 0.5);
            const wchar_t* reason = active ? nullptr : stuckReason(progress);
            if (active != wasActive) {
                wasActive = active;
                tray.postStatus(active ? tray::TrayIcon::Status::Active
                                       : tray::TrayIcon::Status::Passive);
            }
            if (!active) {
                wchar_t detail[160];
                if (reason) {
                    // Tell the user what to do, and still show the raw counts.
                    swprintf_s(detail,
                               L"%s\n(%zu screens \u00b7 samples %llu/%llu)",
                               reason, progress.monitorsSeen,
                               static_cast<unsigned long long>(progress.minSampleCount),
                               static_cast<unsigned long long>(progress.minSamples));
                } else {
                    swprintf_s(detail,
                               L"%zu screens \u00b7 samples %llu/%llu \u00b7 sep %.1f\u00b0/%.0f\u00b0",
                               progress.monitorsSeen,
                               static_cast<unsigned long long>(progress.minSampleCount),
                               static_cast<unsigned long long>(progress.minSamples),
                               progress.meanSeparationDeg,
                               progress.minMeanSeparationDeg);
                }

                // Re-post when either the percentage or the message changes
                // (the reason can change while the percentage is frozen at 0%).
                if (pct != lastPct || detail != lastDetail) {
                    lastPct = pct;
                    lastDetail = detail;
                    tray.postProgress(pct, detail);
                }

                // Periodic diagnostic to the log so the stall is discoverable
                // without hovering the tray.
                const auto now = std::chrono::steady_clock::now();
                if (now - lastDiagLog > 15s) {
                    lastDiagLog = now;
                    log::info(L"Convergence {}%: inference={}, face={}, "
                              L"yaw={:.1f} pitch={:.1f} vel={:.0f} conf={:.2f}, "
                              L"inputs={}, accepted={}, screens={}, "
                              L"samples={}/{} — {}",
                              pct, inferenceAvailable ? L"on" : L"off",
                              (faceDetReady && faceValid) ? L"yes" : L"no",
                              pose.yaw, pose.pitch, velEmaDegPerSec,
                              pose.confidence,
                              evidenceAttempts, evidenceAccepted,
                              progress.monitorsSeen,
                              progress.minSampleCount, progress.minSamples,
                              reason ? reason : L"progressing");
                }
            }

#if defined(MONITOUR_DEBUG_OVERLAY)
            // Debug-only stats box (top-left). Coalesced; safe to call hot.
            if (overlay) {
                overlay::DebugOverlay::Stats os{};
                os.active           = active;
                os.progressPct      = pct;
                os.inferenceOn      = inferenceAvailable;
                os.inputs           = evidenceAttempts;
                os.accepted         = evidenceAccepted;
                os.screens          = progress.monitorsSeen;
                os.sampleCount      = progress.minSampleCount;
                os.minSamples       = progress.minSamples;
                os.separationDeg    = progress.meanSeparationDeg;
                os.minSeparationDeg = progress.minMeanSeparationDeg;
                os.yaw              = pose.yaw;
                os.pitch            = pose.pitch;
                os.velocity         = velEmaDegPerSec;
                os.confidence       = pose.confidence;
                os.faceFound        = faceDetReady && faceValid;
                os.reason           = reason ? reason : L"";
                overlay->update(os);
            }
#endif

            // --- Focus switching (suppressed briefly after input) ---
            // Don't steal focus mid-sentence: hold off switching for
            // suppressAfterInput after the last keystroke / click. Learning
            // above still runs.
            if (hooks.sinceLastUserInput() < settings.suppressAfterInput) {
                continue;
            }

            auto classified = calibrator.classify(pose.yaw, pose.pitch,
                                                  pose.confidence);
            if (!classified) {
                lastTarget = nullptr;
                continue;
            }

            // Predictive dwell. The required dwell shrinks as the head settles:
            // a clearly stopped gaze commits almost immediately (kDwellSettled),
            // while jitter still waits the full dwellThreshold. A head still
            // sweeping fast (≥ kTransitVelDeg) defers entirely, so focus never
            // latches onto a monitor the user is merely panning across — this is
            // the "predict the landing instead of waiting it out" behaviour.
            auto now = std::chrono::steady_clock::now();
            if (*classified != lastTarget) {
                lastTarget = *classified;
                lastTargetSince = now;
                continue;
            }
            if (velEmaDegPerSec >= kTransitVelDeg) {
                continue;  // still mid-turn — don't commit yet
            }
            std::chrono::milliseconds requiredDwell;
            if (velEmaDegPerSec <= kSettledVelDeg) {
                requiredDwell = kDwellSettled;
            } else {
                // Interpolate kDwellSettled → dwellThreshold across the band.
                const double t = (velEmaDegPerSec - kSettledVelDeg) /
                                 (kTransitVelDeg - kSettledVelDeg);
                const auto span = settings.dwellThreshold - kDwellSettled;
                requiredDwell = kDwellSettled +
                    std::chrono::duration_cast<std::chrono::milliseconds>(span * t);
            }
            if (now - lastTargetSince < requiredDwell) {
                continue;
            }

            HWND target = mru.pickForMonitor(*classified);
            if (target) {
                if (focus::ForegroundSetter::setForeground(target) &&
                    glow && g_focusGlowEnabled.load() &&
                    *classified != lastFlashed) {
                    glow->flash(*classified);
                    lastFlashed = *classified;
                }
            }
            } catch (winrt::hresult_error const& e) {
                // Don't let an inference / D3D / WinRT hiccup take the whole
                // process down. The outer wmain_inner handler only catches
                // std::exception; a bare winrt::hresult_error from the hot
                // loop would have unwound past it and terminated silently.
                log::warn(L"Inference loop hresult_error: 0x{:x} {}",
                          static_cast<unsigned>(e.code().value),
                          std::wstring{e.message()});
            } catch (std::exception const& e) {
                const std::string what = e.what();
                log::warn(L"Inference loop exception: {}",
                          std::wstring(what.begin(), what.end()));
            }
        }
    }};

    // Capture starts after the consumer is alive so we don't drop the first
    // frame on the floor.
    capture.start();

    // --- Message pump on the main thread ---
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
            g_paused.exchange(!g_paused.load());
            tray.refresh();
            log::info(L"Hotkey pressed — paused = {}", g_paused.load());
            continue;
        }
        if (msg.message == WM_DISPLAYCHANGE) {
            mru.onDisplayChange();
            focusGlow.onDisplayChange();
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    log::info(L"Monitour shutting down");
    g_quit.store(true);
    SetEvent(slot.event());   // wake the worker
    capture.stop();
    if (inferThread.joinable()) inferThread.join();

#if defined(MONITOUR_DEBUG_OVERLAY)
    debugOverlay.destroy();
#endif
    focusGlow.destroy();

    UnregisterHotKey(nullptr, kHotkeyId);
    hooks.uninstall();
    mru.stop();
    calibrator.save(dataDir / L"calibration.json");
    tray::save(settings, dataDir / L"settings.json");

    MFShutdown();
    log::shutdown();
    return 0;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    try {
        return wmain_inner();
    } catch (std::exception const& e) {
        OutputDebugStringA(e.what());
        const std::string what = e.what();
        monitour::log::error(L"Fatal: {}",
                             std::wstring(what.begin(), what.end()));
        return 1;
    }
}
