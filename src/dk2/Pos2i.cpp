// DKII 0x004D1EC0: Pos2i::shiftDiv — 16.16 fixed-point division.
// Per component: output.c = ((int64)this->c << 16) / divisor, truncated toward
// zero (x86 idiv == C99 division). The original's `shl eax,16 / sar edx,16 /
// idiv` idiom is exactly (int64)c << 16 (verified bit-exact incl. negatives).
// ABI: param1=output (written, returned), param2=divisor. output may alias this.
// x86 idiv faults (#DE) on divisor==0 or quotient-overflow — caller must avoid.
#include "dk2/utils/Pos2i.h"

#include <cstdint>

dk2::Pos2i *dk2::Pos2i::shiftDiv(Pos2i *output, int divisor) {
    output->x = static_cast<int>((static_cast<int64_t>(x) << 16) / divisor);
    output->y = static_cast<int>((static_cast<int64_t>(y) << 16) / divisor);
    return output;
}
