#include "FrameSlot.h"

namespace monitour::capture {

FrameSlot::FrameSlot() {
    InitializeSRWLock(&lock_);
    event_ = CreateEventW(nullptr, /*manualReset*/ FALSE, /*initial*/ FALSE, nullptr);
}

FrameSlot::~FrameSlot() {
    if (event_) {
        CloseHandle(event_);
    }
}

void FrameSlot::publish(Frame frame) {
    AcquireSRWLockExclusive(&lock_);
    slot_ = std::move(frame);
    latestSeq_.store(slot_.seq, std::memory_order_release);
    ReleaseSRWLockExclusive(&lock_);
    SetEvent(event_);
}

bool FrameSlot::waitAndTake(Frame& out, std::chrono::milliseconds timeout) {
    DWORD ms = static_cast<DWORD>(timeout.count());
    DWORD wait = WaitForSingleObject(event_, ms);
    if (wait != WAIT_OBJECT_0) {
        return false;
    }
    AcquireSRWLockExclusive(&lock_);
    if (!slot_.texture) {
        ReleaseSRWLockExclusive(&lock_);
        return false;
    }
    out = std::move(slot_);
    slot_ = Frame{};
    ReleaseSRWLockExclusive(&lock_);
    return true;
}

}  // namespace monitour::capture
