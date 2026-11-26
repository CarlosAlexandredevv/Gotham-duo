#include "output/output_visibility.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

namespace {

using gotham::IsGameWindowFocused;
using gotham::kPresentStaleTimeoutMs;
using gotham::OutputVisibilityInput;
using gotham::ShouldShowDuoOutput;

OutputVisibilityInput MakeActiveDuoInput() {
    OutputVisibilityInput in{};
    in.is_duo = true;
    in.has_presented_duo = true;
    in.has_game_window = true;
    in.game_window_visible = true;
    in.game_window_minimized = false;
    in.game_focused = true;
    in.alt_tab_active = false;
    in.last_present_ms = 1000;
    in.now_ms = 1100;
    return in;
}

void TestFocusMatching() {
    const int game = 1;
    const int out0 = 2;
    const int out1 = 3;
    const int other = 4;

    // Before game HWND is captured, treat as focused so startup isn't blocked.
    CHECK(IsGameWindowFocused(nullptr, nullptr, &out0, &out1));
    CHECK(IsGameWindowFocused(&other, nullptr, &out0, &out1));

    // Focused when foreground is the game or either Duo output window.
    CHECK(IsGameWindowFocused(&game, &game, &out0, &out1));
    CHECK(IsGameWindowFocused(&out0, &game, &out0, &out1));
    CHECK(IsGameWindowFocused(&out1, &game, &out0, &out1));

    // Unfocused when foreground is null (mid-Alt+Tab) or another app.
    CHECK(!IsGameWindowFocused(nullptr, &game, &out0, &out1));
    CHECK(!IsGameWindowFocused(&other, &game, &out0, &out1));
    CHECK(!IsGameWindowFocused(nullptr, &game, nullptr, nullptr));
}

void TestVisibilityTransitions() {
    // Active focused Duo mode shows output windows.
    auto in = MakeActiveDuoInput();
    CHECK(ShouldShowDuoOutput(in));

    // Alt+Tab switcher hides output immediately even before foreground updates.
    in.alt_tab_active = true;
    CHECK(!ShouldShowDuoOutput(in));
    in.alt_tab_active = false;
    CHECK(ShouldShowDuoOutput(in));

    // Switching to another app hides output; returning restores it.
    in.game_focused = false;
    CHECK(!ShouldShowDuoOutput(in));
    in.game_focused = true;
    CHECK(ShouldShowDuoOutput(in));

    // Minimizing or hiding the game window hides output.
    in.game_window_minimized = true;
    CHECK(!ShouldShowDuoOutput(in));
    in.game_window_minimized = false;
    in.game_window_visible = false;
    CHECK(!ShouldShowDuoOutput(in));
    in.game_window_visible = true;
    CHECK(ShouldShowDuoOutput(in));

    // Solo mode or before first Duo frame never shows output.
    in.is_duo = false;
    CHECK(!ShouldShowDuoOutput(in));
    in.is_duo = true;
    in.has_presented_duo = false;
    CHECK(!ShouldShowDuoOutput(in));
}

void TestPresentFreshnessAndClockWrap() {
    auto in = MakeActiveDuoInput();
    in.last_present_ms = 5000;
    in.now_ms = 5000 + kPresentStaleTimeoutMs - 1;
    CHECK(ShouldShowDuoOutput(in));

    // Stale frames hide output.
    in.now_ms = 5000 + kPresentStaleTimeoutMs;
    CHECK(!ShouldShowDuoOutput(in));

    // Non-monotonic clock does not underflow into stale.
    in.now_ms = 4000;
    CHECK(ShouldShowDuoOutput(in));
}

}  // namespace

int main() {
    TestFocusMatching();
    TestVisibilityTransitions();
    TestPresentFreshnessAndClockWrap();
    std::printf("All output_visibility_tests passed.\n");
    return 0;
}
