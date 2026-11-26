#pragma once

#include <algorithm>
#include <cstdint>

namespace gotham {

// DXGI_PRESENT_* / DXGI_SWAP_CHAIN_FLAG_* values (kept numeric so this header stays Windows-free).
inline constexpr uint32_t kPresentTest = 0x1;
inline constexpr uint32_t kPresentDoNotSequence = 0x2;
inline constexpr uint32_t kPresentRestart = 0x4;
inline constexpr uint32_t kPresentDoNotWait = 0x8;
inline constexpr uint32_t kSwapChainFrameLatencyWaitable = 0x40;

/// True when the game's Present is an ordinary frame present that may be replaced by our outputs.
/// Test/restart/sequence/no-wait presents and waitable-object swapchains (the game would block on
/// a latency object that only a real Present signals) always pass through untouched.
inline bool CanSuppressGamePresent(uint32_t present_flags, uint32_t swapchain_flags) {
    const uint32_t special =
        kPresentTest | kPresentDoNotSequence | kPresentRestart | kPresentDoNotWait;
    return (present_flags & special) == 0 &&
           (swapchain_flags & kSwapChainFrameLatencyWaitable) == 0;
}

struct SuppressInput {
    bool enabled = true;
    bool is_duo = false;
    bool outputs_visible = false;
    uint32_t present_flags = 0;
    uint32_t swapchain_flags = 0;
};

/// Decides, before presenting the Duo outputs, whether they take over the game's present.
/// Falls back to the game's own Present in Solo, while output windows are hidden (Alt+Tab,
/// minimized, not yet shown), and for any non-ordinary present.
inline bool ShouldSuppressGamePresent(const SuppressInput& in) {
    return in.enabled && in.is_duo && in.outputs_visible &&
           CanSuppressGamePresent(in.present_flags, in.swapchain_flags);
}

struct OutputSync {
    uint32_t sync_interval[2] = {0, 0};
};

/// Without suppression the game's own Present paces the frame, so both outputs present
/// immediately. When suppressed, the first output inherits the game's sync interval (VSync /
/// frame pacing) and the second never blocks, so only one vblank wait is paid per frame.
inline OutputSync ComputeOutputSync(bool suppressing, uint32_t game_sync_interval) {
    OutputSync s{};
    if (suppressing) {
        s.sync_interval[0] = (std::min)(game_sync_interval, 4u);
    }
    return s;
}

}  // namespace gotham
