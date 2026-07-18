#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>

namespace gog::metal_bridge {

void beginFrame(DWORD width, DWORD height);
void drawIndexed(DWORD fvf, const void *vertices, DWORD vertexCount,
                 const WORD *indices, DWORD indexCount, DWORD flags);
void endFrame();

}

#endif
