#pragma once
#include <Windows.h>

#include <chrono>
#include <filesystem>

namespace monitour::tray {

struct Settings {
    bool                      enabled                  = true;
    std::chrono::milliseconds dwellThreshold           {250};
    std::chrono::milliseconds suppressAfterInput       {1500};
    UINT                      hotkeyModifiers          = MOD_CONTROL | MOD_ALT;
    UINT                      hotkeyVk                 = 'M';
    UINT                      activeFps                = 60;
    UINT                      idleFps                  = 1;
    UINT                      onBatteryFps             = 15;
    bool                      focusGlowEnabled         = true;
};

bool   load(Settings& s, const std::filesystem::path& path);
bool   save(const Settings& s, const std::filesystem::path& path);

// Resolves %LOCALAPPDATA%\Monitour\, creating it if needed.
std::filesystem::path appDataDir();

}  // namespace monitour::tray
