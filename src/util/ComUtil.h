#pragma once
#include <Windows.h>
#include <stdexcept>
#include <string>
#include <system_error>

#include <winrt/base.h>

namespace monitour::util {

inline void check_hresult(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::system_error{
            std::error_code{hr, std::system_category()},
            what ? what : "HRESULT failure"};
    }
}

#define MONITOUR_CHECK_HR(expr) ::monitour::util::check_hresult((expr), #expr)

}  // namespace monitour::util
