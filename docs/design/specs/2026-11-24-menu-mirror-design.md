# Shared Menu Mirroring (Issue #4 — Phase 1) Design

**Date:** 2026-11-24
**Issue:** [#4 — feat: replicate shared menus and subtitles across both monitors instead of spanning](https://github.com/CarlosAlexandredevv/Gotham-duo/issues/4)
**Scope:** Phase 1 covers shared full-screen / centered screens (Start/Pause menus, overlays, cutscenes) in Duo mode. Phase 2 (splitting/replicating centered subtitles and HUD over active 3D split-screen) will be planned separately after RenderDoc pass mapping.
**Status:** Approved for planning (local doc; intentionally uncommitted)

---

## Goal

When **Gotham Duo** is in **Duo mode** (`3840x1080`) and the game displays a centered/shared screen (such as the Start or Pause menu, currently split across `x = 960..2880`), mirror that `1920x1080` center region onto **both** monitors automatically, with a manual keyboard override (`F8`) and zero window resizing.

---

## Architecture

Keep the game window and swapchain at `3840x1080` at all times while in Duo mode (`OutputMode::Duo`). Avoiding `ResizeGameWindowToRect` / `ResizeBuffers` on pause/unpause prevents black-frame hitches and avoids triggering the game's resolution-revert behavior fixed in Issue #2.

```
Game Backbuffer (3840x1080, D3D12_RESOURCE_STATE_COPY_SOURCE)
  ├── [Non-blocking 16xH center-strip copy -> Readback Ring Buffer (3 slots)]
  │         │
  │         ▼ (analyzed 2-3 frames later after GPU fence completion)
  │   SplitSeamDetector::ClassifyCenterStrip()
  │         │
  │         ▼ (+ VK_F8 edge input)
  │   MirrorStateMachine::Update()
  │         │
  │         ├── is_mirroring == false (Split)
  │         │     ├── Copy [0 .. half_w)            -> out_swapchain[0] (Monitor 1)
  │         │     └── Copy [src_w - half_w .. src_w)-> out_swapchain[1] (Monitor 2)
  │         │
  │         └── is_mirroring == true (Mirror)
  │               ├── center_x = (src_w - half_w) / 2  (960 for 3840x1080)
  │               ├── Copy [center_x .. center_x + half_w) -> out_swapchain[0]
  │               └── Copy [center_x .. center_x + half_w) -> out_swapchain[1]
```

In **Solo mode** (`OutputMode::Solo`), no readback buffers are allocated and no center-strip copies are issued.

---

## Components

### 1. `SplitSeamDetector` (`src/split_seam_detector.h`, `src/split_seam_detector.cpp`)

Pure C++ module decoupled from D3D12 and Win32 so it can be unit-tested deterministically:

- **Supported pixel formats:**
  - `DXGI_FORMAT_R8G8B8A8_UNORM`, `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`
  - `DXGI_FORMAT_B8G8R8A8_UNORM`, `DXGI_FORMAT_B8G8R8A8_UNORM_SRGB`
  - `DXGI_FORMAT_R10G10B10A2_UNORM`
  - `IsSupportedSeamFormat(DXGI_FORMAT)` returns `false` for unsupported formats (e.g., FP16 HDR); `DualOutput` logs once and falls back to manual `F8` toggling without readback overhead.
- **Strip geometry:** `kSeamStripWidth = 16` pixels centered on `x = half_w` (`x = half_w - 8 .. half_w + 7`).
- **Row sampling & seam classification (`ClassifyCenterStrip`):**
  - Samples rows from `y = height / 8` to `height - height / 8` with step `4` (avoiding top/bottom letterboxing or HUD edge artifacts).
  - Decodes `p[0..15]` to `[0..255]` RGB and measures L1 color distance `dist(a, b) = |dr| + |dg| + |db|` (`0..765`).
  - A row is **active** if `max(dist(p[7], p[8]), dist(p[3], p[12]), dist(p[1], p[3]), dist(p[12], p[14])) >= 18`.
  - An active row has a **split seam** if either:
    - `dist(p[7], p[8]) > max_local * 2 + 45` (direct viewport cut at `x = half_w`), or
    - `dist(p[3], p[12]) > max(dist(p[1], p[3]), dist(p[12], p[14])) * 2 + 60` (catches a 1–4 px vertical divider line or blended seam between halves),
    where `max_local = max(dist(p[1], p[3]), dist(p[12], p[14]), dist(p[5], p[6]), dist(p[9], p[10]))`.
  - Strip classification (`StripClassification`):
    - `Ambiguous` if `active_rows < sampled_rows / 6` (uniform/solid screen such as black loading or fade transitions) or `0.20 < seam_ratio < 0.35`.
    - `Split` if `seam_ratio >= 0.35`.
    - `Shared` if `seam_ratio <= 0.20`.

### 2. `MirrorStateMachine` (`src/split_seam_detector.h`, `src/split_seam_detector.cpp`)

Pure state machine combining `StripClassification` hysteresis with manual `F8` override:

- **Config (`MirrorConfig`):**
  - `bool auto_detect = true`
  - `uint32_t enter_mirror_ms = 150`
  - `uint32_t exit_mirror_ms = 100`
- **Override modes (`MirrorOverrideMode`):**
  - `Auto`: starts in `is_mirroring = false`.
    - Continuous `Shared` samples for `>= enter_mirror_ms` switch `is_mirroring` to `true`.
    - Continuous `Split` samples for `>= exit_mirror_ms` switch `is_mirroring` to `false`.
    - `Ambiguous` samples pause/reset candidate accumulation without flipping state (preserving current output across fades/loading screens).
    - Non-monotonic timestamps (`now_ms < candidate_start_ms`) safely reset `candidate_start_ms` to `now_ms`.
  - `ManualMirror` / `ManualSplit`: ignores detector samples and locks `is_mirroring` to `true` / `false`.
- **`F8` rising-edge behavior:**
  - When `auto_detect == true`:
    - If in `Auto`: switches to the opposite manual lock (`ManualMirror` if currently splitting, `ManualSplit` if currently mirroring).
    - If in `ManualMirror` or `ManualSplit`: returns to `Auto` (and resets pending hysteresis timers).
  - When `auto_detect == false`:
    - Toggles directly between `ManualSplit` and `ManualMirror`.
- **`Reset()`:** Clears hysteresis timers and restores `Auto` (when `auto_detect == true`) or `ManualSplit` (when `auto_detect == false`), with `is_mirroring = false`. Called on `ToggleToDuo`, `ToggleToSolo`, and `ReleaseD3D12Locked`.

### 3. `DualOutput` D3D12 Integration (`src/dual_output.cpp`)

- **INI loading (`LoadMirrorConfig`):** Reads `[MenuMirror]` (`AutoDetect`, `EnterMirrorMs`, `ExitMirrorMs`) from `gothamduo.ini` in `DualOutput::Start`.
- **Readback ring buffer (`kFrameSlots = 3`):**
  - Created inside `InitD3D12Locked` only when `mirror_config.auto_detect && IsSupportedSeamFormat(desc.Format) && desc.Width >= kSeamStripWidth && (desc.Width / 2) >= (kSeamStripWidth / 2)`.
  - Uses `device->GetCopyableFootprints` for a `16 x src_height` sub-region footprint and allocates `D3D12_HEAP_TYPE_READBACK` buffers (`readback_buf[kFrameSlots]`) plus `readback_valid[kFrameSlots]`.
  - Freed inside `ReleaseD3D12Locked`.
- **Per-frame flow in `PresentFrame` / `CopyAndPresent`:**
  - Polls `VK_F8` (`(GetAsyncKeyState(VK_F8) & 0x8000) != 0`) every frame in Duo mode.
  - Inside `CopyAndPresent`, after waiting on `slot_fence_values[slot]`:
    - If `readback_valid[slot]` is true and `readback_buf[slot]` is allocated, maps `readback_buf[slot]`, calls `ClassifyCenterStrip`, unmaps with an empty write range, and updates `mirror_state`.
    - Otherwise updates `mirror_state` with `std::nullopt` so `F8` remains responsive on every frame.
  - Chooses `boxes[0..1]` based on `mirror_state.is_mirroring()`:
    - Split: `{0, 0, 0, half_w, src_height, 1}` and `{src_width - half_w, 0, 0, src_width, src_height, 1}`.
    - Mirror: `{center_x, 0, 0, center_x + half_w, src_height, 1}` for both outputs, where `center_x = (src_width - half_w) / 2`.
  - While `src_buf` is in `D3D12_RESOURCE_STATE_COPY_SOURCE`, records `CopyTextureRegion` of `{half_w - kSeamStripWidth / 2, 0, 0, half_w + kSeamStripWidth / 2, src_height, 1}` into `readback_buf[slot]` and marks `readback_valid[slot] = true`.
  - Logs mode/mirror state transitions (`DUO mirror: SPLIT -> MIRROR (auto)` / `(manual F8)`).

### 4. Configuration (`config/gothamduo.ini`) & Docs (`README.md`)

Add optional `[MenuMirror]` section to `config/gothamduo.ini`:

```ini
[MenuMirror]
; Automatically mirror centered/shared menus in Duo mode (1 = enabled, 0 = manual F8 only).
AutoDetect=1
; Milliseconds a shared screen must be stable before switching to Mirror.
EnterMirrorMs=150
; Milliseconds a split-screen seam must be stable before returning to Split.
ExitMirrorMs=100
```

---

## Error Handling & Edge Cases

- **Unsupported backbuffer format:** Logs once (`MenuMirror auto-detect disabled: unsupported DXGI_FORMAT %u (use F8 manual toggle).`) and continues normal Duo presentation with manual `F8` mirroring still working.
- **Readback allocation or `Map` failure:** Logs once and disables readback for the current pipeline instance without affecting output presentation.
- **Swapchain resize (`ResizeBuffers`) or Solo switch:** `ReleaseD3D12Locked()` waits for GPU idle, releases readback buffers, clears `readback_valid`, and resets `MirrorStateMachine`.
- **Dark loading screens / fades:** Classified as `Ambiguous`, holding the current Split/Mirror state until real frame content returns.

---

## Verification Strategy

1. **Automated unit tests (`tests/split_seam_detector_test.cpp` via CMake/CTest):**
   - `ClassifyCenterStrip`:
     - Continuous horizontal gradient / centered menu box over split-screen -> `Shared`
     - Distinct left/right scenes (direct seam) -> `Split`
     - Distinct left/right scenes separated by a 2 px solid vertical divider bar -> `Split`
     - Solid black / flat screen -> `Ambiguous`
     - `BGRA8`, `RGBA8`, and `R10G10B10A2_UNORM` formats, plus unsupported format check (`IsSupportedSeamFormat`)
   - `MirrorStateMachine`:
     - `EnterMirrorMs` (`150 ms`) and `ExitMirrorMs` (`100 ms`) hysteresis boundaries
     - `Ambiguous` samples preserve current state and pause/reset candidate accumulation
     - Non-monotonic timestamp safety
     - `F8` toggle behavior (`Auto <-> ManualMirror / ManualSplit`, and `ManualSplit <-> ManualMirror` when `auto_detect == false`)
2. **Build & CTest check:**
   - `cmake --build build --config Release`
   - `ctest --test-dir build -C Release --output-on-failure`
3. **In-game verification (Steam, `3840x1080` Duo):**
   - Enter Duo mode (`F9` or Controller 2 `Start`) -> confirm gameplay stays in Split (`P1 | P2`).
   - Open Start/Pause menu -> confirm both monitors automatically mirror the centered `1920x1080` menu cleanly.
   - Close menu -> confirm split-screen resumes within ~100 ms with no flicker or resolution revert.
   - Press `F8` during gameplay and in menu -> confirm manual override and return to `Auto`.
