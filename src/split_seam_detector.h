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
