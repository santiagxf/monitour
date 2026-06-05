#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>

namespace monitour::capture {

// Single-slot "latest frame" holder. The capture thread overwrites; the
// inference thread takes (and clears) the slot. No queue → we structurally
// cannot process a stale frame.
//
// We hold an ID3D11Texture2D* (refcounted via com_ptr) plus a monotonic
// QPC timestamp and a sequence number. The slot exposes a Win32 event the
// consumer can wait on.
struct Frame {
    winrt::com_ptr<ID3D11Texture2D> texture;
    UINT                            subresource{0};
    LONGLONG                        captureQpc{0};
    uint64_t                        seq{0};
};

class FrameSlot {
public:
    FrameSlot();
    ~FrameSlot();

    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;

    // Producer side: replaces whatever was there, signals the event.
    void publish(Frame frame);

    // Consumer side: blocks up to `timeout` for a frame. Returns false on
    // timeout. On success, `out` is filled and the slot is cleared.
    bool waitAndTake(Frame& out, std::chrono::milliseconds timeout);

    [[nodiscard]] HANDLE event() const noexcept { return event_; }
    [[nodiscard]] uint64_t latestSeq() const noexcept { return latestSeq_.load(std::memory_order_acquire); }

private:
    SRWLOCK              lock_{};
    Frame                slot_{};
    HANDLE               event_{nullptr};
    std::atomic<uint64_t> latestSeq_{0};
};

}  // namespace monitour::capture
