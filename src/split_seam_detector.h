#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace gotham {

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
    uint32_t enter_mirror_ms = 200;
    uint32_t exit_mirror_ms = 100;
};

struct MirrorUpdateResult {
    bool is_mirroring = false;
    bool transitioned = false;
    const char* reason = "";
};

struct ViewportDrawSummary {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    uint32_t draws = 0;
    bool has_dsv = false;
};

struct FrameRenderStats {
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    bool has_menu_blur_pass = false;
    std::vector<ViewportDrawSummary> viewports;
};

/// Classifies a frame as Split, Shared, or Ambiguous using structural D3D12 viewport stats.
StripClassification ClassifyFrameStats(const FrameRenderStats& stats);

/// Pure state machine combining classification hysteresis with F1 manual overrides.
class MirrorStateMachine {
public:
    void SetConfig(MirrorConfig config);
    const MirrorConfig& config() const { return config_; }

    void Reset(bool f1_down = false, bool initial_mirror = false);

    MirrorUpdateResult Update(bool f1_down,
                              std::optional<StripClassification> sample,
                              uint64_t now_ms,
                              bool has_menu_blur = false);

    bool is_mirroring() const { return is_mirroring_; }
    MirrorOverrideMode override_mode() const { return override_mode_; }

private:
    MirrorConfig config_{};
    MirrorOverrideMode override_mode_ = MirrorOverrideMode::Auto;
    bool is_mirroring_ = false;
    bool entered_via_menu_blur_ = false;
    bool prev_f1_down_ = false;
    std::optional<StripClassification> candidate_kind_;
    std::optional<uint64_t> candidate_start_ms_;
};

/// Detects split-screen gameplay while in Solo mode so Duo engages when P2 enters.
class SplitJoinDetector {
public:
    void Reset(bool suppress_until_shared = false);

    bool Update(const MirrorConfig& config,
                std::optional<StripClassification> sample,
                uint64_t now_ms);

    bool is_suppressed() const { return suppressed_; }

private:
    bool suppressed_ = false;
    std::optional<uint64_t> split_start_ms_;
    std::optional<uint64_t> shared_start_ms_;
};

enum CenteredTargetMask : uint8_t {
    kTargetNone = 0,
    kTargetLeft = 1 << 0,
    kTargetRight = 1 << 1,
    kTargetBoth = kTargetLeft | kTargetRight,
};

struct SlateRect {
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    bool has_center_primitive = true;
    uint8_t target_mask = kTargetBoth;
};

enum class CenteredUiMatch {
    None,
    CrossesCenter,
    InsideCenteredBox,
};

/// Returns whether a full-canvas Slate draw crosses the center split or lies inside one that did.
CenteredUiMatch ClassifyCenteredSlateRect(const SlateRect& rect,
                                          uint32_t frame_w,
                                          uint32_t frame_h,
                                          std::vector<SlateRect>& centered_boxes,
                                          uint8_t* out_target_mask = nullptr);

struct ScissorBox {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

struct ReplicatedScissors {
    bool draw_left = false;
    ScissorBox left{};
    bool draw_right = false;
    ScissorBox right{};
};

/// Computes shifted and half-clamped scissor rects for Monitor 1 (-W/4) and Monitor 2 (+W/4).
ReplicatedScissors ComputeReplicatedScissors(const ScissorBox& cur_sc,
                                             uint32_t frame_w,
                                             uint32_t frame_h,
                                             uint8_t target_mask);

}  // namespace gotham
