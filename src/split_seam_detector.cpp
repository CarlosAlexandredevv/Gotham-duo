#include "split_seam_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gotham {

namespace {

bool NearInt(int a, int b) {
    return std::abs(a - b) <= 2;
}

}  // namespace

StripClassification ClassifyFrameStats(const FrameRenderStats& stats) {
    if (stats.frame_width < 32 || stats.frame_height < 32 || stats.viewports.empty()) {
        return StripClassification::Ambiguous;
    }

    const int full_w = static_cast<int>(stats.frame_width);
    const int full_h = static_cast<int>(stats.frame_height);
    const int half_w = full_w / 2;

    uint32_t total_draws = 0;
    uint32_t left_split_draws = 0;
    uint32_t right_split_draws = 0;
    uint32_t left_split_dsv_draws = 0;
    uint32_t right_split_dsv_draws = 0;
    uint32_t full_dsv_draws = 0;

    for (const auto& vp : stats.viewports) {
        total_draws += vp.draws;
        if (NearInt(vp.x, 0) && NearInt(vp.y, 0) && NearInt(vp.w, full_w) &&
            NearInt(vp.h, full_h) && vp.has_dsv) {
            full_dsv_draws += vp.draws;
        }
        if (NearInt(vp.y, 0) && NearInt(vp.w, half_w) && NearInt(vp.h, full_h)) {
            if (NearInt(vp.x, 0)) {
                left_split_draws += vp.draws;
                if (vp.has_dsv) {
                    left_split_dsv_draws += vp.draws;
                }
            } else if (NearInt(vp.x, half_w)) {
                right_split_draws += vp.draws;
                if (vp.has_dsv) {
                    right_split_dsv_draws += vp.draws;
                }
            }
        }
    }

    if (total_draws == 0) {
        return StripClassification::Ambiguous;
    }

    if (stats.has_menu_blur_pass) {
        return StripClassification::Shared;
    }

    if (left_split_draws >= 10 && right_split_draws >= 10) {
        return StripClassification::Split;
    }

    if (full_dsv_draws < 10 && (left_split_dsv_draws >= 10 || right_split_dsv_draws >= 10)) {
        return StripClassification::Split;
    }

    return StripClassification::Shared;
}

void MirrorStateMachine::SetConfig(MirrorConfig config) {
    config_ = config;
    Reset();
}

void MirrorStateMachine::Reset(bool f1_down, bool initial_mirror) {
    override_mode_ = config_.auto_detect ? MirrorOverrideMode::Auto
                                         : MirrorOverrideMode::ManualSplit;
    is_mirroring_ = config_.auto_detect && initial_mirror;
    entered_via_menu_blur_ = false;
    prev_f1_down_ = f1_down;
    candidate_kind_.reset();
    candidate_start_ms_.reset();
}

MirrorUpdateResult MirrorStateMachine::Update(bool f1_down,
                                              std::optional<StripClassification> sample,
                                              uint64_t now_ms,
                                              bool has_menu_blur) {
    const bool f1_rising = f1_down && !prev_f1_down_;
    prev_f1_down_ = f1_down;

    if (f1_rising) {
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        entered_via_menu_blur_ = false;
        const bool was_mirroring = is_mirroring_;

        if (!config_.auto_detect || override_mode_ == MirrorOverrideMode::Auto) {
            is_mirroring_ = !is_mirroring_;
            override_mode_ = is_mirroring_ ? MirrorOverrideMode::ManualMirror
                                           : MirrorOverrideMode::ManualSplit;
            return MirrorUpdateResult{is_mirroring_, is_mirroring_ != was_mirroring, "manual F1"};
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
    if (wants_mirror && has_menu_blur) {
        entered_via_menu_blur_ = true;
    }
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

    uint64_t hold_ms = wants_mirror ? static_cast<uint64_t>(config_.enter_mirror_ms)
                                    : static_cast<uint64_t>(config_.exit_mirror_ms);
    if (wants_mirror && has_menu_blur) {
        hold_ms = 0;
    } else if (!wants_mirror && entered_via_menu_blur_) {
        hold_ms = (std::min)(hold_ms, uint64_t{32});
    }
    if ((now_ms - *candidate_start_ms_) >= hold_ms) {
        is_mirroring_ = wants_mirror;
        if (!is_mirroring_) {
            entered_via_menu_blur_ = false;
        }
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        return MirrorUpdateResult{is_mirroring_, true, "auto"};
    }

    return MirrorUpdateResult{is_mirroring_, false, ""};
}

void SplitJoinDetector::Reset(bool suppress_until_shared) {
    suppressed_ = suppress_until_shared;
    split_start_ms_.reset();
    shared_start_ms_.reset();
}

bool SplitJoinDetector::Update(const MirrorConfig& config,
                               std::optional<StripClassification> sample,
                               uint64_t now_ms) {
    if (!config.auto_detect || !sample.has_value()) {
        if (!sample.has_value() || *sample == StripClassification::Ambiguous) {
            split_start_ms_.reset();
            shared_start_ms_.reset();
        }
        return false;
    }

    const StripClassification cls = *sample;
    if (cls == StripClassification::Ambiguous) {
        split_start_ms_.reset();
        shared_start_ms_.reset();
        return false;
    }

    if (suppressed_) {
        split_start_ms_.reset();
        if (cls == StripClassification::Shared) {
            if (!shared_start_ms_.has_value() || now_ms < *shared_start_ms_) {
                shared_start_ms_ = now_ms;
            }
            if ((now_ms - *shared_start_ms_) >= static_cast<uint64_t>(config.enter_mirror_ms)) {
                suppressed_ = false;
                shared_start_ms_.reset();
            }
        } else {
            shared_start_ms_.reset();
        }
        return false;
    }

    shared_start_ms_.reset();
    if (cls == StripClassification::Split) {
        if (!split_start_ms_.has_value() || now_ms < *split_start_ms_) {
            split_start_ms_ = now_ms;
        }
        if ((now_ms - *split_start_ms_) >= static_cast<uint64_t>(config.exit_mirror_ms)) {
            split_start_ms_.reset();
            return true;
        }
    } else {
        split_start_ms_.reset();
    }
    return false;
}

CenteredUiMatch ClassifyCenteredSlateRect(const SlateRect& rect,
                                          uint32_t frame_w,
                                          uint32_t frame_h,
                                          std::vector<SlateRect>& centered_boxes,
                                          uint8_t* out_target_mask) {
    if (out_target_mask) {
        *out_target_mask = kTargetNone;
    }
    if (frame_w < 32 || frame_h < 32 || rect.target_mask == kTargetNone) {
        return CenteredUiMatch::None;
    }
    const float fw = static_cast<float>(frame_w);
    const float fh = static_cast<float>(frame_h);
    const float w = rect.max_x - rect.min_x;
    const float h = rect.max_y - rect.min_y;
    if (w <= 0.0f || h <= 0.0f) {
        return CenteredUiMatch::None;
    }
    if (rect.min_x < -64.0f || rect.max_x > fw + 64.0f ||
        rect.min_y < -64.0f || rect.max_y > fh + 64.0f) {
        return CenteredUiMatch::None;
    }
    // Exclude full-screen fades, tall center divider bars, and 1px screen-edge hairlines.
    if (w >= 0.51f * fw || h >= 0.50f * fh || (w < 32.0f && h > 0.40f * fh) ||
        (h <= 2.0f && (rect.max_y <= 4.0f || rect.min_y >= fh - 4.0f))) {
        return CenteredUiMatch::None;
    }

    const float half_w = 0.5f * fw;
    const float center_x = 0.5f * (rect.min_x + rect.max_x);
    if (rect.has_center_primitive && rect.min_x < half_w - 2.0f && rect.max_x > half_w + 2.0f &&
        std::fabs(center_x - half_w) <= 0.05f * fw) {
        if (centered_boxes.size() < 64) {
            centered_boxes.push_back(SlateRect{
                rect.min_x - 32.0f,
                rect.max_x + 32.0f,
                rect.min_y - 12.0f,
                rect.max_y + 12.0f,
                true,
                rect.target_mask,
            });
        }
        if (out_target_mask) {
            *out_target_mask = rect.target_mask;
        }
        return CenteredUiMatch::CrossesCenter;
    }

    for (const auto& box : centered_boxes) {
        if (rect.min_x >= box.min_x && rect.max_x <= box.max_x &&
            rect.min_y >= box.min_y && rect.max_y <= box.max_y) {
            if (out_target_mask) {
                *out_target_mask = box.target_mask;
            }
            return CenteredUiMatch::InsideCenteredBox;
        }
    }
    return CenteredUiMatch::None;
}

ReplicatedScissors ComputeReplicatedScissors(const ScissorBox& cur_sc,
                                             uint32_t frame_w,
                                             uint32_t frame_h,
                                             uint8_t target_mask) {
    ReplicatedScissors res{};
    if (frame_w < 32 || frame_h < 32 || target_mask == kTargetNone) {
        return res;
    }
    const int32_t fw = static_cast<int32_t>(frame_w);
    const int32_t fh = static_cast<int32_t>(frame_h);
    const int32_t half_w = fw / 2;
    const int32_t quarter_w = fw / 4;
    const int32_t top = (std::max)(0, cur_sc.top);
    const int32_t bottom = (std::min)(fh, cur_sc.bottom);
    if (bottom <= top) {
        return res;
    }

    const bool is_p1_half_sc = (cur_sc.left <= 2 && NearInt(cur_sc.right, half_w));
    const bool is_p2_half_sc = (NearInt(cur_sc.left, half_w) && cur_sc.right >= fw - 2);

    if ((target_mask & kTargetLeft) != 0) {
        const int32_t l = is_p1_half_sc ? 0 : std::clamp(cur_sc.left - quarter_w, 0, half_w);
        const int32_t r = is_p1_half_sc ? half_w : std::clamp(cur_sc.right - quarter_w, 0, half_w);
        if (r > l) {
            res.draw_left = true;
            res.left = ScissorBox{l, top, r, bottom};
        }
    }
    if ((target_mask & kTargetRight) != 0) {
        const int32_t l = is_p2_half_sc ? half_w : std::clamp(cur_sc.left + quarter_w, half_w, fw);
        const int32_t r = is_p2_half_sc ? fw : std::clamp(cur_sc.right + quarter_w, half_w, fw);
        if (r > l) {
            res.draw_right = true;
            res.right = ScissorBox{l, top, r, bottom};
        }
    }
    return res;
}

}  // namespace gotham
