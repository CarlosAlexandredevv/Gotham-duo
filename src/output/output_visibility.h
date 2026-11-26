#pragma once

#include <cstdint>

namespace gotham {

inline constexpr uint64_t kPresentStaleTimeoutMs = 1500;

/// Returns true when foreground focus belongs to the game window or one of the Duo output windows.
/// Uses opaque pointer comparison so the policy can be unit-tested without Win32 HWNDs.
inline bool IsGameWindowFocused(const void* fg_hwnd,
                                const void* game_hwnd,
                                const void* out0_hwnd,
                                const void* out1_hwnd) {
    if (!game_hwnd) {
        return true;
    }
    if (!fg_hwnd) {
        return false;
    }
    return fg_hwnd == game_hwnd ||
           (out0_hwnd && fg_hwnd == out0_hwnd) ||
           (out1_hwnd && fg_hwnd == out1_hwnd);
}

struct OutputVisibilityInput {
    bool is_duo = false;
    bool has_presented_duo = false;
    bool has_game_window = false;
    bool game_window_visible = false;
    bool game_window_minimized = false;
    bool game_focused = false;
    bool alt_tab_active = false;
    uint64_t last_present_ms = 0;
    uint64_t now_ms = 0;
};

/// Returns true when the two topmost Duo output windows should be visible.
inline bool ShouldShowDuoOutput(const OutputVisibilityInput& in) {
    if (!in.is_duo || !in.has_presented_duo || in.alt_tab_active) {
        return false;
    }
    if (in.has_game_window &&
        (!in.game_window_visible || in.game_window_minimized || !in.game_focused)) {
        return false;
    }
    if (in.last_present_ms != 0 && in.now_ms >= in.last_present_ms &&
        (in.now_ms - in.last_present_ms) >= kPresentStaleTimeoutMs) {
        return false;
    }
    return true;
}

}  // namespace gotham
