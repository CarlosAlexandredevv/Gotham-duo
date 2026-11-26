#include "output/monitor_layout.h"

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

using gotham::ComputeDuoLayout;
using gotham::ComputeMirrorRect;
using gotham::ComputeSoloRect;
using gotham::DuoLayout;
using gotham::FitRect;
using gotham::LayoutRect;

void TestIdentical1080p() {
    const LayoutRect m1{0, 0, 1920, 1080};
    const LayoutRect m2{1920, 0, 3840, 1080};
    const DuoLayout l = ComputeDuoLayout(m1, m2, 0, 0);
    CHECK(l.half_w == 1920 && l.half_h == 1080);
    CHECK((l.span == LayoutRect{0, 0, 3840, 1080}));
    CHECK(l.out[0] == m1);
    CHECK(l.out[1] == m2);
}

void TestMixed1440pAnd1080pNeverCrops() {
    const LayoutRect m1{0, 0, 2560, 1440};
    const LayoutRect m2{2560, 0, 4480, 1080};
    const DuoLayout l = ComputeDuoLayout(m1, m2, 0, 0);
    // Both players render at the smaller monitor (1920x1080); no half is wider than its source.
    CHECK(l.half_w == 1920 && l.half_h == 1080);
    CHECK((l.span == LayoutRect{0, 0, 3840, 1080}));
    // 16:9 content fills both monitors exactly (1440p is upscaled).
    CHECK(l.out[0] == m1);
    CHECK(l.out[1] == m2);
}

void TestUltrawideIsPillarboxedAndCentered() {
    const LayoutRect m1{0, 0, 3440, 1440};
    const LayoutRect m2{3440, 0, 5360, 1080};
    const DuoLayout l = ComputeDuoLayout(m1, m2, 0, 0);
    CHECK(l.half_w == 1920 && l.half_h == 1080);
    CHECK((l.out[0] == LayoutRect{440, 0, 3000, 1440}));  // 2560x1440 centered
    CHECK(l.out[1] == m2);
}

void TestNegativeCoordinatesAndVerticalStack() {
    // Monitor 2 to the left of monitor 1 (negative x).
    const LayoutRect left_mon{-1920, 0, 0, 1080};
    const LayoutRect right_mon{0, 0, 1920, 1080};
    DuoLayout l = ComputeDuoLayout(left_mon, right_mon, 0, 0);
    CHECK(l.span.left == -1920 && l.span.top == 0);
    CHECK(l.out[0] == left_mon);

    // Stacked vertically with the secondary above (negative y).
    const LayoutRect top_mon{0, -1080, 1920, 0};
    const LayoutRect bottom_mon{0, 0, 1920, 1080};
    l = ComputeDuoLayout(top_mon, bottom_mon, 0, 0);
    CHECK(l.span.left == 0 && l.span.top == -1080);
    CHECK(l.out[0] == top_mon);
    CHECK(l.out[1] == bottom_mon);
}

void TestPortraitMonitorLetterboxed() {
    const LayoutRect m1{0, 0, 1920, 1080};
    const LayoutRect portrait{1920, 0, 3000, 1920};  // 1080x1920
    const DuoLayout l = ComputeDuoLayout(m1, portrait, 0, 0);
    CHECK(l.half_w == 1080 && l.half_h == 1080);
    CHECK((l.out[1] == LayoutRect{1920, 420, 3000, 1500}));  // 1080x1080 centered vertically
    CHECK((l.out[0] == LayoutRect{420, 0, 1500, 1080}));
}

void TestOverridesAndMirrorSolo() {
    const LayoutRect m1{0, 0, 1920, 1080};
    const LayoutRect m2{1920, 0, 3840, 1080};
    const DuoLayout l = ComputeDuoLayout(m1, m2, 5120, 1440);
    CHECK(l.half_w == 2560 && l.half_h == 1440);
    CHECK(l.span.width() == 5120 && l.span.height() == 1440);
    // Oversized per-player render is fit down onto each 1080p monitor (never cropped).
    CHECK(l.out[0] == m1);

    const DuoLayout base = ComputeDuoLayout(m1, m2, 0, 0);
    CHECK((ComputeMirrorRect(m1, base, 0, 0) == LayoutRect{0, 0, 1920, 1080}));
    CHECK((ComputeMirrorRect(m1, base, 1280, 720) == LayoutRect{0, 0, 1280, 720}));
    CHECK((ComputeSoloRect(m2, 0, 0) == m2));
    CHECK((ComputeSoloRect(m2, 2560, 0) == LayoutRect{1920, 0, 4480, 1080}));
}

void TestFitRectDegenerate() {
    const LayoutRect m{0, 0, 1920, 1080};
    CHECK(FitRect(0, 1080, m) == m);
    CHECK(FitRect(1920, 0, m) == m);
    const LayoutRect empty{0, 0, 0, 0};
    CHECK(FitRect(1920, 1080, empty) == empty);
}

}  // namespace

int main() {
    TestIdentical1080p();
    TestMixed1440pAnd1080pNeverCrops();
    TestUltrawideIsPillarboxedAndCentered();
    TestNegativeCoordinatesAndVerticalStack();
    TestPortraitMonitorLetterboxed();
    TestOverridesAndMirrorSolo();
    TestFitRectDegenerate();
    std::printf("All monitor_layout_tests passed.\n");
    return 0;
}
