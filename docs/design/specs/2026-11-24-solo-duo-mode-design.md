# Solo / Duo Mode Switching Design

**Date:** 2026-11-24
**Status:** Approved for planning (local doc; intentionally uncommitted)

## Goal

Start **Gotham Duo** in **Solo mode** (single-player fullscreen borderless on the primary monitor, zero split-screen overlays or extra GPU copy overhead) and dynamically switch to **Duo mode** (`3840x1080` split across both target monitors, `1920x1080` per player) when Player 2 joins or the user toggles manually.

## Trigger Rules

- **Initial state:** `OutputMode::Solo`
- **Solo → Duo:**
  - Keyboard `F9` rising edge, **or**
  - `Start` button rising edge on controller index `1..3` (or specific index configured in `gothamduo.ini` `[Input] P2ControllerIndex`).
- **Duo → Solo:**
  - Keyboard `F9` rising edge, **or**
  - Holding `Back/View + Start` continuously for `HoldBackStartMs` (default `800 ms`) on controller index `1..3`.
- **Anti-repeat / debounce:**
  - `F9` and `Start` trigger only on rising edges (`!was_pressed && is_pressed`).
  - After `Back + Start` triggers `Duo → Solo`, controller toggles stay locked until both buttons are released so a single hold cannot immediately re-trigger `Solo → Duo`.

## Architecture

### 1. `InputTrigger` (`src/input_trigger.h`, `src/input_trigger.cpp`)

Pure state machine decoupled from Win32/XInput calls so it can be unit-tested deterministically without physical controllers:

- `InputConfig`:
  - `bool allow_controller = true`
  - `int p2_controller_index = -1` (`-1` scans slots `1..3`; `0..3` monitors only that slot)
  - `uint64_t hold_back_start_ms = 800`
- `ControllerButtons` (normalized per slot):
  - `bool connected`
  - `bool start`
  - `bool back`
- `TriggerAction`: `None`, `ToggleToDuo`, `ToggleToSolo`
- `InputTrigger::Update(bool is_duo, bool f9_down, const std::array<ControllerButtons, 4>& slots, uint64_t now_ms)`
- `PollSystemInput(...)`: thin Win32 wrapper reading `GetAsyncKeyState(VK_F9)` and `XInputGetState` (dynamically loaded via `LoadLibraryW(L"xinput1_4.dll")` with fallback `xinput9_1_0.dll`; missing XInput DLL silently leaves controller polling inactive).

### 2. `DualOutput` (`src/dual_output.h`, `src/dual_output.cpp`)

Replaces `split_enabled` / `tried_wide_resize` with explicit `OutputMode` state management:

- **Monitor tracking:**
  - `mon_p1`, `mon_p2`: left-to-right target monitors for Duo mode (from `gothamduo.ini` `[Monitors]` or auto-selected landscape monitors).
  - `mon_solo`: monitor used in Solo mode (`primary == true`, falling back to `mon_p2` / first landscape monitor).
- **Window creation (`WindowThreadProc`):**
  - Creates both borderless output windows **hidden** (no `ShowWindow` on startup; `currently_visible = false`).
  - `WM_TIMER` (`200 ms`) shows the output windows **only** when `mode == OutputMode::Duo` and the game window is alive, visible, not minimized, and actively presenting (`last_present_ms` within `1500 ms`).
- **Mode Transitions in `PresentFrame`:**
  - **In `OutputMode::Solo`:**
    - Does not create or copy to `out_swapchain[0..1]`.
    - When `ToggleToDuo` fires:
      1. If `game_swapchain->GetFullscreenState(&fs, nullptr)` reports `fs == TRUE`, calls `game_swapchain->SetFullscreenState(FALSE, nullptr)` and waits for the next frame (`pending_duo_resize = true`) so DXGI finishes leaving exclusive fullscreen cleanly.
      2. Calls `ResizeGameWindowToRect(game_hwnd, span_rect)` (borderless `WS_POPUP`, spanning `mon_p1` + `mon_p2`, e.g., `-1920,0 3840x1080`).
      3. Sets `mode = OutputMode::Duo` and posts `kMsgSetVisible(1)` to the window thread.
  - **In `OutputMode::Duo`:**
    - Copies left/right halves and presents `out_swapchain[0..1]` as today.
    - When `ToggleToSolo` fires:
      1. Sets `mode = OutputMode::Solo` and posts `kMsgSetVisible(0)` immediately to hide both output windows.
      2. Calls `ReleaseD3D12()` to free extra swapchains, command allocators, and fence VRAM.
      3. Calls `ResizeGameWindowToRect(game_hwnd, mon_solo.rect)` (`WS_POPUP` borderless covering the primary monitor, e.g., `0,0 1920x1080`).

### 3. Configuration (`config/gothamduo.ini`)

Optional `[Input]` section read alongside `[Monitors]` via `GetPrivateProfileIntW`:

```ini
[Input]
AllowControllerToggle=1
P2ControllerIndex=-1
HoldBackStartMs=800
```

## Verification Strategy

1. **Automated unit test executable (`gothamduo_tests` via CMake/CTest):**
   - Tests `InputTrigger` state transitions (`F9` toggle, `Start` rising edge on slots `1..3`, slot `0` ignored when `p2_controller_index == -1`, `Back+Start` hold `< 800 ms` vs `>= 800 ms`, release lock preventing immediate re-entry, custom `p2_controller_index`).
2. **Build + DLL smoke test:**
   - Verify `version.dll` builds cleanly in `Release` (`x64`), exports all 17 symbols, starts in `Solo` mode (`OutputMode::Solo`) with no crash.
3. **In-game verification (Steam):**
   - Game launches cleanly in `1920x1080` on the primary monitor with no overlays visible.
   - `F9` transitions to `3840x1080` across both monitors; `F9` again hides overlays and restores `1920x1080` borderless on the primary monitor.
   - Controller `Start` / `Back+Start` verified when second controller is attached.
