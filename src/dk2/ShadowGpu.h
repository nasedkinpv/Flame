#ifndef FLAMETAL_DK2_SHADOW_GPU_H
#define FLAMETAL_DK2_SHADOW_GPU_H

#include <cstdint>

namespace dk2 {
struct MyCESurfHandle;
struct MySurface;
}

namespace dk2::shadowgpu {

struct TargetRegion {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

// Called by MyCESurfHandle::paint while shadows_end still identifies the
// surface being baked. Returns true when the CPU coverage was intentionally
// left blank and a Metal mask was queued instead.
bool finishIfCurrent(MyCESurfHandle *handle, const MySurface *source);

// Resolves a queued handle after the original atlas packer has assigned its
// holder. `boundSurface` is the bridge's IDirectDrawSurface4 identity.
bool resolveTarget(const void *handleKey, const void *boundSurface,
                   TargetRegion *out);

// True only while the host has advertised a working, live Metal shadow path.
bool active();

}

#endif
