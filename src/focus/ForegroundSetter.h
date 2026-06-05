#pragma once
#include <Windows.h>

namespace monitour::focus {

class ForegroundSetter {
public:
    // Attempts to make `target` the foreground window. Returns true on success.
    //
    // Implementation: AttachThreadInput trick. SetForegroundWindow normally
    // refuses unless the caller already owns the foreground; we temporarily
    // attach our thread's input queue to the current foreground thread,
    // which makes Windows consider us "co-owners" and allow the call.
    static bool setForeground(HWND target);
};

}  // namespace monitour::focus
