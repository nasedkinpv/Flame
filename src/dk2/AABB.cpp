// DKII AABB integer methods. All leaf, no globals, signed compares.
// ABI (param1 = output written/returned, param2 = other read) — output may
// alias this/other since all inputs are read into registers before writing.
#include "dk2/utils/AABB.h"

#include <algorithm>

// 0x0052D3A0: TRUE iff `this` fully bounds `other`.
BOOL dk2::AABB::contains(AABB *other) {
    return minX <= other->minX
        && minY <= other->minY
        && maxX >= other->maxX
        && maxY >= other->maxY;
}

// 0x005B7050: inclusive overlap (touching edges count).
BOOL dk2::AABB::isIntersects(AABB *other) {
    return minX <= other->maxX
        && maxX >= other->minX
        && minY <= other->maxY
        && maxY >= other->minY;
}

// 0x005B6FD0: overlap box. min = max of mins, max = min of maxes, then clamp
// max up to min per axis (disjoint boxes yield a zero-size box at the boundary).
dk2::AABB *dk2::AABB::intersection(AABB *output, AABB *other) {
    const int32_t mnX = std::max(minX, other->minX);
    const int32_t mnY = std::max(minY, other->minY);
    int32_t mxX = std::min(maxX, other->maxX);
    int32_t mxY = std::min(maxY, other->maxY);
    mxX = std::max(mxX, mnX);
    mxY = std::max(mxY, mnY);
    output->minX = mnX;
    output->minY = mnY;
    output->maxX = mxX;
    output->maxY = mxY;
    return output;
}

// 0x005B7090: union / bounding box. min = min of mins, max = max of maxes.
dk2::AABB *dk2::AABB::getOuter(AABB *output, AABB *other) {
    output->minX = std::min(minX, other->minX);
    output->minY = std::min(minY, other->minY);
    output->maxX = std::max(maxX, other->maxX);
    output->maxY = std::max(maxY, other->maxY);
    return output;
}
