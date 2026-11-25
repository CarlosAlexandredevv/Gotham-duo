#include "split_seam_detector.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace gotham {
namespace {

struct Rgb {
    int r;
    int g;
    int b;
};

Rgb DecodePixel(uint32_t raw, DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return Rgb{
            static_cast<int>((raw >> 16) & 0xFFu),
            static_cast<int>((raw >> 8) & 0xFFu),
            static_cast<int>(raw & 0xFFu),
        };
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return Rgb{
            static_cast<int>((raw >> 2) & 0xFFu),
            static_cast<int>((raw >> 12) & 0xFFu),
            static_cast<int>((raw >> 22) & 0xFFu),
        };
    }
    return Rgb{
        static_cast<int>(raw & 0xFFu),
        static_cast<int>((raw >> 8) & 0xFFu),
        static_cast<int>((raw >> 16) & 0xFFu),
    };
}

int ColorDistL1(Rgb a, Rgb b) {
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
    const uint32_t y_begin = height / 8;
    const uint32_t y_end = height - height / 8;

    uint32_t sampled_rows = 0;
    uint32_t active_rows = 0;
    uint32_t seam_rows = 0;

    for (uint32_t y = y_begin; y < y_end; y += 4) {
        ++sampled_rows;
        const uint8_t* row_bytes = base + static_cast<size_t>(y) * row_pitch;
        std::array<Rgb, kSeamStripWidth> px{};
        for (uint32_t x = 0; x < kSeamStripWidth; ++x) {
            uint32_t raw = 0;
            std::memcpy(&raw, row_bytes + static_cast<size_t>(x) * sizeof(uint32_t), sizeof(raw));
            px[x] = DecodePixel(raw, format);
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
        if (seam_immediate > (local_max * 2 + 45) ||
            seam_wide > (outer_local_max * 2 + 60)) {
            ++seam_rows;
        }
    }

    if (sampled_rows == 0 || active_rows == 0 || active_rows * 6 < sampled_rows) {
        return StripClassification::Ambiguous;
    }

    const double seam_ratio =
        static_cast<double>(seam_rows) / static_cast<double>(active_rows);
    if (seam_ratio >= 0.35) {
        return StripClassification::Split;
    }
    if (seam_ratio <= 0.20) {
        return StripClassification::Shared;
    }
    return StripClassification::Ambiguous;
}

void MirrorStateMachine::SetConfig(MirrorConfig config) {
    config_ = config;
    Reset();
}

void MirrorStateMachine::Reset(bool f8_down) {
    override_mode_ = config_.auto_detect ? MirrorOverrideMode::Auto
                                         : MirrorOverrideMode::ManualSplit;
    is_mirroring_ = false;
    prev_f8_down_ = f8_down;
    candidate_kind_.reset();
    candidate_start_ms_.reset();
}

MirrorUpdateResult MirrorStateMachine::Update(bool f8_down,
                                              std::optional<StripClassification> sample,
                                              uint64_t now_ms) {
    const bool f8_rising = f8_down && !prev_f8_down_;
    prev_f8_down_ = f8_down;

    if (f8_rising) {
        candidate_kind_.reset();
        candidate_start_ms_.reset();
        const bool was_mirroring = is_mirroring_;

        if (!config_.auto_detect || override_mode_ == MirrorOverrideMode::Auto) {
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

}  // namespace gotham
