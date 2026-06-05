#include "ScopedMmcss.h"

#include <avrt.h>

#include "Logging.h"

namespace monitour::util {

ScopedMmcss::ScopedMmcss(std::wstring_view task) noexcept {
    handle_ = AvSetMmThreadCharacteristicsW(std::wstring{task}.c_str(), &taskIndex_);
    if (!handle_) {
        log::warn(L"AvSetMmThreadCharacteristicsW({}) failed: {}",
                  task, GetLastError());
    } else {
        // Boost slightly above normal; MMCSS keeps the priority capped.
        AvSetMmThreadPriority(handle_, AVRT_PRIORITY_HIGH);
    }
}

ScopedMmcss::~ScopedMmcss() {
    if (handle_) {
        AvRevertMmThreadCharacteristics(handle_);
        handle_ = nullptr;
    }
}

}  // namespace monitour::util
