# Solo / Duo Mode Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Start Gotham Duo in Solo mode (`1920x1080` borderless on the primary monitor) and switch cleanly to/from Duo mode (`3840x1080` split across two monitors) via `F9` or Controller 2 (`Start` to enter Duo, hold `Back + Start` for 800 ms to return to Solo).

**Architecture:** Extract pure input-trigger state logic into `src/input_trigger.{h,cpp}` (with dynamic `XInputGetState` polling and zero-hardware unit tests), then refactor `DualOutput` (`src/dual_output.cpp`) around an explicit `OutputMode` state machine (`Solo` vs `Duo`) that creates hidden overlay windows, frees extra D3D12 swapchain VRAM when returning to `Solo`, and resizes the game window cleanly between `mon_solo` and the combined dual-monitor span.

**Tech Stack:** C++20, MSVC x64, Windows SDK (Win32, D3D12, DXGI 1.5, XInput loaded dynamically), CMake 4.4 / CTest.

---

### File Structure

- **Create:** `src/input_trigger.h` — Pure `InputConfig`, `ControllerButtons`, `TriggerAction`, `InputTrigger` class, plus `PollSystemInput` declaration.
- **Create:** `src/input_trigger.cpp` — `InputTrigger::Update` state machine + dynamic `XInput` loader/poller.
- **Create:** `tests/input_trigger_test.cpp` — Standalone assert-based tests for `InputTrigger` (compiled into `gothamduo_tests.exe`).
- **Modify:** `CMakeLists.txt` — Add `src/input_trigger.cpp` to `gothamduo` and register `gothamduo_tests` via `enable_testing()` / `add_test()`.
- **Modify:** `src/dual_output.cpp` — Replace `split_enabled` / `tried_wide_resize` with `OutputMode::Solo` / `OutputMode::Duo`, hidden startup windows, `SetFullscreenState(FALSE)` handling, `ResizeGameWindowToRect`, and `[Input]` INI loading.
- **Modify:** `config/gothamduo.ini` and `README.md` — Document `[Input]` keys and updated Solo-first behavior (`F9`, Controller 2 `Start` / `Back+Start`).

---

### Task 1: Add `InputTrigger` pure state machine and unit tests (TDD)

**Files:**
- Create: `src/input_trigger.h`
- Create: `src/input_trigger.cpp`
- Create: `tests/input_trigger_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write `src/input_trigger.h` and `tests/input_trigger_test.cpp` first**

Create `src/input_trigger.h`:

```cpp
#pragma once

#include <array>
#include <cstdint>

namespace gotham {

struct InputConfig {
    bool allow_controller = true;
    int p2_controller_index = -1;  // -1 = monitor slots 1..3; 0..3 = specific slot
    uint64_t hold_back_start_ms = 800;
};

struct ControllerButtons {
    bool connected = false;
    bool start = false;
    bool back = false;
};

enum class TriggerAction {
    None,
    ToggleToDuo,
    ToggleToSolo,
};

/// Pure state machine deciding when to switch between Solo and Duo modes.
class InputTrigger {
public:
    explicit InputTrigger(InputConfig config = {}) : config_(config) {}

    void SetConfig(InputConfig config) { config_ = config; }
    const InputConfig& config() const { return config_; }

    /// Evaluates F9 and controller button states at timestamp `now_ms`.
    TriggerAction Update(bool is_duo,
                         bool f9_down,
                         const std::array<ControllerButtons, 4>& slots,
                         uint64_t now_ms);

private:
    InputConfig config_{};
    bool prev_f9_down_ = false;
    std::array<bool, 4> prev_start_down_{};
    std::array<uint64_t, 4> combo_hold_start_ms_{};
    std::array<bool, 4> wait_release_after_exit_{};
};

/// Polls VK_F9 and XInput (loaded dynamically) into `out_slots`.
void PollSystemInput(bool& out_f9_down, std::array<ControllerButtons, 4>& out_slots);

}  // namespace gotham
```

Create `tests/input_trigger_test.cpp`:

```cpp
#include "input_trigger.h"

#include <cassert>
#include <cstdio>

using gotham::ControllerButtons;
using gotham::InputConfig;
using gotham::InputTrigger;
using gotham::TriggerAction;

namespace {

std::array<ControllerButtons, 4> EmptySlots() {
    return {};
}

void TestF9EdgeToggle() {
    InputTrigger trig;
    auto slots = EmptySlots();

    assert(trig.Update(false, false, slots, 0) == TriggerAction::None);
    assert(trig.Update(false, true, slots, 10) == TriggerAction::ToggleToDuo);
    // Holding F9 down must not re-trigger.
    assert(trig.Update(true, true, slots, 20) == TriggerAction::None);
    assert(trig.Update(true, false, slots, 30) == TriggerAction::None);
    assert(trig.Update(true, true, slots, 40) == TriggerAction::ToggleToSolo);
}

void TestController2StartEntersDuo() {
    InputTrigger trig;
    auto slots = EmptySlots();
    slots[0] = {true, true, false};
    // Slot 0 (P1) Start must be ignored in default auto mode (-1).
    assert(trig.Update(false, false, slots, 100) == TriggerAction::None);

    slots[1] = {true, true, false};
    assert(trig.Update(false, false, slots, 110) == TriggerAction::ToggleToDuo);
    // Holding Start in Duo must not exit Duo.
    assert(trig.Update(true, false, slots, 2000) == TriggerAction::None);
}

void TestHoldBackStartExitsDuoAfter800Ms() {
    InputTrigger trig;
    auto slots = EmptySlots();

    // Enter Duo via slot 1 Start, then release.
    slots[1] = {true, true, false};
    assert(trig.Update(false, false, slots, 100) == TriggerAction::ToggleToDuo);
    slots[1] = {true, false, false};
    assert(trig.Update(true, false, slots, 120) == TriggerAction::None);

    // Hold Back+Start for 799 ms -> still None.
    slots[1] = {true, true, true};
    assert(trig.Update(true, false, slots, 200) == TriggerAction::None);
    assert(trig.Update(true, false, slots, 999) == TriggerAction::None);

    // Reach 800 ms -> ToggleToSolo.
    assert(trig.Update(true, false, slots, 1000) == TriggerAction::ToggleToSolo);

    // While still holding Start/Back in Solo, must NOT immediately re-enter Duo.
    assert(trig.Update(false, false, slots, 1050) == TriggerAction::None);

    // Release both buttons, then Start again -> re-enters Duo.
    slots[1] = {true, false, false};
    assert(trig.Update(false, false, slots, 1100) == TriggerAction::None);
    slots[1] = {true, true, false};
    assert(trig.Update(false, false, slots, 1150) == TriggerAction::ToggleToDuo);
}

void TestDisabledControllerAndExplicitSlot() {
    InputConfig cfg{};
    cfg.allow_controller = false;
    InputTrigger disabled_trig(cfg);
    auto slots = EmptySlots();
    slots[1] = {true, true, false};
    assert(disabled_trig.Update(false, false, slots, 10) == TriggerAction::None);
    assert(disabled_trig.Update(false, true, slots, 20) == TriggerAction::ToggleToDuo);

    cfg.allow_controller = true;
    cfg.p2_controller_index = 2;
    InputTrigger slot2_trig(cfg);
    assert(slot2_trig.Update(false, false, slots, 30) == TriggerAction::None);
    slots[2] = {true, true, false};
    assert(slot2_trig.Update(false, false, slots, 40) == TriggerAction::ToggleToDuo);
}

}  // namespace

int main() {
    TestF9EdgeToggle();
    TestController2StartEntersDuo();
    TestHoldBackStartExitsDuoAfter800Ms();
    TestDisabledControllerAndExplicitSlot();
    std::puts("All InputTrigger tests passed.");
    return 0;
}
```

- [ ] **Step 2: Create stub `src/input_trigger.cpp` and register test target in `CMakeLists.txt` to verify TDD failure**

Write a minimal stub `src/input_trigger.cpp` first that compiles cleanly and returns `TriggerAction::None`:

```cpp
#include "input_trigger.h"

namespace gotham {

TriggerAction InputTrigger::Update(bool,
                                   bool,
                                   const std::array<ControllerButtons, 4>&,
                                   uint64_t) {
    return TriggerAction::None;
}

void PollSystemInput(bool& out_f9_down, std::array<ControllerButtons, 4>& out_slots) {
    out_f9_down = false;
    out_slots = {};
}

}  // namespace gotham
```

Update `CMakeLists.txt` to add `src/input_trigger.cpp` to `gothamduo`, enable CTest, and add `gothamduo_tests`:

```cmake
add_library(gothamduo SHARED
    src/dllmain.cpp
    src/dual_output.cpp
    src/dxgi_hook.cpp
    src/input_trigger.cpp
    src/version_proxy.cpp
    src/version_trampolines.asm
    src/version.def
)
```

Append to the end of `CMakeLists.txt`:

```cmake
enable_testing()
add_executable(gothamduo_tests
    tests/input_trigger_test.cpp
    src/input_trigger.cpp
)
target_include_directories(gothamduo_tests PRIVATE src)
target_compile_definitions(gothamduo_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(gothamduo_tests PRIVATE user32)
add_test(NAME input_trigger_tests COMMAND gothamduo_tests)
```

- [ ] **Step 3: Build and run `ctest` to verify the test fails against the stub**

Run:
```powershell
& "E:\DevTools\CMake\bin\cmake.exe" --build build --config Release --target gothamduo_tests
& "E:\DevTools\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```
Expected: `gothamduo_tests` compiles and FAILS assertion in `TestF9EdgeToggle`.

- [ ] **Step 4: Implement `src/input_trigger.cpp`**

Replace `src/input_trigger.cpp` with the full implementation:

```cpp
#include "input_trigger.h"

#include <Windows.h>
#include <Xinput.h>

namespace gotham {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn ResolveXInputGetState() {
    static HMODULE mod = []() -> HMODULE {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) {
            m = LoadLibraryW(L"xinput9_1_0.dll");
        }
        return m;
    }();
    static XInputGetStateFn fn =
        mod ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState")) : nullptr;
    return fn;
}

}  // namespace

TriggerAction InputTrigger::Update(bool is_duo,
                                   bool f9_down,
                                   const std::array<ControllerButtons, 4>& slots,
                                   uint64_t now_ms) {
    const bool f9_rising = f9_down && !prev_f9_down_;
    prev_f9_down_ = f9_down;

    if (f9_rising) {
        combo_hold_start_ms_.fill(0);
        for (size_t i = 0; i < 4; ++i) {
            prev_start_down_[i] = slots[i].start;
        }
        return is_duo ? TriggerAction::ToggleToSolo : TriggerAction::ToggleToDuo;
    }

    if (!config_.allow_controller) {
        return TriggerAction::None;
    }

    int start_idx = 1;
    int end_idx = 3;
    if (config_.p2_controller_index >= 0 && config_.p2_controller_index <= 3) {
        start_idx = config_.p2_controller_index;
        end_idx = config_.p2_controller_index;
    }

    TriggerAction action = TriggerAction::None;
    for (int i = start_idx; i <= end_idx; ++i) {
        const auto& s = slots[i];
        if (!s.connected) {
            prev_start_down_[i] = false;
            combo_hold_start_ms_[i] = 0;
            wait_release_after_exit_[i] = false;
            continue;
        }

        if (wait_release_after_exit_[i]) {
            if (!s.start && !s.back) {
                wait_release_after_exit_[i] = false;
            }
            prev_start_down_[i] = s.start;
            combo_hold_start_ms_[i] = 0;
            continue;
        }

        const bool start_rising = s.start && !prev_start_down_[i];
        prev_start_down_[i] = s.start;

        if (!is_duo) {
            combo_hold_start_ms_[i] = 0;
            if (start_rising && action == TriggerAction::None) {
                action = TriggerAction::ToggleToDuo;
            }
            continue;
        }

        if (s.start && s.back) {
            if (combo_hold_start_ms_[i] == 0) {
                combo_hold_start_ms_[i] = now_ms;
            } else if (now_ms - combo_hold_start_ms_[i] >= config_.hold_back_start_ms &&
                       action == TriggerAction::None) {
                combo_hold_start_ms_[i] = 0;
                wait_release_after_exit_[i] = true;
                action = TriggerAction::ToggleToSolo;
            }
        } else {
            combo_hold_start_ms_[i] = 0;
        }
    }

    return action;
}

void PollSystemInput(bool& out_f9_down, std::array<ControllerButtons, 4>& out_slots) {
    out_f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    out_slots = {};

    XInputGetStateFn get_state = ResolveXInputGetState();
    if (!get_state) {
        return;
    }

    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE state{};
        if (get_state(i, &state) == ERROR_SUCCESS) {
            out_slots[i].connected = true;
            out_slots[i].start = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
            out_slots[i].back = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        }
    }
}

}  // namespace gotham
```

- [ ] **Step 5: Build and run `ctest` to verify all `InputTrigger` tests pass**

Run:
```powershell
& "E:\DevTools\CMake\bin\cmake.exe" --build build --config Release
& "E:\DevTools\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```
Expected: `All InputTrigger tests passed.` (`100% tests passed, 0 tests failed out of 1`).

---

### Task 2: Integrate Solo/Duo state machine into `DualOutput`

**Files:**
- Modify: `src/dual_output.cpp`

- [ ] **Step 1: Add `OutputMode` and `mon_solo` selection + `[Input]` config reading in `src/dual_output.cpp`**

Include `"input_trigger.h"` at the top of `src/dual_output.cpp`, add `enum class OutputMode { Solo, Duo };`, update `SelectTargetMonitors` to also pick `out_solo` (the primary monitor, or first landscape monitor if none marked primary), and add `LoadInputConfig(HMODULE self_module)`:

```cpp
InputConfig LoadInputConfig(HMODULE self_module) {
    wchar_t ini_path[MAX_PATH] = {};
    GetModuleFileNameW(self_module, ini_path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(ini_path, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - ini_path), L"gothamduo.ini");
    }

    InputConfig cfg{};
    cfg.allow_controller = GetPrivateProfileIntW(L"Input", L"AllowControllerToggle", 1, ini_path) != 0;
    cfg.p2_controller_index = GetPrivateProfileIntW(L"Input", L"P2ControllerIndex", -1, ini_path);
    const int hold_ms = GetPrivateProfileIntW(L"Input", L"HoldBackStartMs", 800, ini_path);
    cfg.hold_back_start_ms = hold_ms > 0 ? static_cast<uint64_t>(hold_ms) : 800;
    return cfg;
}
```

Replace `ResizeGameWindowToDualSpan` with a general `ResizeGameWindowToRect(HWND gh, const RECT& rect, const char* reason)`:

```cpp
void ResizeGameWindowToRect(HWND gh, const RECT& rect, const char* reason) {
    if (!gh || !IsWindow(gh)) {
        return;
    }

    const int w = static_cast<int>(rect.right - rect.left);
    const int h = static_cast<int>(rect.bottom - rect.top);

    LONG_PTR style = GetWindowLongPtrW(gh, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(gh, GWL_STYLE, style);

    SetWindowPos(gh,
                 nullptr,
                 static_cast<int>(rect.left),
                 static_cast<int>(rect.top),
                 w,
                 h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOSENDCHANGING |
                     SWP_ASYNCWINDOWPOS);

    Log("Requested game window resize (%s): hwnd=%p (%ld,%ld %dx%d)",
        reason,
        static_cast<void*>(gh),
        rect.left,
        rect.top,
        w,
        h);
}
```

- [ ] **Step 2: Start output windows hidden (`currently_visible = false`) and switch `WindowThreadProc` timer to `OutputMode::Duo`**

Update `DualOutput::Impl` fields in `src/dual_output.cpp`:

```cpp
enum class OutputMode {
    Solo,
    Duo,
};
```

In `DualOutput::Impl`:

```cpp
    MonitorDesc mon_solo{};
    MonitorDesc mon_p1{};
    MonitorDesc mon_p2{};
    bool has_monitors = false;
    ...
    std::atomic<OutputMode> mode{OutputMode::Solo};
    bool pending_duo_resize = false;
    InputTrigger input_trigger;
```

In `WindowThreadProc` (`src/dual_output.cpp`), remove the startup `ShowWindow` / `UpdateWindow` calls and track `currently_visible = false`:

```cpp
            if (self->hwnd[i]) {
                Log("Created output window %d (hidden): hwnd=%p (%ld,%ld %ldx%ld)",
                    i + 1,
                    static_cast<void*>(self->hwnd[i]),
                    targets[i]->rect.left,
                    targets[i]->rect.top,
                    targets[i]->width(),
                    targets[i]->height());
            }
        }

        SetEvent(self->windows_ready_event);
        if (self->hwnd[0]) {
            SetTimer(self->hwnd[0], 1, 200, nullptr);
        }

        bool currently_visible = false;
        auto apply_visibility = [&](bool show) { ... };

        MSG msg{};
        while (!self->stop_requested.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kMsgSetVisible) {
                apply_visibility(msg.wParam != 0);
                continue;
            }
            if (msg.message == WM_TIMER && msg.hwnd == self->hwnd[0]) {
                const HWND gh = self->game_hwnd.load();
                const ULONGLONG last_ms = self->last_present_ms.load();
                const bool game_active =
                    (!gh || (IsWindow(gh) && IsWindowVisible(gh) && !IsIconic(gh))) &&
                    (last_ms == 0 || (GetTickCount64() - last_ms) < 1500);
                apply_visibility(self->mode.load() == OutputMode::Duo && game_active);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
```

- [ ] **Step 3: Handle `Solo ↔ Duo` transitions cleanly in `DualOutput::PresentFrame`**

In `DualOutput::Start`:
```cpp
    impl_->input_trigger.SetConfig(LoadInputConfig(self_module));
    const auto monitors = QueryMonitors();
    impl_->has_monitors =
        SelectTargetMonitors(self_module, monitors, impl_->mon_solo, impl_->mon_p1, impl_->mon_p2);
    Log("Gotham Duo starting in SOLO mode (mon_solo=%ls)", impl_->mon_solo.device);
```

In `DualOutput::PresentFrame`:
```cpp
    bool f9_down = false;
    std::array<ControllerButtons, 4> slots{};
    PollSystemInput(f9_down, slots);

    const bool is_duo = (impl_->mode.load() == OutputMode::Duo);
    const TriggerAction action =
        impl_->input_trigger.Update(is_duo, f9_down, slots, GetTickCount64());

    if (action == TriggerAction::ToggleToDuo) {
        impl_->mode.store(OutputMode::Duo);
        impl_->pending_duo_resize = true;
        Log("Mode switched to DUO");
    } else if (action == TriggerAction::ToggleToSolo) {
        impl_->mode.store(OutputMode::Solo);
        impl_->pending_duo_resize = false;
        if (impl_->win_thread_id != 0) {
            PostThreadMessageW(impl_->win_thread_id, kMsgSetVisible, 0, 0);
        }
        impl_->ReleaseD3D12();
        ResizeGameWindowToRect(impl_->game_hwnd.load(), impl_->mon_solo.rect, "solo");
        Log("Mode switched to SOLO");
    }

    if (impl_->mode.load() != OutputMode::Duo) {
        return;
    }

    if (impl_->pending_duo_resize) {
        BOOL is_fullscreen = FALSE;
        if (SUCCEEDED(game_swapchain->GetFullscreenState(&is_fullscreen, nullptr)) &&
            is_fullscreen) {
            game_swapchain->SetFullscreenState(FALSE, nullptr);
            return;
        }
        impl_->pending_duo_resize = false;
        RECT span_rect{};
        span_rect.left = (std::min)(impl_->mon_p1.rect.left, impl_->mon_p2.rect.left);
        span_rect.top = (std::min)(impl_->mon_p1.rect.top, impl_->mon_p2.rect.top);
        span_rect.right = span_rect.left + impl_->mon_p1.width() + impl_->mon_p2.width();
        span_rect.bottom = span_rect.top + (std::min)(impl_->mon_p1.height(), impl_->mon_p2.height());
        ResizeGameWindowToRect(impl_->game_hwnd.load(), span_rect, "duo");
        if (impl_->win_thread_id != 0) {
            PostThreadMessageW(impl_->win_thread_id, kMsgSetVisible, 1, 0);
        }
    }
```

- [ ] **Step 4: Build `Release`, run `ctest`, and run the DLL smoke test**

Run:
```powershell
& "E:\DevTools\CMake\bin\cmake.exe" --build build --config Release
& "E:\DevTools\CMake\bin\ctest.exe" --test-dir build -C Release --output-on-failure
```
Then run the Python smoke test loading `build\Release\version.dll` and verify `gothamduo.log` shows `Starting in SOLO mode` (and no errors).

---

### Task 3: Update `config/gothamduo.ini` and `README.md`, then deploy `version.dll` for live game test

**Files:**
- Modify: `config/gothamduo.ini`
- Modify: `README.md`

- [ ] **Step 1: Add `[Input]` defaults to `config/gothamduo.ini`**

```ini
; Gotham Duo configuration
; Copy this file next to version.dll in:
;   LEGOBatmanLotDK\Binaries\Win64\gothamduo.ini

[Monitors]
; Monitor indices as logged in gothamduo.log (Monitor[0], Monitor[1], ...).
; Leave at -1 to auto-select the two landscape monitors ordered left-to-right.
Player1=-1
Player2=-1

[Input]
; Enable automatic switch on Controller 2 (Start -> Duo, hold Back+Start -> Solo).
AllowControllerToggle=1
; -1 = watch controller slots 1..3; or 0..3 for a specific XInput slot.
P2ControllerIndex=-1
; Milliseconds to hold Back+Start on Controller 2 to return to Solo mode.
HoldBackStartMs=800
```

- [ ] **Step 2: Update `README.md` `How it works` and `Installation` sections**

Document that Gotham Duo starts in **Solo mode** on the primary monitor, enters **Duo mode** (`3840x1080` split) when pressing `Start` on Controller 2 or `F9`, and returns to **Solo mode** when holding `Back + Start` (800 ms) on Controller 2 or pressing `F9`.

- [ ] **Step 3: Deploy `build\Release\version.dll` to the game folder (when game is closed) and verify end-to-end in-game**

Run:
```powershell
Copy-Item -Path "build\Release\version.dll" -Destination "D:\steam\steamapps\common\LEGO Batman - Legacy of the Dark Knight\LEGOBatmanLotDK\Binaries\Win64\version.dll" -Force
```
Verify in `gothamduo.log`:
1. Launches cleanly in Solo mode (`1920x1080`, overlays hidden).
2. `F9` switches to `Mode switched to DUO` -> `3840x1080` -> `each output=1920x1080`.
3. `F9` again switches to `Mode switched to SOLO` -> overlays hidden -> restores `1920x1080` on primary monitor.
