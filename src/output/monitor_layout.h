#pragma once

#include <algorithm>
#include <cstdint>

namespace gotham {

struct LayoutRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t width() const { return right - left; }
    int32_t height() const { return bottom - top; }
    bool operator==(const LayoutRect& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
};

struct DuoLayout {
    /// Game window rect in Duo split mode (width = 2 * half.w).
    LayoutRect span;
    /// Per-player render size (identical for both players so neither half is ever cropped).
    uint32_t half_w = 0;
    uint32_t half_h = 0;
    /// Where each player's output window sits inside its monitor (aspect-preserving fit).
    LayoutRect out[2];
};

/// Fits content_w x content_h into `monitor` preserving aspect ratio, centered (letterbox/pillarbox).
inline LayoutRect FitRect(uint32_t content_w, uint32_t content_h, const LayoutRect& monitor) {
    const int64_t mw = monitor.width();
    const int64_t mh = monitor.height();
    if (content_w == 0 || content_h == 0 || mw <= 0 || mh <= 0) {
        return monitor;
    }
    int64_t w = mw;
    int64_t h = mh;
    if (static_cast<int64_t>(content_w) * mh <= static_cast<int64_t>(content_h) * mw) {
        w = (std::max<int64_t>)(1, static_cast<int64_t>(content_w) * mh / content_h);
    } else {
        h = (std::max<int64_t>)(1, static_cast<int64_t>(content_h) * mw / content_w);
    }
    const int32_t left = monitor.left + static_cast<int32_t>((mw - w) / 2);
    const int32_t top = monitor.top + static_cast<int32_t>((mh - h) / 2);
    return LayoutRect{left, top, left + static_cast<int32_t>(w), top + static_cast<int32_t>(h)};
}

/// Both players render at the smaller monitor's resolution (or the ini span override), so mixed
/// resolutions never clip; each output window is then scaled/letterboxed onto its own monitor.
inline DuoLayout ComputeDuoLayout(const LayoutRect& p1,
                                  const LayoutRect& p2,
                                  uint32_t target_width,
                                  uint32_t target_height) {
    DuoLayout l{};
    const uint32_t auto_w = static_cast<uint32_t>((std::max)(1, (std::min)(p1.width(), p2.width())));
    const uint32_t auto_h =
        static_cast<uint32_t>((std::max)(1, (std::min)(p1.height(), p2.height())));
    l.half_w = target_width > 0 ? (std::max)(1u, target_width / 2) : auto_w;
    l.half_h = target_height > 0 ? target_height : auto_h;

    const int32_t left = (std::min)(p1.left, p2.left);
    const int32_t top = (std::min)(p1.top, p2.top);
    l.span = LayoutRect{left,
                        top,
                        left + static_cast<int32_t>(2 * l.half_w),
                        top + static_cast<int32_t>(l.half_h)};
    l.out[0] = FitRect(l.half_w, l.half_h, p1);
    l.out[1] = FitRect(l.half_w, l.half_h, p2);
    return l;
}

/// Mirrored menus use the per-player size (so both monitors share one aspect) unless overridden.
inline LayoutRect ComputeMirrorRect(const LayoutRect& solo,
                                    const DuoLayout& duo,
                                    uint32_t solo_width,
                                    uint32_t solo_height) {
    const int32_t w = static_cast<int32_t>(solo_width > 0 ? solo_width : duo.half_w);
    const int32_t h = static_cast<int32_t>(solo_height > 0 ? solo_height : duo.half_h);
    return LayoutRect{solo.left, solo.top, solo.left + w, solo.top + h};
}

/// Solo mode keeps the game at the primary monitor's native size unless overridden.
inline LayoutRect ComputeSoloRect(const LayoutRect& solo,
                                  uint32_t solo_width,
                                  uint32_t solo_height) {
    const int32_t w = static_cast<int32_t>(solo_width > 0 ? solo_width : solo.width());
    const int32_t h = static_cast<int32_t>(solo_height > 0 ? solo_height : solo.height());
    return LayoutRect{solo.left, solo.top, solo.left + w, solo.top + h};
}

}  // namespace gotham
