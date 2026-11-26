# Shared Menu Mirroring (Issue #4 — Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatically detect shared/centered menus (such as Start/Pause) in Duo mode (`3840x1080`) using a non-blocking D3D12 center-strip readback and mirror the centered `1920x1080` region (`960..2880`) onto both monitors, with an `F8` manual override and `[MenuMirror]` INI config.

**Architecture:** Extract pure pixel-seam classification (`ClassifyCenterStrip`, `IsSupportedSeamFormat`) and hysteresis + `F8` override logic (`MirrorStateMachine`) into `src/split_seam_detector.{h,cpp}` with zero Win32/D3D12 runtime dependencies so all edge cases are verified in CTest. Then wire a 3-slot `D3D12_HEAP_TYPE_READBACK` ring buffer (`16 x H` center strip) and conditional `CopyTextureRegion` source box selection into `DualOutput` (`src/dual_output.cpp`), keeping the game at `3840x1080` with zero `ResizeBuffers` calls.

**Tech Stack:** C++20, MSVC x64, Windows SDK (`dxgiformat.h`, `d3d12.h`, `dxgi1_5.h`, `Win32`), CMake / CTest.

---

### File Structure

- **Create:** `src/split_seam_detector.h` — `StripClassification`, `MirrorConfig`, `MirrorOverrideMode`, `MirrorUpdateResult`, `IsSupportedSeamFormat`, `ClassifyCenterStrip`, and `MirrorStateMachine`.
- **Create:** `src/split_seam_detector.cpp` — Pixel decoding (`RGBA8`, `BGRA8`, `R10G10B10A2`), seam classification across sampled center rows, and hysteresis + `F8` state machine.
- **Create:** `tests/split_seam_detector_test.cpp` — Standalone assert-based unit tests (`split_seam_detector_tests.exe`) covering all formats, seam patterns, hysteresis timers, and `F8` overrides.
- **Modify:** `CMakeLists.txt` — Add `src/split_seam_detector.cpp` to `gothamduo` and register `split_seam_detector_tests` via `add_test()`.
- **Modify:** `src/dual_output.cpp` — Add `[MenuMirror]` INI loading, `VK_F8` polling, 3-slot `16 x H` readback ring buffer in `InitD3D12Locked` / `ReleaseD3D12Locked`, and mirrored `CopyTextureRegion` box selection in `CopyAndPresent`.
- **Modify:** `config/gothamduo.ini` and `README.md` — Document `[MenuMirror]` settings and `F8` manual menu-mirror toggle.

---

### Task 1: Add `SplitSeamDetector` (`IsSupportedSeamFormat` and `ClassifyCenterStrip`) with TDD

**Files:**
- Create: `src/split_seam_detector.h`
- Create: `src/split_seam_detector.cpp`
- Create: `tests/split_seam_detector_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `src/split_seam_detector.h`, a minimal stub `src/split_seam_detector.cpp`, `tests/split_seam_detector_test.cpp` (Part 1), and register in `CMakeLists.txt`**

Create `src/split_seam_detector.h`:

```cpp
#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <optional>

namespace gotham {

inline constexpr uint32_t kSeamStripWidth = 16;

enum class StripClassification {
    Split,
    Shared,
    Ambiguous,
};

enum class MirrorOverrideMode {
    Auto,
    ManualMirror,
    ManualSplit,
};

struct MirrorConfig {
    bool auto_detect = true;
    uint32_t enter_mirror_ms = 150;
    uint32_t exit_mirror_ms = 100;
};

struct MirrorUpdateResult {
    bool is_mirroring = false;
    bool transitioned = false;
    const char* reason = "";
};

/// Returns true for 32bpp DXGI formats supported by ClassifyCenterStrip.
bool IsSupportedSeamFormat(DXGI_FORMAT format);

/// Analyzes a kSeamStripWidth-wide vertical strip centered on the split boundary.
StripClassification ClassifyCenterStrip(const void* data,
                                        uint32_t row_pitch,
                                        uint32_t height,
                                        DXGI_FORMAT format);

/// Pure state machine combining seam classification hysteresis with F8 manual overrides.
class MirrorStateMachine {
public:
    void SetConfig(MirrorConfig config);
    const MirrorConfig& config() const { return config_; }

    void Reset();

    MirrorUpdateResult Update(bool f8_down,
                              std::optional<StripClassification> sample,
                              uint64_t now_ms);

    bool is_mirroring() const { return is_mirroring_; }
    MirrorOverrideMode override_mode() const { return override_mode_; }

private:
    MirrorConfig config_{};
    MirrorOverrideMode override_mode_ = MirrorOverrideMode::Auto;
    bool is_mirroring_ = false;
    bool prev_f8_down_ = false;
    std::optional<StripClassification> candidate_kind_;
    std::optional<uint64_t> candidate_start_ms_;
};

}  // namespace gotham
```

Create stub `src/split_seam_detector.cpp`:

```cpp
#include "split_seam_detector.h"

namespace gotham {

bool IsSupportedSeamFormat(DXGI_FORMAT) {
    return false;
}

StripClassification ClassifyCenterStrip(const void*, uint32_t, uint32_t, DXGI_FORMAT) {
    return StripClassification::Ambiguous;
}

void MirrorStateMachine::SetConfig(MirrorConfig config) {
    config_ = config;
    Reset();
}

void MirrorStateMachine::Reset() {
    override_mode_ = config_.auto_detect ? MirrorOverrideMode::Auto
                                         : MirrorOverrideMode::ManualSplit;
    is_mirroring_ = false;
    prev_f8_down_ = false;
    candidate_kind_.reset();
    candidate_start_ms_.reset();
}

MirrorUpdateResult MirrorStateMachine::Update(bool,
                                              std::optional<StripClassification>,
                                              uint64_t) {
    return MirrorUpdateResult{is_mirroring_, false, ""};
}

}  // namespace gotham
```

Create `tests/split_seam_detector_test.cpp`:

```cpp
#include "split_seam_detector.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

namespace {

using gotham::ClassifyCenterStrip;
using gotham::IsSupportedSeamFormat;
using gotham::kSeamStripWidth;
using gotham::StripClassification;

struct Rgb8 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

uint32_t PackPixel(Rgb8 c, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return static_cast<uint32_t>(c.b) | (static_cast<uint32_t>(c.g) << 8) |
               (static_cast<uint32_t>(c.r) << 16) | 0xFF000000u;
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const uint32_t r10 = static_cast<uint32_t>(c.r) << 2;
        const uint32_t g10 = static_cast<uint32_t>(c.g) << 2;
        const uint32_t b10 = static_cast<uint32_t>(c.b) << 2;
        return r10 | (g10 << 10) | (b10 << 20) | (3u << 30);
    }
    return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) |
           (static_cast<uint32_t>(c.b) << 16) | 0xFF000000u;
}

struct StripBuffer {
    uint32_t row_pitch = 256;
    uint32_t height = 1080;
    std::vector<uint8_t> bytes;

    StripBuffer(uint32_t h = 1080, uint32_t pitch = 256)
        : row_pitch(pitch), height(h), bytes(static_cast<size_t>(pitch) * h, 0) {}

    void SetPixel(uint32_t x, uint32_t y, Rgb8 c, DXGI_FORMAT fmt) {
        auto* row = reinterpret_cast<uint32_t*>(bytes.data() + static_cast<size_t>(y) * row_pitch);
        row[x] = PackPixel(c, fmt);
    }
};

void FillRowSplit(StripBuffer& buf, uint32_t y, Rgb8 left, Rgb8 right, DXGI_FORMAT fmt) {
    for (uint32_t x = 0; x < kSeamStripWidth; ++x) {
        buf.SetPixel(x, y, x < 8 ? left : right, fmt);
    }
}

void FillRowSharedGradient(StripBuffer& buf, uint32_t y, Rgb8 base, DXGI_FORMAT fmt) {
    for (uint32_t x = 0; x < kSeamStripWidth; ++x) {
        const uint8_t r = static_cast<uint8_t>(base.r + x * 2);
        const uint8_t g = static_cast<uint8_t>(base.g + x);
        const uint8_t b = static_cast<uint8_t>(base.b + x * 2);
        buf.SetPixel(x, y, Rgb8{r, g, b}, fmt);
    }
}

void TestSupportedFormats() {
    CHECK(IsSupportedSeamFormat(DXGI_FORMAT_R8G8B8A8_UNORM));
    CHECK(IsSupportedSeamFormat(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
    CHECK(IsSupportedSeamFormat(DXGI_FORMAT_B8G8R8A8_UNORM));
    CHECK(IsSupportedSeamFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
    CHECK(IsSupportedSeamFormat(DXGI_FORMAT_R10G10B10A2_UNORM));
    CHECK(!IsSupportedSeamFormat(DXGI_FORMAT_R16G16B16A16_FLOAT));
    CHECK(!IsSupportedSeamFormat(DXGI_FORMAT_UNKNOWN));
}

void TestClassifySplitAndSharedAcrossFormats() {
    const std::array<DXGI_FORMAT, 3> formats = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,
    };

    for (DXGI_FORMAT fmt : formats) {
        // 1. Direct split seam between P1 and P2 views.
        StripBuffer split_buf;
        for (uint32_t y = 0; y < split_buf.height; ++y) {
            const uint8_t vary = static_cast<uint8_t>((y * 3) % 40);
            FillRowSplit(split_buf, y, Rgb8{20, static_cast<uint8_t>(40 + vary), 200},
                         Rgb8{210, static_cast<uint8_t>(170 + vary), 30}, fmt);
        }
        CHECK(ClassifyCenterStrip(split_buf.bytes.data(), split_buf.row_pitch, split_buf.height,
                                  fmt) == StripClassification::Split);

        // 2. Split screen with a solid 2px black divider bar at columns 7 and 8.
        StripBuffer bar_buf = split_buf;
        for (uint32_t y = 0; y < bar_buf.height; ++y) {
            bar_buf.SetPixel(7, y, Rgb8{0, 0, 0}, fmt);
            bar_buf.SetPixel(8, y, Rgb8{0, 0, 0}, fmt);
        }
        CHECK(ClassifyCenterStrip(bar_buf.bytes.data(), bar_buf.row_pitch, bar_buf.height, fmt) ==
              StripClassification::Split);

        // 3. Shared menu/screen: smooth gradient across center seam.
        StripBuffer shared_buf;
        for (uint32_t y = 0; y < shared_buf.height; ++y) {
            FillRowSharedGradient(shared_buf, y, Rgb8{40, 80, 120}, fmt);
        }
        CHECK(ClassifyCenterStrip(shared_buf.bytes.data(), shared_buf.row_pitch,
                                  shared_buf.height, fmt) == StripClassification::Shared);
    }
}

void TestCenteredMenuOverSplitAndUniformBlack() {
    constexpr DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    // 4. Solid uniform black loading/fade frame -> Ambiguous.
    StripBuffer black_buf;
    CHECK(ClassifyCenterStrip(black_buf.bytes.data(), black_buf.row_pitch, black_buf.height,
                              fmt) == StripClassification::Ambiguous);

    // 5. Centered Pause menu covering middle 75% of rows over split-screen -> Shared.
    StripBuffer menu_buf;
    for (uint32_t y = 0; y < menu_buf.height; ++y) {
        if (y >= 200 && y < 880) {
            FillRowSharedGradient(menu_buf, y, Rgb8{35, 45, 70}, fmt);
        } else {
            FillRowSplit(menu_buf, y, Rgb8{15, 50, 210}, Rgb8{220, 160, 20}, fmt);
        }
    }
    CHECK(ClassifyCenterStrip(menu_buf.bytes.data(), menu_buf.row_pitch, menu_buf.height, fmt) ==
          StripClassification::Shared);
}

}  // namespace

int main() {
    TestSupportedFormats();
    TestClassifySplitAndSharedAcrossFormats();
    TestCenteredMenuOverSplitAndUniformBlack();
    std::printf("All split_seam_detector_tests passed.\n");
    return 0;
}
```

Edit `CMakeLists.txt` to include `src/split_seam_detector.cpp` in `gothamduo` and add `split_seam_detector_tests`:

In `CMakeLists.txt`, update `add_library(gothamduo SHARED ...)`:
```cmake
add_library(gothamduo SHARED
    src/dllmain.cpp
    src/dual_output.cpp
    src/dxgi_hook.cpp
    src/input_trigger.cpp
    src/split_seam_detector.cpp
    src/version_proxy.cpp
    src/version_trampolines.asm
    src/version.def
)
```
And append at the bottom of `CMakeLists.txt`:
```cmake
add_executable(split_seam_detector_tests
    tests/split_seam_detector_test.cpp
    src/split_seam_detector.cpp
)

set_target_properties(split_seam_detector_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_include_directories(split_seam_detector_tests PRIVATE
    src
)

target_compile_definitions(split_seam_detector_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

add_test(NAME split_seam_detector_tests COMMAND split_seam_detector_tests)
```

- [ ] **Step 2: Build and run `split_seam_detector_tests` to verify it fails**

Run:
```powershell
cmake -B build -A x64 && cmake --build build --config Release --target split_seam_detector_tests && ctest --test-dir build -C Release -R split_seam_detector_tests --output-on-failure
```
Expected: FAIL (`CHECK failed` on `IsSupportedSeamFormat(DXGI_FORMAT_R8G8B8A8_UNORM)`).

- [ ] **Step 3: Implement `IsSupportedSeamFormat` and `ClassifyCenterStrip` in `src/split_seam_detector.cpp`**

Replace `IsSupportedSeamFormat` and `ClassifyCenterStrip` in `src/split_seam_detector.cpp`:

```cpp
#include "split_seam_detector.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace gotham {

namespace {

struct Rgb8 {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb8 DecodePixel(uint32_t raw, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return Rgb8{
            static_cast<int>((raw & 0x3FFu) >> 2),
            static_cast<int>(((raw >> 10) & 0x3FFu) >> 2),
            static_cast<int>(((raw >> 20) & 0x3FFu) >> 2),
        };
    }
    // For both RGBA8 and BGRA8, channel-wise L1 differences are permutation-invariant.
    return Rgb8{
        static_cast<int>(raw & 0xFFu),
        static_cast<int>((raw >> 8) & 0xFFu),
        static_cast<int>((raw >> 16) & 0xFFu),
    };
}

int ColorDistL1(Rgb8 a, Rgb8 b) {
    return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
}

}  // namespace

bool IsSupportedSeamFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return true;
        default:
            return false;
    }
}

StripClassification ClassifyCenterStrip(const void* data,
                                        uint32_t row_pitch,
                                        uint32_t height,
                                        DXGI_FORMAT format) {
    if (!data || !IsSupportedSeamFormat(format) || height < 16 ||
        row_pitch < kSeamStripWidth * sizeof(uint32_t)) {
        return StripClassification::Ambiguous;
    }

    const auto* base = static_cast<const uint8_t*>(data);
    const uint32_t y_start = height / 8;
    const uint32_t y_end = height - y_start;
    constexpr uint32_t kRowStep = 4;

    uint32_t sampled_rows = 0;
    uint32_t active_rows = 0;
    uint32_t seam_rows = 0;

    for (uint32_t y = y_start; y < y_end; y += kRowStep) {
        ++sampled_rows;
        const auto* row =
            reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * row_pitch);

        std::array<Rgb8, kSeamStripWidth> px{};
        for (uint32_t x = 0; x < kSeamStripWidth; ++x) {
            px[x] = DecodePixel(row[x], format);
        }

        const int local_left = ColorDistL1(px[1], px[3]);
        const int local_right = ColorDistL1(px[12], px[14]);
        const int near_left = ColorDistL1(px[5], px[6]);
        const int near_right = ColorDistL1(px[9], px[10]);
        const int seam_immediate = ColorDistL1(px[7], px[8]);
        const int seam_wide = ColorDistL1(px[3], px[12]);

        const int local_max = std::max({local_left, local_right, near_left, near_right});
        const int outer_local_max = std::max(local_left, local_right);
        const int row_activity = std::max({seam_immediate, seam_wide, local_max});
        if (row_activity < 18) {
            continue;
        }

        ++active_rows;
        const bool direct_seam = seam_immediate > (local_max * 2 + 45);
        const bool wide_seam = seam_wide > (outer_local_max * 2 + 60);
        if (direct_seam || wide_seam) {
            ++seam_rows;
        }
    }

    if (sampled_rows == 0 || active_rows < (sampled_rows / 6)) {
        return StripClassification::Ambiguous;
    }

    const double seam_ratio = static_cast<double>(seam_rows) / static_cast<double>(active_rows);
    if (seam_ratio >= 0.35) {
        return StripClassification::Split;
    }
    if (seam_ratio <= 0.20) {
        return StripClassification::Shared;
    }
    return StripClassification::Ambiguous;
}

```

- [ ] **Step 4: Build and run `split_seam_detector_tests` to verify it passes**

Run:
```powershell
cmake --build build --config Release --target split_seam_detector_tests && ctest --test-dir build -C Release -R split_seam_detector_tests --output-on-failure
```
Expected: `All split_seam_detector_tests passed.` and `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/split_seam_detector.h src/split_seam_detector.cpp tests/split_seam_detector_test.cpp
git commit -m "feat: add SplitSeamDetector for Duo menu mirroring"
```

---

### Task 2: Implement `MirrorStateMachine` (hysteresis + `F8` manual override) with TDD

**Files:**
- Modify: `tests/split_seam_detector_test.cpp`
- Modify: `src/split_seam_detector.cpp`

- [ ] **Step 1: Add `MirrorStateMachine` unit tests to `tests/split_seam_detector_test.cpp`**

Add these `using` declarations and tests to `tests/split_seam_detector_test.cpp`, and invoke them from `main()`:

```cpp
using gotham::MirrorConfig;
using gotham::MirrorOverrideMode;
using gotham::MirrorStateMachine;
using gotham::MirrorUpdateResult;

void TestMirrorHysteresisAndAmbiguousHold() {
    MirrorStateMachine sm;
    MirrorConfig cfg{};
    cfg.auto_detect = true;
    cfg.enter_mirror_ms = 150;
    cfg.exit_mirror_ms = 100;
    sm.SetConfig(cfg);

    // Starts in Auto + Split.
    CHECK(!sm.is_mirroring());
    CHECK(sm.override_mode() == MirrorOverrideMode::Auto);

    // Shared samples enter Mirror only after 150 ms.
    CHECK(!sm.Update(false, StripClassification::Shared, 1000).is_mirroring);
    CHECK(!sm.Update(false, StripClassification::Shared, 1149).is_mirroring);
    MirrorUpdateResult entered = sm.Update(false, StripClassification::Shared, 1150);
    CHECK(entered.is_mirroring);
    CHECK(entered.transitioned);

    // Ambiguous frames (e.g. black loading screen) keep Mirror active and reset exit timer.
    CHECK(sm.Update(false, StripClassification::Split, 1200).is_mirroring);
    CHECK(sm.Update(false, StripClassification::Ambiguous, 1250).is_mirroring);
    CHECK(sm.Update(false, StripClassification::Split, 1300).is_mirroring);
    CHECK(sm.Update(false, StripClassification::Split, 1399).is_mirroring);
    MirrorUpdateResult exited = sm.Update(false, StripClassification::Split, 1400);
    CHECK(!exited.is_mirroring);
    CHECK(exited.transitioned);

    // Non-monotonic timestamp resets candidate start without underflow.
    CHECK(!sm.Update(false, StripClassification::Shared, 2000).is_mirroring);
    CHECK(!sm.Update(false, StripClassification::Shared, 1900).is_mirroring);
    CHECK(!sm.Update(false, StripClassification::Shared, 2049).is_mirroring);
    CHECK(sm.Update(false, StripClassification::Shared, 2050).is_mirroring);
}

void TestMirrorF8AutoAndManualOverrides() {
    MirrorStateMachine sm;
    MirrorConfig cfg{};
    cfg.auto_detect = true;
    cfg.enter_mirror_ms = 150;
    cfg.exit_mirror_ms = 100;
    sm.SetConfig(cfg);

    // In Auto + Split, F8 rising edge locks ManualMirror; holding F8 does not retrigger.
    MirrorUpdateResult r = sm.Update(true, std::nullopt, 100);
    CHECK(r.is_mirroring && r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualMirror);

    r = sm.Update(true, StripClassification::Split, 300);
    CHECK(r.is_mirroring && !r.transitioned);

    // Release F8, then press F8 again -> returns to Auto (still mirroring until Split settles).
    CHECK(sm.Update(false, std::nullopt, 310).is_mirroring);
    r = sm.Update(true, std::nullopt, 320);
    CHECK(sm.override_mode() == MirrorOverrideMode::Auto);
    CHECK(r.is_mirroring && !r.transitioned);

    // After 100 ms of Split in Auto, returns to Split.
    CHECK(sm.Update(false, StripClassification::Split, 330).is_mirroring);
    CHECK(!sm.Update(false, StripClassification::Split, 430).is_mirroring);

    // AutoDetect=0 toggles directly ManualSplit <-> ManualMirror on F8 rising edge.
    cfg.auto_detect = false;
    sm.SetConfig(cfg);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualSplit);
    CHECK(!sm.Update(false, StripClassification::Shared, 1000).is_mirroring);
    r = sm.Update(true, std::nullopt, 1010);
    CHECK(r.is_mirroring && r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualMirror);
    CHECK(sm.Update(false, std::nullopt, 1020).is_mirroring);
    r = sm.Update(true, std::nullopt, 1030);
    CHECK(!r.is_mirroring && r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualSplit);

    // Reset restores Auto + Split (when auto_detect=true).
    cfg.auto_detect = true;
    sm.SetConfig(cfg);
    sm.Update(true, std::nullopt, 1100);
    CHECK(sm.is_mirroring());
    sm.Reset();
    CHECK(!sm.is_mirroring());
    CHECK(sm.override_mode() == MirrorOverrideMode::Auto);
}
```

Update `main()` in `tests/split_seam_detector_test.cpp`:
```cpp
int main() {
    TestSupportedFormats();
    TestClassifySplitAndSharedAcrossFormats();
    TestCenteredMenuOverSplitAndUniformBlack();
    TestMirrorHysteresisAndAmbiguousHold();
    TestMirrorF8AutoAndManualOverrides();
    std::printf("All split_seam_detector_tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Build and run `split_seam_detector_tests` to verify the new tests fail**

Run:
```powershell
cmake --build build --config Release --target split_seam_detector_tests && ctest --test-dir build -C Release -R split_seam_detector_tests --output-on-failure
```
Expected: FAIL (`CHECK failed` in `TestMirrorHysteresisAndAmbiguousHold`).

- [ ] **Step 3: Implement `MirrorStateMachine::Update` in `src/split_seam_detector.cpp`**

Replace `MirrorStateMachine::Update` in `src/split_seam_detector.cpp`:

```cpp
MirrorUpdateResult MirrorStateMachine::Update(bool f8_down,
                                              std::optional<StripClassification> sample,
                                              uint64_t now_ms) {
    const bool f8_rising = f8_down && !prev_f8_down_;
    prev_f8_down_ = f8_down;

    if (f8_rising) {
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        const bool was_mirroring = is_mirroring_;

        if (!config_.auto_detect) {
            is_mirroring_ = !is_mirroring_;
            override_mode_ = is_mirroring_ ? MirrorOverrideMode::ManualMirror
                                           : MirrorOverrideMode::ManualSplit;
            return MirrorUpdateResult{is_mirroring_, is_mirroring_ != was_mirroring, "manual F8"};
        }

        if (override_mode_ == MirrorOverrideMode::Auto) {
            is_mirroring_ = !is_mirroring_;
            override_mode_ = is_mirroring_ ? MirrorOverrideMode::ManualMirror
                                           : MirrorOverrideMode::ManualSplit;
            return MirrorUpdateResult{is_mirroring_, is_mirroring_ != was_mirroring, "manual F8"};
        }

        override_mode_ = MirrorOverrideMode::Auto;
        return MirrorUpdateResult{is_mirroring_, false, "auto resumed"};
    }

    if (override_mode_ != MirrorOverrideMode::Auto || !config_.auto_detect ||
        !sample.has_value()) {
        return MirrorUpdateResult{is_mirroring_, false, ""};
    }

    const StripClassification cls = *sample;
    if (cls == StripClassification::Ambiguous) {
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        return MirrorUpdateResult{is_mirroring_, false, ""};
    }

    const bool wants_mirror = (cls == StripClassification::Shared);
    if (wants_mirror == is_mirroring_) {
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        return MirrorUpdateResult{is_mirroring_, false, ""};
    }

    if (!candidate_kind_.has_value() || *candidate_kind_ != cls ||
        !candidate_start_ms_.has_value() || now_ms < *candidate_start_ms_) {
        candidate_kind_ = cls;
        candidate_start_ms_ = now_ms;
    }

    const uint64_t hold_ms =
        wants_mirror ? static_cast<uint64_t>(config_.enter_mirror_ms)
                     : static_cast<uint64_t>(config_.exit_mirror_ms);
    if ((now_ms - *candidate_start_ms_) >= hold_ms) {
        is_mirroring_ = wants_mirror;
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        return MirrorUpdateResult{is_mirroring_, true, "auto"};
    }

    return MirrorUpdateResult{is_mirroring_, false, ""};
}
```

- [ ] **Step 4: Build and run all unit tests to verify they pass**

Run:
```powershell
cmake --build build --config Release --target split_seam_detector_tests && ctest --test-dir build -C Release --output-on-failure
```
Expected: `100% tests passed, 0 tests failed out of 3` (`input_trigger_tests`, `duo_resize_guard_tests`, `split_seam_detector_tests`).

- [ ] **Step 5: Commit**

```bash
git add src/split_seam_detector.cpp tests/split_seam_detector_test.cpp
git commit -m "feat: add MirrorStateMachine hysteresis and F8 override"
```

---

### Task 3: Wire center-strip readback and mirrored `CopyTextureRegion` into `DualOutput`

**Files:**
- Modify: `src/dual_output.cpp`

- [ ] **Step 1: Add `[MenuMirror]` INI loader, `seam_readback` resources, and `MirrorStateMachine` wiring to `src/dual_output.cpp`**

1. At top of `src/dual_output.cpp`, include `"split_seam_detector.h"`:
```cpp
#include "dual_output.h"
#include "duo_resize_guard.h"
#include "input_trigger.h"
#include "log.h"
#include "split_seam_detector.h"
```

2. Right after `LoadInputConfig` (`src/dual_output.cpp:70`), add `LoadMirrorConfig`:
```cpp
MirrorConfig LoadMirrorConfig(HMODULE self_module) {
    MirrorConfig cfg{};
    wchar_t ini_path[MAX_PATH] = {};
    if (!BuildIniPath(self_module, ini_path)) {
        return cfg;
    }

    cfg.auto_detect = GetPrivateProfileIntW(L"MenuMirror", L"AutoDetect", 1, ini_path) != 0;
    const int enter_ms = static_cast<int>(
        GetPrivateProfileIntW(L"MenuMirror", L"EnterMirrorMs", 150, ini_path));
    const int exit_ms = static_cast<int>(
        GetPrivateProfileIntW(L"MenuMirror", L"ExitMirrorMs", 100, ini_path));
    cfg.enter_mirror_ms = static_cast<uint32_t>(enter_ms < 0 ? 150 : enter_ms);
    cfg.exit_mirror_ms = static_cast<uint32_t>(exit_ms < 0 ? 100 : exit_ms);
    return cfg;
}
```

3. In `DualOutput::Impl` (`src/dual_output.cpp:254-265`), add readback and mirror state members alongside `out_swapchain`:
```cpp
    ComPtr<IDXGISwapChain3> out_swapchain[2];
    ComPtr<ID3D12Resource> seam_readback[kFrameSlots];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT seam_footprint{};
    UINT64 seam_readback_bytes = 0;
    bool seam_readback_valid[kFrameSlots]{};
    bool seam_readback_ready = false;
    bool logged_seam_unsupported = false;
    bool logged_seam_readback_err = false;
    MirrorStateMachine mirror_state;
```

4. In `ReleaseD3D12Locked()` (`src/dual_output.cpp:377-398`), release readback buffers and reset `mirror_state`:
```cpp
    void ReleaseD3D12Locked() {
        WaitForGpuIdle();
        for (auto& sc : out_swapchain) {
            sc.Reset();
        }
        for (UINT i = 0; i < kFrameSlots; ++i) {
            seam_readback[i].Reset();
            seam_readback_valid[i] = false;
        }
        seam_footprint = {};
        seam_readback_bytes = 0;
        seam_readback_ready = false;
        logged_seam_readback_err = false;
        mirror_state.Reset();
        cmd_list.Reset();
        for (auto& alloc : allocators) {
            alloc.Reset();
        }
        fence.Reset();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        device.Reset();
        pipeline_ready = false;
        tearing_supported = false;
        logged_runtime_err = false;
        src_width = 0;
        src_height = 0;
        src_format = DXGI_FORMAT_UNKNOWN;
    }
```

5. Add `InitSeamReadbackLocked(const DXGI_SWAP_CHAIN_DESC1& desc)` inside `DualOutput::Impl` and call it at the end of `InitD3D12Locked` (before setting `pipeline_ready = true`):
```cpp
    void InitSeamReadbackLocked(const DXGI_SWAP_CHAIN_DESC1& desc) {
        seam_readback_ready = false;
        for (UINT i = 0; i < kFrameSlots; ++i) {
            seam_readback[i].Reset();
            seam_readback_valid[i] = false;
        }
        if (!mirror_state.config().auto_detect) {
            return;
        }
        if (desc.Width < kSeamStripWidth || (desc.Width / 2) < (kSeamStripWidth / 2)) {
            return;
        }
        if (!IsSupportedSeamFormat(desc.Format)) {
            if (!logged_seam_unsupported) {
                logged_seam_unsupported = true;
                Log("MenuMirror auto-detect disabled: unsupported DXGI_FORMAT %u (use F8 manual toggle).",
                    static_cast<unsigned>(desc.Format));
            }
            return;
        }

        D3D12_RESOURCE_DESC strip_desc{};
        strip_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        strip_desc.Width = kSeamStripWidth;
        strip_desc.Height = desc.Height;
        strip_desc.DepthOrArraySize = 1;
        strip_desc.MipLevels = 1;
        strip_desc.Format = desc.Format;
        strip_desc.SampleDesc.Count = 1;
        strip_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total_bytes = 0;
        device->GetCopyableFootprints(&strip_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);
        if (total_bytes == 0) {
            return;
        }

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = total_bytes;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.Format = DXGI_FORMAT_UNKNOWN;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (UINT i = 0; i < kFrameSlots; ++i) {
            const HRESULT hr = device->CreateCommittedResource(
                &rb_heap,
                D3D12_HEAP_FLAG_NONE,
                &rb_desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&seam_readback[i]));
            if (FAILED(hr)) {
                Log("MenuMirror readback alloc[%u] failed (hr=0x%08X); falling back to F8 manual toggle.",
                    i,
                    static_cast<unsigned>(hr));
                for (auto& rb : seam_readback) {
                    rb.Reset();
                }
                return;
            }
        }

        seam_footprint = footprint;
        seam_readback_bytes = total_bytes;
        seam_readback_ready = true;
    }
```

Call `InitSeamReadbackLocked(desc);` right before `src_width = desc.Width;` in `InitD3D12Locked`.

6. Update `CopyAndPresent` signature to accept `(IDXGISwapChain3* game_swapchain, ID3D12CommandQueue* game_queue, OrigPresentFn orig_present, bool f8_down, ULONGLONG now_ms)`:

- After waiting for `slot_fence_values[slot]`, evaluate any completed readback and update `mirror_state`:
```cpp
            std::optional<StripClassification> sample;
            if (seam_readback_ready && seam_readback_valid[slot] &&
                mirror_state.config().auto_detect) {
                const D3D12_RANGE read_range{0, static_cast<SIZE_T>(seam_readback_bytes)};
                void* mapped = nullptr;
                const HRESULT map_hr = seam_readback[slot]->Map(0, &read_range, &mapped);
                if (SUCCEEDED(map_hr) && mapped) {
                    sample = ClassifyCenterStrip(mapped,
                                                 seam_footprint.Footprint.RowPitch,
                                                 src_height,
                                                 src_format);
                    const D3D12_RANGE empty_written{0, 0};
                    seam_readback[slot]->Unmap(0, &empty_written);
                } else if (!logged_seam_readback_err) {
                    logged_seam_readback_err = true;
                    Log("MenuMirror readback Map failed (hr=0x%08X).",
                        static_cast<unsigned>(map_hr));
                }
            }

            const MirrorUpdateResult mirror_res =
                mirror_state.Update(f8_down, sample, static_cast<uint64_t>(now_ms));
            if (mirror_res.transitioned) {
                Log("DUO layout: %s (%s)",
                    mirror_res.is_mirroring ? "MIRROR" : "SPLIT",
                    mirror_res.reason);
            }
```

- Replace the fixed `boxes[2]` in `CopyAndPresent` with conditional Split/Mirror boxes:
```cpp
            const UINT half_w = src_width / 2;
            const UINT center_x = (src_width - half_w) / 2;
            const D3D12_BOX split_boxes[2] = {
                {0, 0, 0, half_w, src_height, 1},
                {src_width - half_w, 0, 0, src_width, src_height, 1},
            };
            const D3D12_BOX mirror_box{center_x, 0, 0, center_x + half_w, src_height, 1};
            const D3D12_BOX boxes[2] = {
                mirror_res.is_mirroring ? mirror_box : split_boxes[0],
                mirror_res.is_mirroring ? mirror_box : split_boxes[1],
            };
```

- Right after the `for (int i = 0; i < 2; ++i)` `CopyTextureRegion` loop (while `src_buf` is still in `D3D12_RESOURCE_STATE_COPY_SOURCE`), queue the next center-strip copy if enabled:
```cpp
            bool queued_readback = false;
            if (seam_readback_ready && mirror_state.config().auto_detect) {
                D3D12_TEXTURE_COPY_LOCATION rb_dst{};
                rb_dst.pResource = seam_readback[slot].Get();
                rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                rb_dst.PlacedFootprint = seam_footprint;

                const UINT strip_left = half_w - (kSeamStripWidth / 2);
                const D3D12_BOX strip_box{
                    strip_left, 0, 0, strip_left + kSeamStripWidth, src_height, 1};
                cmd_list->CopyTextureRegion(&rb_dst, 0, 0, 0, &src_loc, &strip_box);
                queued_readback = true;
            }
```
And right after `game_queue->ExecuteCommandLists(1, lists);`:
```cpp
            seam_readback_valid[slot] = queued_readback;
```

7. In `DualOutput::Start` (`src/dual_output.cpp:665`):
```cpp
    impl_->mirror_state.SetConfig(LoadMirrorConfig(self_module));
```

8. In `DualOutput::PresentFrame` (`src/dual_output.cpp:709-731` and `796`):
- On `ToggleToDuo`: call `impl_->mirror_state.Reset();`
- Before calling `impl_->CopyAndPresent(...)`:
```cpp
    const bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    impl_->CopyAndPresent(game_swapchain, game_queue, orig_present, f8_down, now_ms);
```

- [ ] **Step 2: Build the full solution (`version.dll` + all tests) and run CTest**

Run:
```powershell
cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure
```
Expected: `version.dll` builds with 0 errors and all 3 CTest targets (`input_trigger_tests`, `duo_resize_guard_tests`, `split_seam_detector_tests`) pass.

- [ ] **Step 3: Commit**

```bash
git add src/dual_output.cpp
git commit -m "feat(dual-output): mirror shared menus in Duo mode with seam detection"
```

---

### Task 4: Document `[MenuMirror]` and `F8` in `config/gothamduo.ini` and `README.md`

**Files:**
- Modify: `config/gothamduo.ini`
- Modify: `README.md`

- [ ] **Step 1: Update `config/gothamduo.ini`**

Append `[MenuMirror]` section to `config/gothamduo.ini`:

```ini
[MenuMirror]
; Automatically mirror centered/shared menus in Duo mode (1 = enabled, 0 = manual F8 only).
AutoDetect=1
; Milliseconds a shared screen must be stable before switching to Mirror.
EnterMirrorMs=150
; Milliseconds a split-screen seam must be stable before returning to Split.
ExitMirrorMs=100
```

- [ ] **Step 2: Update `README.md`**

In `README.md`:
- Under `## Status`, update the menu checklist item and bullet notes to reflect Phase 1 (`F8` manual override + automatic centered-menu mirroring in Duo mode; subtitles/shared HUD elements during active split-screen still pending Phase 2).
- Under `## Installation`, add `F8` next to `F9` / `F10`:
  `- \`F8\`: Toggle mirrored menu mode manually in Duo mode (overrides auto-detection, press again to return to Auto).`
  And mention `[MenuMirror]` (`AutoDetect`, `EnterMirrorMs`, `ExitMirrorMs`).
- Under `## Known limitations (expected)`, update the first bullet to clarify that centered menus mirror in Duo mode while centered subtitles/HUD prompts over active split-screen remain pending Phase 2.

- [ ] **Step 3: Verify final build, tests, and git status**

Run:
```powershell
cmake --build build --config Release && ctest --test-dir build -C Release --output-on-failure && git status -sb
```
Expected: Clean `Release` build, `100% tests passed, 0 tests failed out of 3`.

- [ ] **Step 4: Commit**

```bash
git add config/gothamduo.ini README.md
git commit -m "docs: document [MenuMirror] config and F8 menu mirror toggle"
```

---

## In-Game Verification Checklist (Manual — Steam `LEGOBatmanLotDK-Win64-Shipping.exe`)

1. Copy `build/Release/version.dll` and `config/gothamduo.ini` to `LEGOBatmanLotDK\Binaries\Win64\`.
2. Launch game in Solo (`1920x1080`), then switch to Duo (`F9` or Controller 2 `Start`).
3. Verify split-screen gameplay stays `P1 | P2` across Monitor 1 and Monitor 2 (`gothamduo.log` shows no rapid `SPLIT <-> MIRROR` flapping).
4. Open **Start / Pause** menu -> verify both monitors automatically mirror the centered `1920x1080` menu cleanly (`DUO layout: MIRROR (auto)` in `gothamduo.log`).
5. Close menu -> verify split-screen returns cleanly (`DUO layout: SPLIT (auto)`).
6. Press `F8` during gameplay -> verify manual mirror toggle (`DUO layout: MIRROR (manual F8)`), press `F8` again to return to `Auto`.
