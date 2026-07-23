// DKII 0x0057BBF0: sub_57BBF0 -- batched sphere-cull bitmask.
//
// __fastcall leaf on the appendToSceneObject2EList render hot path (0x584900
// calls it twice). For each sphere entry in the object's array, filtered by
// (entry->flags & mask) == mask, tests whether point (X,Y,Z) is WITHIN the
// sphere inflated by W, and accumulates a result bitmask (bit i set = overlap/
// in-range). Verified against the original SIMD translation's sign convention
// (distanceSquared - radiusSum^2 < 0 => in range) -- an earlier version of
// this function had the subtraction order (and therefore the branch) flipped,
// which inverted every bit and broke light selection game-wide.
// Range and bit-numbering are selected by mask bits 0 and 5.
//
// Replaces an x87 leaf (14 x87 instrs, 0 calls, no globals). The original kept
// the (radius+W) term in 80-bit x87 across its square while rounding the
// per-axis deltas to 32-bit via fsts; this impl uses float throughout. The
// output is a DISCRETE bitmask of sign tests, so results are identical for all
// realistic inputs and differ only when the true scalar is within ~1e-7 of 0
// (point on the sphere surface) -- a vanishingly rare game-world case. Build
// difftests with -ffp-contract=off so mul/add don't fuse.
//
// Layout iterated (offsets from disasm):
//   obj +0  int32 base      (start-index base)
//   obj +4  int32 count     (end = base + count)
//   obj +0x38 ...           array of entry* (4 bytes each)
//   entry +4  int32 flags   ((flags & mask) == mask gates the test)
//   entry +8/+0xC/+0x10     float center cx,cy,cz
//   entry +0x20             float radius
#include <cstdint>
#include <cstring>

namespace dk2 {

static inline int32_t load_i32(const unsigned char *p) {
    uint32_t u = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                 (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    return static_cast<int32_t>(u);
}
static inline float load_f32(const unsigned char *p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

// __fastcall: ecx=obj, edx=arg2 (UNUSED -- body clobbers edx immediately as the
// bit accumulator), stack X,Y,Z,W,mask. Returns the bitmask.
int __fastcall sub_57BBF0(int *objRaw, void * /*edx*/, float X, float Y,
                          float Z, float W, int mask) {
    const auto *obj = reinterpret_cast<const unsigned char *>(objRaw);
    const int32_t base = load_i32(obj + 0x00);
    const int32_t count = load_i32(obj + 0x04);

    // Range + initial bit position from mask bits 0 and 5 (x86 `shl cl` masks
    // the count to 5 bits, hence base & 31).
    const bool selRange = (mask & 1u) != 0;
    const bool clampEmpty = (mask & 0x20u) != 0;
    const int32_t start = selRange ? base : 0;
    const int32_t end = clampEmpty ? base : (base + count);
    if (start >= end) return 0;
    const uint32_t bit0 = selRange ? static_cast<uint32_t>(base & 31) : 0u;

    const auto *slots = reinterpret_cast<unsigned char *const *>(obj + 0x38);
    uint32_t result = 0;
    uint32_t bit = 1u << bit0;
    for (int32_t i = start; i < end; ++i) {
        const auto *entry = reinterpret_cast<const unsigned char *>(slots[i]);
        const int32_t flags = load_i32(entry + 0x04);
        if ((flags & mask) == mask) {
            const float cx = load_f32(entry + 0x08);
            const float cy = load_f32(entry + 0x0C);
            const float cz = load_f32(entry + 0x10);
            const float r = load_f32(entry + 0x20);
            const float dx = X - cx, dy = Y - cy, dz = Z - cz;
            const float rW = r + W;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            const float scalar = distanceSquared - rW * rW;
            if (scalar < 0.0f) result |= bit;  // point within inflated sphere
        }
        bit <<= 1;
    }
    return static_cast<int32_t>(result);
}

}  // namespace dk2
