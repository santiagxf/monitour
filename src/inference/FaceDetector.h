#pragma once
#include <Windows.h>
#include <d3d11.h>

#include <optional>

#include <winrt/Windows.Media.FaceAnalysis.h>

namespace monitour::d3d { class D3DContext; }

namespace monitour::inference {

// Cheap face detection using the Windows built-in
// Windows.Media.FaceAnalysis.FaceDetector (a CPU Viola-Jones-style detector).
// Zero model files, zero extra dependencies. We feed it the NV12 frame's Y
// plane as a Gray8 bitmap — no color conversion — so each detection is just a
// GPU→CPU readback of the luma plane plus the detector pass.
//
// Construct on an MTA thread (the inference worker): CreateAsync().get() and
// DetectAsync().get() block, which would deadlock on the STA main thread.
class FaceDetector {
public:
    FaceDetector() = default;

    FaceDetector(const FaceDetector&) = delete;
    FaceDetector& operator=(const FaceDetector&) = delete;

    // Initializes the detector. Returns false if the platform doesn't support
    // it (the caller then falls back to a center-square crop).
    bool create();

    [[nodiscard]] bool ready() const noexcept { return detector_ != nullptr; }

    // Runs one detection on the largest face in `nv12` (the capture texture).
    // Returns the tight face box in source-frame pixels, or nullopt if no face
    // was found. `subresource` selects the array slice delivered by capture.
    std::optional<RECT> detect(d3d::D3DContext& d3d, ID3D11Texture2D* nv12,
                               UINT subresource);

private:
    winrt::Windows::Media::FaceAnalysis::FaceDetector detector_{nullptr};

    // Reused luma staging texture (lazily (re)created to match the frame).
    winrt::com_ptr<ID3D11Texture2D> staging_;
    UINT stagingW_{0};
    UINT stagingH_{0};
};

}  // namespace monitour::inference
