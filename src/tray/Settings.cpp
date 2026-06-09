#include "Settings.h"

#include <Windows.h>
#include <ShlObj.h>

#include <fstream>

#include "util/Logging.h"

namespace monitour::tray {

std::filesystem::path appDataDir() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        std::filesystem::path p{raw};
        CoTaskMemFree(raw);
        p /= L"Monitour";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }
    return std::filesystem::current_path();
}

bool save(const Settings& s, const std::filesystem::path& path) {
    std::ofstream f{path};
    if (!f) return false;
    f << "{\n"
      << "  \"enabled\": "             << (s.enabled ? "true" : "false") << ",\n"
      << "  \"dwellMs\": "             << s.dwellThreshold.count() << ",\n"
      << "  \"suppressAfterInputMs\": "<< s.suppressAfterInput.count() << ",\n"
      << "  \"hotkeyModifiers\": "     << s.hotkeyModifiers << ",\n"
      << "  \"hotkeyVk\": "            << s.hotkeyVk << ",\n"
      << "  \"hotkeyTeachModifiers\": "<< s.hotkeyTeachModifiers << ",\n"
      << "  \"hotkeyTeachVk\": "       << s.hotkeyTeachVk << ",\n"
      << "  \"activeFps\": "           << s.activeFps << ",\n"
      << "  \"idleFps\": "             << s.idleFps << ",\n"
      << "  \"onBatteryFps\": "        << s.onBatteryFps << ",\n"
      << "  \"focusGlowEnabled\": "    << (s.focusGlowEnabled ? "true" : "false") << "\n"
      << "}\n";
    return f.good();
}

bool load(Settings& s, const std::filesystem::path& path) {
    std::ifstream f{path};
    if (!f) return false;
    // TODO: real JSON parsing. Defaults are fine until then.
    (void)s;
    log::warn(L"Settings::load: JSON parser not yet implemented; using defaults.");
    return false;
}

}  // namespace monitour::tray
