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
