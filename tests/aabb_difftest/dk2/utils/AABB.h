// Offline stub of the genapi-generated dk2/utils/AABB.h (struct id
// constructor_00404DB0). Layout: int minX,minY,maxX,maxY (0x10).
#pragma once
#include <cstdint>
using BOOL = int;
struct tagPOINT { int32_t x, y; };
namespace dk2 {
#pragma pack(push, 1)
struct AABB {
    int32_t minX, minY, maxX, maxY;
    BOOL contains(AABB *other);
    BOOL isIntersects(AABB *other);
    AABB *intersection(AABB *output, AABB *other);
    AABB *getOuter(AABB *output, AABB *other);
    AABB *appendPoint(AABB *output, tagPOINT *point);
    int move(int newX, int newY);
};
#pragma pack(pop)
static_assert(sizeof(AABB) == 0x10);
}
