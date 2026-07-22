#ifndef FLAMETAL_DK2_SHADOW_GPU_H
#define FLAMETAL_DK2_SHADOW_GPU_H

#include <cstdint>

namespace dk2 {
struct MyCESurfHandle;
struct MySurface;
}

namespace dk2::shadowgpu {

// Called by MyCESurfHandle::paint while shadows_end still identifies the
// surface being baked. Returns true when the CPU coverage was intentionally
// left blank and a Metal mask was shipped instead. Immediate-mode: the
// target region is resolved here, at capture time, and travels with the
// mask - there is no later re-resolve step (see shadowMaskCaptured).
bool finishIfCurrent(MyCESurfHandle *handle, const MySurface *source);

// True only while the host has advertised a working, live Metal shadow path.
bool active();

}

#endif
