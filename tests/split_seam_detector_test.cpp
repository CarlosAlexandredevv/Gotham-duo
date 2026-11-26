#include "output/split_seam_detector.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

namespace {

using gotham::CenteredUiMatch;
using gotham::ClassifyCenteredSlateRect;
using gotham::ClassifyFrameStats;
using gotham::FrameRenderStats;
using gotham::MirrorConfig;
using gotham::MirrorOverrideMode;
using gotham::MirrorStateMachine;
using gotham::MirrorUpdateResult;
using gotham::SlateRect;
using gotham::SplitJoinDetector;
using gotham::StripClassification;

void TestClassifyFrameStats() {
    // 1. Empty / zero-draw frames are Ambiguous.
    FrameRenderStats empty{};
    empty.frame_width = 3840;
    empty.frame_height = 1080;
    CHECK(ClassifyFrameStats(empty) == StripClassification::Ambiguous);

    FrameRenderStats zero_draws = empty;
    zero_draws.viewports.push_back({0, 0, 3840, 1080, 0, false});
    CHECK(ClassifyFrameStats(zero_draws) == StripClassification::Ambiguous);

    // 2. Active split-screen at 3840x1080 (P1 left + P2 right, no menu blur) -> Split.
    FrameRenderStats split_3840 = empty;
    split_3840.viewports.push_back({0, 0, 1920, 1080, 85, true});
    split_3840.viewports.push_back({1920, 0, 1920, 1080, 90, true});
    split_3840.viewports.push_back({0, 0, 3840, 1080, 20, false});
    CHECK(ClassifyFrameStats(split_3840) == StripClassification::Split);

    // 3. Active split-screen at 1920x1080 (during Mirror mode when gameplay starts) -> Split.
    FrameRenderStats split_1920{};
    split_1920.frame_width = 1920;
    split_1920.frame_height = 1080;
    split_1920.viewports.push_back({0, 0, 960, 1080, 75, true});
    split_1920.viewports.push_back({960, 0, 960, 1080, 80, true});
    CHECK(ClassifyFrameStats(split_1920) == StripClassification::Split);

    // 4. Pause/Start menu over split-screen (has_menu_blur_pass == true) -> Shared.
    FrameRenderStats pause_menu = split_3840;
    pause_menu.has_menu_blur_pass = true;
    CHECK(ClassifyFrameStats(pause_menu) == StripClassification::Shared);

    // 5. Main Menu / Loading / Shared camera (full-width 3840x1080 only) -> Shared.
    FrameRenderStats main_menu = empty;
    main_menu.viewports.push_back({0, 0, 3840, 1080, 50, true});
    CHECK(ClassifyFrameStats(main_menu) == StripClassification::Shared);

    // 6. 21:9 cinema cutscene (630,0 2581x1080 + Slate 3840x1080) -> Shared.
    FrameRenderStats cutscene = empty;
    cutscene.viewports.push_back({630, 0, 2581, 1080, 120, true});
    cutscene.viewports.push_back({0, 0, 3840, 1080, 10, false});
    CHECK(ClassifyFrameStats(cutscene) == StripClassification::Shared);

    // 7. Intro video (0,0 1920x1080 + 960,0 1920x1080, no right half at 1920) -> Shared.
    FrameRenderStats intro = empty;
    intro.viewports.push_back({0, 0, 1920, 1080, 8, false});
    intro.viewports.push_back({960, 0, 1920, 1080, 2, false});
    CHECK(ClassifyFrameStats(intro) == StripClassification::Shared);

    // 8. Single-player shop/interior loading while co-op partner stays in 3D world -> Split.
    FrameRenderStats p1_loading = empty;
    p1_loading.viewports.push_back({1920, 0, 1920, 1080, 180, true});
    p1_loading.viewports.push_back({0, 0, 3840, 1080, 12, false});
    CHECK(ClassifyFrameStats(p1_loading) == StripClassification::Split);

    FrameRenderStats p2_loading = empty;
    p2_loading.viewports.push_back({0, 0, 1920, 1080, 160, true});
    p2_loading.viewports.push_back({0, 0, 3840, 1080, 12, false});
    CHECK(ClassifyFrameStats(p2_loading) == StripClassification::Split);
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

    // Ambiguous frames keep Mirror active and reset exit timer.
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

    // Pause/Start menu (has_menu_blur=true) enters Mirror immediately (0 ms) and exits fast (32 ms).
    sm.Reset();
    MirrorUpdateResult pause_enter = sm.Update(false, StripClassification::Shared, 3000, true);
    CHECK(pause_enter.is_mirroring);
    CHECK(pause_enter.transitioned);
    // Single post-resize frame without blur (< 32 ms) does not bounce out of Mirror.
    CHECK(sm.Update(false, StripClassification::Split, 3016, false).is_mirroring);
    // Unpausing exits Mirror cleanly at 32 ms instead of waiting full exit_mirror_ms (100 ms).
    MirrorUpdateResult pause_exit = sm.Update(false, StripClassification::Split, 3048, false);
    CHECK(!pause_exit.is_mirroring);
    CHECK(pause_exit.transitioned);
}

void TestMirrorF1AutoAndManualOverrides() {
    MirrorStateMachine sm;
    MirrorConfig cfg{};
    cfg.auto_detect = true;
    cfg.enter_mirror_ms = 150;
    cfg.exit_mirror_ms = 100;
    sm.SetConfig(cfg);

    // In Auto + Split, F1 rising edge locks ManualMirror; holding F1 does not retrigger.
    MirrorUpdateResult r = sm.Update(true, std::nullopt, 100);
    CHECK(r.is_mirroring && r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualMirror);

    r = sm.Update(true, StripClassification::Split, 300);
    CHECK(r.is_mirroring && !r.transitioned);

    // Release F1, then press F1 again -> returns to Auto (still mirroring until Split settles).
    CHECK(sm.Update(false, std::nullopt, 310).is_mirroring);
    r = sm.Update(true, std::nullopt, 320);
    CHECK(sm.override_mode() == MirrorOverrideMode::Auto);
    CHECK(r.is_mirroring && !r.transitioned);

    // After 100 ms of Split in Auto, returns to Split.
    CHECK(sm.Update(false, StripClassification::Split, 330).is_mirroring);
    CHECK(!sm.Update(false, StripClassification::Split, 430).is_mirroring);

    // AutoDetect=0 toggles directly ManualSplit <-> ManualMirror on F1 rising edge.
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

    // Reset(true) prevents a held F1 across mode toggle from firing a spurious rising edge.
    sm.Reset(true);
    r = sm.Update(true, std::nullopt, 1200);
    CHECK(!r.is_mirroring && !r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::Auto);
    CHECK(!sm.Update(false, std::nullopt, 1210).is_mirroring);
    r = sm.Update(true, std::nullopt, 1220);
    CHECK(r.is_mirroring && r.transitioned);
    CHECK(sm.override_mode() == MirrorOverrideMode::ManualMirror);
}

void TestSplitJoinDetector() {
    MirrorConfig cfg{};
    cfg.auto_detect = true;
    cfg.enter_mirror_ms = 200;
    cfg.exit_mirror_ms = 100;

    SplitJoinDetector jd;
    CHECK(!jd.is_suppressed());

    // Single-player Shared screens do not trigger Duo.
    CHECK(!jd.Update(cfg, StripClassification::Shared, 100));
    CHECK(!jd.Update(cfg, StripClassification::Shared, 500));

    // Entering split-screen triggers Duo after exit_mirror_ms (100 ms).
    CHECK(!jd.Update(cfg, StripClassification::Split, 1000));
    CHECK(!jd.Update(cfg, StripClassification::Split, 1099));
    CHECK(jd.Update(cfg, StripClassification::Split, 1100));

    // Manual exit to Solo suppresses re-trigger while split-screen remains active.
    jd.Reset(true);
    CHECK(jd.is_suppressed());
    CHECK(!jd.Update(cfg, StripClassification::Split, 2000));
    CHECK(!jd.Update(cfg, StripClassification::Split, 2500));
    CHECK(jd.is_suppressed());

    // Returning to a Shared screen for enter_mirror_ms re-arms auto-Duo.
    CHECK(!jd.Update(cfg, StripClassification::Shared, 3000));
    CHECK(!jd.Update(cfg, StripClassification::Shared, 3199));
    CHECK(!jd.Update(cfg, StripClassification::Shared, 3200));
    CHECK(!jd.is_suppressed());

    // Subsequent split-screen triggers Duo again.
    CHECK(!jd.Update(cfg, StripClassification::Split, 4000));
    CHECK(jd.Update(cfg, StripClassification::Split, 4100));

    // Disabled auto_detect never auto-triggers Duo.
    cfg.auto_detect = false;
    jd.Reset(false);
    CHECK(!jd.Update(cfg, StripClassification::Split, 5000));
    CHECK(!jd.Update(cfg, StripClassification::Split, 5500));
}

void TestClassifyCenteredSlateRect() {
    std::vector<SlateRect> boxes;

    // P1 left HUD and P2 right HUD never replicate.
    CHECK(ClassifyCenteredSlateRect({1512.0f, 1728.0f, 848.0f, 1080.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(ClassifyCenteredSlateRect({1952.0f, 2168.0f, 848.0f, 1080.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(boxes.empty());

    // Center split divider bar (tall vertical strip crossing 1920) is excluded.
    CHECK(ClassifyCenteredSlateRect({1917.0f, 1923.0f, -6.0f, 1086.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    // Full-screen fade overlay is excluded.
    CHECK(ClassifyCenteredSlateRect({0.0f, 3840.0f, 0.0f, 1080.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    // Off-center lateral HUD boxes that touch or cross 1920 are excluded.
    CHECK(ClassifyCenteredSlateRect({87.0f, 2040.6f, 841.0f, 1003.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(ClassifyCenteredSlateRect({1906.0f, 3473.0f, 960.0f, 1025.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(ClassifyCenteredSlateRect({0.0f, 1932.4f, 0.0f, 117.3f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(ClassifyCenteredSlateRect({1393.6f, 2028.6f, 0.0f, 162.5f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(ClassifyCenteredSlateRect({1083.7f, 2296.4f, 0.0f, 1.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    // Out-of-bounds / non-screen coordinates are excluded.
    CHECK(ClassifyCenteredSlateRect({0.0f, 2416.9f, -713.3f, 32.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);
    CHECK(boxes.empty());

    // Wide 16:9 centered subtitle box (~1820 px wide, like Bat-Mite's shop dialogue) replicates.
    uint8_t mask = 0;
    CHECK(ClassifyCenteredSlateRect(
              {1010.0f, 2830.0f, 859.0f, 980.0f}, 3840, 1080, boxes, &mask) ==
          CenteredUiMatch::CrossesCenter);
    CHECK(mask == gotham::kTargetBoth);
    // Left-aligned speaker prefix ("Bat-Mirim:") inside that wide subtitle box also replicates.
    CHECK(ClassifyCenteredSlateRect(
              {1035.0f, 1240.0f, 875.0f, 912.0f}, 3840, 1080, boxes, &mask) ==
          CenteredUiMatch::InsideCenteredBox);
    CHECK(mask == gotham::kTargetBoth);
    boxes.clear();

    // Batched P1 + P2 symmetric HUD quads with an empty gap across x=1920 (no center primitive)
    // must NOT replicate or poison centered_boxes.
    CHECK(ClassifyCenteredSlateRect(
              {1720.3f, 2156.0f, 939.5f, 980.0f, false, gotham::kTargetBoth},
              3840,
              1080,
              boxes,
              &mask) == CenteredUiMatch::None);
    CHECK(boxes.empty());

    // Centered banner drawn under P2-only scissor (e.g. when P1 HUD is hidden inside shop)
    // shifts onto P2 only, and its child elements inherit kTargetRight.
    CHECK(ClassifyCenteredSlateRect(
              {1802.0f, 2039.0f, 75.0f, 127.0f, true, gotham::kTargetRight},
              3840,
              1080,
              boxes,
              &mask) == CenteredUiMatch::CrossesCenter);
    CHECK(mask == gotham::kTargetRight);
    CHECK(ClassifyCenteredSlateRect(
              {1841.5f, 1877.5f, 83.0f, 119.0f, false, gotham::kTargetLeft},
              3840,
              1080,
              boxes,
              &mask) == CenteredUiMatch::InsideCenteredBox);
    CHECK(mask == gotham::kTargetRight);
    boxes.clear();

    // Centered subtitle background box crosses 1920 and is recorded.
    CHECK(ClassifyCenteredSlateRect({1518.0f, 2322.0f, 846.0f, 893.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::CrossesCenter);
    CHECK(boxes.size() == 1);

    // Narrow centered notification box and padded inner text/counter replicate cleanly.
    CHECK(ClassifyCenteredSlateRect({1834.0f, 2007.0f, 75.0f, 127.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::CrossesCenter);
    CHECK(ClassifyCenteredSlateRect({1810.0f, 1915.0f, 82.0f, 118.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::InsideCenteredBox);

    // Speaker name strictly inside left portion of subtitle box replicates as InsideCenteredBox.
    CHECK(ClassifyCenteredSlateRect({1560.0f, 1690.0f, 856.0f, 886.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::InsideCenteredBox);

    // Subtitle text line crossing 1920 replicates directly.
    CHECK(ClassifyCenteredSlateRect({1717.0f, 2150.0f, 853.6f, 888.8f}, 3840, 1080, boxes) ==
          CenteredUiMatch::CrossesCenter);

    // Unrelated P1 HUD drawn afterwards does not match centered subtitle box.
    CHECK(ClassifyCenteredSlateRect({100.0f, 500.0f, 900.0f, 1000.0f}, 3840, 1080, boxes) ==
          CenteredUiMatch::None);

    // Scissor shifting covers full canvas, 16:9 safe-zone, tight widget clips, and P1/P2 half clips.
    const auto sc_full =
        gotham::ComputeReplicatedScissors({0, 0, 3840, 1080}, 3840, 1080, gotham::kTargetBoth);
    CHECK(sc_full.draw_left && sc_full.left.left == 0 && sc_full.left.right == 1920);
    CHECK(sc_full.draw_right && sc_full.right.left == 1920 && sc_full.right.right == 3840);

    const auto sc_safe =
        gotham::ComputeReplicatedScissors({960, 0, 2880, 1080}, 3840, 1080, gotham::kTargetBoth);
    CHECK(sc_safe.draw_left && sc_safe.left.left == 0 && sc_safe.left.right == 1920);
    CHECK(sc_safe.draw_right && sc_safe.right.left == 1920 && sc_safe.right.right == 3840);

    const auto sc_icon =
        gotham::ComputeReplicatedScissors({1840, 83, 1878, 119}, 3840, 1080, gotham::kTargetBoth);
    CHECK(sc_icon.draw_left && sc_icon.left.left == 880 && sc_icon.left.right == 918);
    CHECK(sc_icon.draw_right && sc_icon.right.left == 2800 && sc_icon.right.right == 2838);

    const auto sc_p2 =
        gotham::ComputeReplicatedScissors({1920, 0, 3840, 1080}, 3840, 1080, gotham::kTargetRight);
    CHECK(!sc_p2.draw_left);
    CHECK(sc_p2.draw_right && sc_p2.right.left == 1920 && sc_p2.right.right == 3840);
}

}  // namespace

int main() {
    TestClassifyFrameStats();
    TestMirrorHysteresisAndAmbiguousHold();
    TestMirrorF1AutoAndManualOverrides();
    TestSplitJoinDetector();
    TestClassifyCenteredSlateRect();
    std::printf("All split_seam_detector_tests passed.\n");
    return 0;
}
