#pragma once

#include <cstdint>
#include <optional>

namespace gotham {

/// Cooldown and retry-limit guard for re-applying the Duo span window size when the game reverts.
struct DuoResizeGuard {
    static constexpr uint64_t kCooldownMs = 1000;
    static constexpr int kMaxRetries = 5;

    int retry_count = 0;
    std::optional<uint64_t> last_resize_ms;
    bool limit_logged = false;

    void Reset() {
        retry_count = 0;
        last_resize_ms.reset();
        limit_logged = false;
    }

    void NoteResizeRequested(uint64_t now_ms) {
        last_resize_ms = now_ms;
    }

    /// Returns true when a Duo resize should be re-applied for a mismatched backbuffer size.
    bool ShouldReapply(uint32_t cur_w,
                       uint32_t cur_h,
                       uint32_t expected_w,
                       uint32_t expected_h,
                       uint64_t now_ms,
                       bool& out_hit_limit) {
        out_hit_limit = false;
        if (cur_w == 0 || cur_h == 0 || expected_w == 0 || expected_h == 0) {
            return false;
        }
        if (cur_w == expected_w && cur_h == expected_h) {
            Reset();
            return false;
        }
        if (last_resize_ms.has_value()) {
            if (now_ms < *last_resize_ms) {
                last_resize_ms = now_ms;
                return false;
            }
            if ((now_ms - *last_resize_ms) < kCooldownMs) {
                return false;
            }
        }
        if (retry_count >= kMaxRetries) {
            if (!limit_logged) {
                limit_logged = true;
                out_hit_limit = true;
            }
            return false;
        }
        ++retry_count;
        last_resize_ms = now_ms;
        return true;
    }
};

}  // namespace gotham
