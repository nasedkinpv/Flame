// DKII 0x0052D3A0 / 0x005B7050: AABB::contains / isIntersects — integer
// bounding-box predicates. Leaf, read-only, signed compares. this=ecx, one
// AABB* arg (other), return BOOL (0/1), ret 4.
//
// contains:      TRUE iff `this` fully bounds `other`:
//                minX<=other.minX && minY<=other.minY
//                && maxX>=other.maxX && maxY>=other.maxY
// isIntersects:  inclusive AABB overlap (touching edges count):
//                minX<=other.maxX && maxX>=other.minX
//                && minY<=other.maxY && maxY>=other.minY
#include "dk2/utils/AABB.h"

BOOL dk2::AABB::contains(AABB *other) {
    return minX <= other->minX
        && minY <= other->minY
        && maxX >= other->maxX
        && maxY >= other->maxY;
}

BOOL dk2::AABB::isIntersects(AABB *other) {
    return minX <= other->maxX
        && maxX >= other->minX
        && minY <= other->maxY
        && maxY >= other->minY;
}
