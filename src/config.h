#pragma once

#include "input/input_trigger.h"
#include "output/split_seam_detector.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cwchar>
#endif

namespace gotham {

// Win32 virtual-key defaults (defined numerically so this header stays Windows-free for unit tests).
inline constexpr int kDefaultToggleDuoVk = 0x78;      // VK_F9
inline constexpr int kDefaultToggleMirrorVk = 0x70;   // VK_F1
inline constexpr int kDefaultReapplyResizeVk = 0x79;  // VK_F10

struct MonitorsConfig {
    int player1 = -1;
    int player2 = -1;
};

struct HotkeysConfig {
    /// Win32 virtual-key code, or 0 when disabled.
    int toggle_duo_vk = kDefaultToggleDuoVk;
    int toggle_mirror_vk = kDefaultToggleMirrorVk;
    int reapply_resize_vk = kDefaultReapplyResizeVk;
};

struct DisplayConfig {
    /// Optional Duo span width/height overrides (0 = auto from target monitors).
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    /// Optional Solo / Mirror width/height overrides (0 = auto from primary monitor).
    uint32_t solo_width = 0;
    uint32_t solo_height = 0;
};

struct LoggingConfig {
    bool enabled = true;
    bool verbose = false;
};

struct AppConfig {
    MonitorsConfig monitors{};
    InputConfig input{};
    HotkeysConfig hotkeys{};
    MirrorConfig mirror{};
    DisplayConfig display{};
    LoggingConfig logging{};
    std::vector<std::string> warnings;
};

namespace detail {

inline std::string Trim(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

inline std::string ToUpperAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

inline std::optional<int> ParseIntStrict(std::string_view sv) {
    const std::string s = Trim(sv);
    if (s.empty()) {
        return std::nullopt;
    }
    size_t idx = 0;
    int64_t val = 0;
    try {
        val = std::stoll(s, &idx, 10);
    } catch (...) {
        return std::nullopt;
    }
    if (idx != s.size() || val < -2147483648LL || val > 2147483647LL) {
        return std::nullopt;
    }
    return static_cast<int>(val);
}

inline std::optional<bool> ParseBoolStrict(std::string_view sv) {
    const std::string up = ToUpperAscii(Trim(sv));
    if (up == "1" || up == "TRUE" || up == "YES" || up == "ON") {
        return true;
    }
    if (up == "0" || up == "FALSE" || up == "NO" || up == "OFF") {
        return false;
    }
    return std::nullopt;
}

}  // namespace detail

/// Parses a hotkey name (e.g. "F9", "F1", "A", "Insert", "0x78", "None") into a Win32 VK code.
/// Returns 0 for explicitly disabled hotkeys, or std::nullopt for unrecognized values.
inline std::optional<int> ParseVirtualKey(std::string_view raw) {
    const std::string up = detail::ToUpperAscii(detail::Trim(raw));
    if (up.empty() || up == "NONE" || up == "OFF" || up == "DISABLED" || up == "0") {
        return 0;
    }

    // Hex VK code (e.g. 0x78)
    if (up.size() > 2 && up[0] == '0' && up[1] == 'X') {
        size_t idx = 0;
        int val = 0;
        try {
            val = std::stoi(up.substr(2), &idx, 16);
        } catch (...) {
            return std::nullopt;
        }
        if (idx == up.size() - 2 && val >= 1 && val <= 0xFE) {
            return val;
        }
        return std::nullopt;
    }

    // Function keys F1..F24
    if (up.size() >= 2 && up[0] == 'F') {
        if (auto n = detail::ParseIntStrict(std::string_view(up).substr(1));
            n.has_value() && *n >= 1 && *n <= 24) {
            return 0x70 + (*n - 1);
        }
    }

    // Single ASCII letter A..Z or digit 1..9
    if (up.size() == 1) {
        const char c = up[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '1' && c <= '9')) {
            return static_cast<int>(c);
        }
    }

    // Explicit digit D0..D9 or numpad Num0..Num9
    if (up.size() == 2 && up[0] == 'D' && up[1] >= '0' && up[1] <= '9') {
        return static_cast<int>(up[1]);
    }
    if (up.size() == 4 && up.rfind("NUM", 0) == 0 && up[3] >= '0' && up[3] <= '9') {
        return 0x60 + (up[3] - '0');
    }

    // Common named keys
    if (up == "INSERT" || up == "INS") return 0x2D;
    if (up == "DELETE" || up == "DEL") return 0x2E;
    if (up == "HOME") return 0x24;
    if (up == "END") return 0x23;
    if (up == "PAGEUP" || up == "PGUP") return 0x21;
    if (up == "PAGEDOWN" || up == "PGDN") return 0x22;
    if (up == "PAUSE") return 0x13;
    if (up == "SCROLLLOCK") return 0x91;
    if (up == "PRINTSCREEN" || up == "PRTSC") return 0x2C;
    if (up == "TAB") return 0x09;
    if (up == "SPACE") return 0x20;
    if (up == "BACKSPACE") return 0x08;
    if (up == "ENTER" || up == "RETURN") return 0x0D;
    if (up == "ESCAPE" || up == "ESC") return 0x1B;

    return std::nullopt;
}

/// Parses INI contents into AppConfig. Missing or malformed entries fall back to defaults with warnings.
inline AppConfig ParseConfig(std::string_view ini_text) {
    AppConfig cfg{};
    std::string section;

    size_t pos = 0;
    int line_no = 0;
    while (pos <= ini_text.size()) {
        size_t nl = ini_text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = ini_text.size();
        }
        std::string_view raw_line = ini_text.substr(pos, nl - pos);
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.remove_suffix(1);
        }
        pos = (nl < ini_text.size()) ? (nl + 1) : (ini_text.size() + 1);
        ++line_no;

        // Strip UTF-8 BOM on line 1 if present.
        if (line_no == 1 && raw_line.size() >= 3 &&
            static_cast<unsigned char>(raw_line[0]) == 0xEF &&
            static_cast<unsigned char>(raw_line[1]) == 0xBB &&
            static_cast<unsigned char>(raw_line[2]) == 0xBF) {
            raw_line.remove_prefix(3);
        }

        const std::string line = detail::Trim(raw_line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = detail::ToUpperAscii(detail::Trim(std::string_view(line).substr(1, line.size() - 2)));
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            cfg.warnings.push_back("Line " + std::to_string(line_no) + ": ignoring malformed line '" + line + "'");
            continue;
        }

        const std::string key_orig = detail::Trim(std::string_view(line).substr(0, eq));
        const std::string key = detail::ToUpperAscii(key_orig);
        std::string val = detail::Trim(std::string_view(line).substr(eq + 1));
        if (const size_t semi = val.find(';'); semi != std::string::npos) {
            val = detail::Trim(std::string_view(val).substr(0, semi));
        }

        auto warn_invalid = [&](const char* expected) {
            cfg.warnings.push_back("[" + section + "] " + key_orig + "='" + val +
                                   "' is invalid (" + expected + "); using default.");
        };

        if (section == "MONITORS") {
            if (key == "PLAYER1") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= -1) {
                    cfg.monitors.player1 = *v;
                } else {
                    warn_invalid("expected integer >= -1");
                }
            } else if (key == "PLAYER2") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= -1) {
                    cfg.monitors.player2 = *v;
                } else {
                    warn_invalid("expected integer >= -1");
                }
            }
        } else if (section == "INPUT") {
            if (key == "ALLOWCONTROLLERTOGGLE") {
                if (auto v = detail::ParseBoolStrict(val); v.has_value()) {
                    cfg.input.allow_controller = *v;
                } else {
                    warn_invalid("expected 0/1 or true/false");
                }
            } else if (key == "P2CONTROLLERINDEX") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= -1 && *v <= 3) {
                    cfg.input.p2_controller_index = *v;
                } else {
                    warn_invalid("expected -1 or 0..3");
                }
            } else if (key == "HOLDBACKSTARTMS") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v > 0) {
                    cfg.input.hold_back_start_ms = static_cast<uint32_t>(*v);
                } else {
                    warn_invalid("expected integer > 0");
                }
            }
        } else if (section == "HOTKEYS") {
            if (key == "TOGGLEDUOKEY") {
                if (auto vk = ParseVirtualKey(val); vk.has_value()) {
                    cfg.hotkeys.toggle_duo_vk = *vk;
                } else {
                    warn_invalid("expected key name like F9, 0x78, or None");
                }
            } else if (key == "TOGGLEMIRRORKEY") {
                if (auto vk = ParseVirtualKey(val); vk.has_value()) {
                    cfg.hotkeys.toggle_mirror_vk = *vk;
                } else {
                    warn_invalid("expected key name like F1, 0x70, or None");
                }
            } else if (key == "REAPPLYRESIZEKEY") {
                if (auto vk = ParseVirtualKey(val); vk.has_value()) {
                    cfg.hotkeys.reapply_resize_vk = *vk;
                } else {
                    warn_invalid("expected key name like F10, 0x79, or None");
                }
            }
        } else if (section == "MENUMIRROR") {
            if (key == "AUTODETECT") {
                if (auto v = detail::ParseBoolStrict(val); v.has_value()) {
                    cfg.mirror.auto_detect = *v;
                } else {
                    warn_invalid("expected 0/1 or true/false");
                }
            } else if (key == "ENTERMIRRORMS") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= 0) {
                    cfg.mirror.enter_mirror_ms = static_cast<uint32_t>(*v);
                } else {
                    warn_invalid("expected integer >= 0");
                }
            } else if (key == "EXITMIRRORMS") {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= 0) {
                    cfg.mirror.exit_mirror_ms = static_cast<uint32_t>(*v);
                } else {
                    warn_invalid("expected integer >= 0");
                }
            }
        } else if (section == "DISPLAY") {
            auto parse_dim = [&](uint32_t& target) {
                if (auto v = detail::ParseIntStrict(val); v.has_value() && *v >= 0 && *v <= 32768) {
                    target = static_cast<uint32_t>(*v);
                } else {
                    warn_invalid("expected integer 0..32768");
                }
            };
            if (key == "TARGETWIDTH") {
                parse_dim(cfg.display.target_width);
            } else if (key == "TARGETHEIGHT") {
                parse_dim(cfg.display.target_height);
            } else if (key == "SOLOWIDTH") {
                parse_dim(cfg.display.solo_width);
            } else if (key == "SOLOHEIGHT") {
                parse_dim(cfg.display.solo_height);
            }
        } else if (section == "LOGGING") {
            if (key == "ENABLED") {
                if (auto v = detail::ParseBoolStrict(val); v.has_value()) {
                    cfg.logging.enabled = *v;
                } else {
                    warn_invalid("expected 0/1 or true/false");
                }
            } else if (key == "VERBOSE") {
                if (auto v = detail::ParseBoolStrict(val); v.has_value()) {
                    cfg.logging.verbose = *v;
                } else {
                    warn_invalid("expected 0/1 or true/false");
                }
            }
        }
    }

    return cfg;
}

/// Loads gothamduo.ini from a wide file path; returns defaults if the file does not exist.
inline AppConfig LoadConfigFromFile(const wchar_t* ini_path) {
    if (!ini_path || ini_path[0] == L'\0') {
        return AppConfig{};
    }
    std::ifstream ifs(ini_path, std::ios::binary);
    if (!ifs.is_open()) {
        return AppConfig{};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ParseConfig(ss.str());
}

#ifdef _WIN32
/// Builds the gothamduo.ini path beside self_module and loads it (falls back to defaults if missing).
inline AppConfig LoadConfigForModule(HMODULE self_module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(self_module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return AppConfig{};
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash ||
        (MAX_PATH - static_cast<size_t>(slash + 1 - path)) < _countof(L"gothamduo.ini")) {
        return AppConfig{};
    }
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path), L"gothamduo.ini");
    return LoadConfigFromFile(path);
}
#endif

}  // namespace gotham
