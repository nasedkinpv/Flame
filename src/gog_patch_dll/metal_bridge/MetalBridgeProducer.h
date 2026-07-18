#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>
#include <ddraw.h>

namespace gog::metal_bridge {

void beginFrame(DWORD width, DWORD height);
void drawIndexed(DWORD fvf, const void *vertices, DWORD vertexCount,
                 const WORD *indices, DWORD indexCount, DWORD flags);
void setTexture(DWORD stage, DWORD textureId, IDirectDrawSurface4 *surface);
void endFrame();

}

#endif
