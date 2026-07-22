// Offline stub of the genapi-generated dk2/utils/Pos2i.h (struct id pos_xy).
// Layout must match the generated struct.
#pragma once
#include <cstdint>
namespace dk2 {
#pragma pack(push, 1)
struct Pos2i {
    int32_t x, y;
    Pos2i *shiftDiv(Pos2i *output, int divisor);
};
#pragma pack(pop)
static_assert(sizeof(Pos2i) == 0x8);
}
