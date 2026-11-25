#include "split_seam_detector.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
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
using gotham::MirrorConfig;
using gotham::MirrorOverrideMode;
using gotham::MirrorStateMachine;
using gotham::MirrorUpdateResult;
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
        const uint32_t packed = PackPixel(c, fmt);
        std::memcpy(bytes.data() + static_cast<size_t>(y) * row_pitch +
                        static_cast<size_t>(x) * sizeof(uint32_t),
                    &packed, sizeof(packed));
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

void TestBoundaryAndAmbiguousCases() {
    constexpr DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    StripBuffer valid_buf(64, 64);
    for (uint32_t y = 0; y < valid_buf.height; ++y) {
        FillRowSplit(valid_buf, y, Rgb8{20, 40, 200}, Rgb8{210, 170, 30}, fmt);
    }

    // 1. Guard inputs.
    CHECK(ClassifyCenterStrip(nullptr, 64, 64, fmt) == StripClassification::Ambiguous);
    CHECK(ClassifyCenterStrip(valid_buf.bytes.data(), 64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT) ==
          StripClassification::Ambiguous);
    CHECK(ClassifyCenterStrip(valid_buf.bytes.data(),
                              static_cast<uint32_t>(kSeamStripWidth * sizeof(uint32_t)) - 4, 64,
                              fmt) == StripClassification::Ambiguous);
    CHECK(ClassifyCenterStrip(valid_buf.bytes.data(), 64, 15, fmt) ==
          StripClassification::Ambiguous);

    // 2. Small valid height == 16 and tight row_pitch == 64.
    StripBuffer small_black(16, 64);
    CHECK(ClassifyCenterStrip(small_black.bytes.data(), small_black.row_pitch, small_black.height,
                              fmt) == StripClassification::Ambiguous);

    StripBuffer small_split(16, 64);
    for (uint32_t y = 0; y < small_split.height; ++y) {
        FillRowSplit(small_split, y, Rgb8{20, 40, 200}, Rgb8{210, 170, 30}, fmt);
    }
    CHECK(ClassifyCenterStrip(small_split.bytes.data(), small_split.row_pitch, small_split.height,
                              fmt) == StripClassification::Split);

    // 3. Intermediate Ambiguous band (0.20 < seam_ratio < 0.35; ~28% seam rows).
    StripBuffer mid_buf;
    for (uint32_t y = 0; y < mid_buf.height; ++y) {
        if (y < 360) {
            FillRowSplit(mid_buf, y, Rgb8{15, 50, 210}, Rgb8{220, 160, 20}, fmt);
        } else {
            FillRowSharedGradient(mid_buf, y, Rgb8{35, 45, 70}, fmt);
        }
    }
    CHECK(ClassifyCenterStrip(mid_buf.bytes.data(), mid_buf.row_pitch, mid_buf.height, fmt) ==
          StripClassification::Ambiguous);

    // 4. *_UNORM_SRGB formats in ClassifyCenterStrip.
    const std::array<DXGI_FORMAT, 2> srgb_formats = {
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    };
    for (DXGI_FORMAT srgb_fmt : srgb_formats) {
        StripBuffer srgb_split(64, 64);
        StripBuffer srgb_shared(64, 64);
        for (uint32_t y = 0; y < 64; ++y) {
            FillRowSplit(srgb_split, y, Rgb8{20, 60, 210}, Rgb8{220, 150, 25}, srgb_fmt);
            FillRowSharedGradient(srgb_shared, y, Rgb8{40, 80, 120}, srgb_fmt);
        }
        CHECK(ClassifyCenterStrip(srgb_split.bytes.data(), srgb_split.row_pitch, srgb_split.height,
                                  srgb_fmt) == StripClassification::Split);
        CHECK(ClassifyCenterStrip(srgb_shared.bytes.data(), srgb_shared.row_pitch,
                                  srgb_shared.height, srgb_fmt) == StripClassification::Shared);
    }

    // 5. R10G10B10A2_UNORM with non-zero low 2 bits (0x3) per 10-bit channel.
    StripBuffer r10_split(64, 64);
    for (uint32_t y = 0; y < r10_split.height; ++y) {
        for (uint32_t x = 0; x < kSeamStripWidth; ++x) {
            const Rgb8 c = x < 8 ? Rgb8{20, 60, 210} : Rgb8{220, 150, 25};
            const uint32_t packed = PackPixel(c, DXGI_FORMAT_R10G10B10A2_UNORM) | 0x00300C03u;
            std::memcpy(r10_split.bytes.data() + static_cast<size_t>(y) * r10_split.row_pitch +
                            static_cast<size_t>(x) * sizeof(uint32_t),
                        &packed, sizeof(packed));
        }
    }
    CHECK(ClassifyCenterStrip(r10_split.bytes.data(), r10_split.row_pitch, r10_split.height,
                              DXGI_FORMAT_R10G10B10A2_UNORM) == StripClassification::Split);
}

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

}  // namespace

int main() {
    TestSupportedFormats();
    TestClassifySplitAndSharedAcrossFormats();
    TestCenteredMenuOverSplitAndUniformBlack();
    TestBoundaryAndAmbiguousCases();
    TestMirrorHysteresisAndAmbiguousHold();
    TestMirrorF8AutoAndManualOverrides();
    std::printf("All split_seam_detector_tests passed.\n");
    return 0;
}
