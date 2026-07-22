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

// 0x00556590: translate this by point (add point to all 4 fields — NOT a
// min/max grow despite the name), then copy this to output. param1=output
// (copied, returned), param2=point. this mutated. Two's-complement wraparound.
dk2::AABB *dk2::AABB::appendPoint(AABB *output, tagPOINT *point) {
    const int32_t px = point->x;
    const int32_t py = point->y;
    minX = static_cast<int32_t>(static_cast<uint32_t>(minX) + static_cast<uint32_t>(px));
    minY = static_cast<int32_t>(static_cast<uint32_t>(minY) + static_cast<uint32_t>(py));
    maxX = static_cast<int32_t>(static_cast<uint32_t>(maxX) + static_cast<uint32_t>(px));
    maxY = static_cast<int32_t>(static_cast<uint32_t>(maxY) + static_cast<uint32_t>(py));
    *output = *this;
    return output;
}

// 0x005DC2D0: move this so minX->newX, minY->newY, preserving width/height
// (maxX += newX - minX, maxY += newY - minY). Returns newX (eax in original).
// this mutated in place. Modular arithmetic is associative, so the delta
// computation matches x86 sub/add regardless of grouping.
int dk2::AABB::move(int newX, int newY) {
    const uint32_t dx = static_cast<uint32_t>(newX) - static_cast<uint32_t>(minX);
    const uint32_t dy = static_cast<uint32_t>(newY) - static_cast<uint32_t>(minY);
    maxX = static_cast<int32_t>(static_cast<uint32_t>(maxX) + dx);
    maxY = static_cast<int32_t>(static_cast<uint32_t>(maxY) + dy);
    minX = newX;
    minY = newY;
    return newX;
}
