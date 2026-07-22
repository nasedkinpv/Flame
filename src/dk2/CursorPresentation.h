#pragma once

#include "dk2/utils/AABB.h"
#include "dk2/utils/Pos2i.h"
#include "dk2/utils/Size2i.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dk2::cursor_presentation {

inline int scaledMetric(int value, float cursorScale) {
    return static_cast<int>(std::lround(static_cast<float>(value) * cursorScale));
}

inline int referenceMetric(int value, int dimension, int referenceDimension) {
    return static_cast<int>(static_cast<int64_t>(value) * dimension / referenceDimension);
}

struct TooltipPlacement {
    int x;
    int y;
};

// Translation of the placement block at 0042E7CE..0042E8F1. CursorScale is
// applied only to the cursor-to-tooltip gaps; text centring, side switching
// and the original screen-edge clamps remain unchanged.
inline TooltipPlacement placeTooltip(
        const Pos2i &mouse,
        const AABB &textBounds,
        const Size2i &viewport,
        const Size2i &reference,
        bool alternateOffset,
        float cursorScale) {
    const int textWidth = textBounds.maxX - textBounds.minX;
    const int textHeight = textBounds.maxY - textBounds.minY;
    cursorScale = std::clamp(cursorScale, 0.25f, 2.0f);
    const int rightGap = scaledMetric(referenceMetric(
        alternateOffset ? 60 : 98, reference.w, 640), cursorScale);
    const int leftGap = scaledMetric(
        referenceMetric(30, reference.w, 640), cursorScale);

    TooltipPlacement placement {
        mouse.x + rightGap,
        alternateOffset ? mouse.y - textHeight / 2 : mouse.y - textHeight,
    };

    const int rightMargin = referenceMetric(40, reference.w, 640);
    if (placement.x + textWidth > viewport.w - rightMargin) {
        placement.x = mouse.x - textWidth - leftGap;
        placement.y = mouse.y - textHeight / 2;
    }

    const int minY = referenceMetric(40, reference.h, 480);
    const int maxY = viewport.h - referenceMetric(10, reference.h, 480) - textHeight;
    if (placement.y <= minY) placement.y = minY;
    if (placement.y >= maxY) placement.y = maxY;
    return placement;
}

}  // namespace dk2::cursor_presentation
