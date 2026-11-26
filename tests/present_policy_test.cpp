#include "output/present_policy.h"

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

using namespace gotham;

void TestPassThroughForSpecialPresents() {
    CHECK(CanSuppressGamePresent(0, 0));
    CHECK(CanSuppressGamePresent(0x200 /*ALLOW_TEARING*/, 0x800 /*ALLOW_TEARING flag*/));
    CHECK(!CanSuppressGamePresent(kPresentTest, 0));
    CHECK(!CanSuppressGamePresent(kPresentRestart, 0));
    CHECK(!CanSuppressGamePresent(kPresentDoNotSequence, 0));
    CHECK(!CanSuppressGamePresent(kPresentDoNotWait, 0));
    // Waitable-object swapchains must keep the game's real Present (it signals the object).
    CHECK(!CanSuppressGamePresent(0, kSwapChainFrameLatencyWaitable));
}

void TestSuppressOnlyWhenDuoOutputsVisible() {
    SuppressInput in{};
    in.enabled = true;
    in.is_duo = true;
    in.outputs_visible = true;
    CHECK(ShouldSuppressGamePresent(in));

    // Immediate fallback to the normal present in every other state.
    SuppressInput solo = in;
    solo.is_duo = false;
    CHECK(!ShouldSuppressGamePresent(solo));

    SuppressInput hidden = in;  // Alt+Tab / minimized / not yet shown
    hidden.outputs_visible = false;
    CHECK(!ShouldSuppressGamePresent(hidden));

    SuppressInput disabled = in;  // [Performance] SuppressGamePresent=0
    disabled.enabled = false;
    CHECK(!ShouldSuppressGamePresent(disabled));

    SuppressInput test_present = in;
    test_present.present_flags = kPresentTest;
    CHECK(!ShouldSuppressGamePresent(test_present));
}

void TestFramePacingTransfer() {
    // Not suppressing: game present paces; outputs never block.
    OutputSync s = ComputeOutputSync(false, 1);
    CHECK(s.sync_interval[0] == 0 && s.sync_interval[1] == 0);

    // Suppressing: first output inherits VSync, second never blocks.
    s = ComputeOutputSync(true, 1);
    CHECK(s.sync_interval[0] == 1 && s.sync_interval[1] == 0);
    s = ComputeOutputSync(true, 2);
    CHECK(s.sync_interval[0] == 2 && s.sync_interval[1] == 0);

    // Uncapped games stay uncapped; invalid intervals are clamped to DXGI's 0..4.
    s = ComputeOutputSync(true, 0);
    CHECK(s.sync_interval[0] == 0 && s.sync_interval[1] == 0);
    s = ComputeOutputSync(true, 99);
    CHECK(s.sync_interval[0] == 4);
}

}  // namespace

int main() {
    TestPassThroughForSpecialPresents();
    TestSuppressOnlyWhenDuoOutputsVisible();
    TestFramePacingTransfer();
    std::printf("All present_policy_tests passed.\n");
    return 0;
}
