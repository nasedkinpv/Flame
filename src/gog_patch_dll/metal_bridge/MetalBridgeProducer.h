#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>
#include <ddraw.h>
#include <cstdint>

namespace gog::metal_bridge {

bool isEnabled();
void pollInput();
void beginFrame(DWORD width, DWORD height);
void drawIndexed(DWORD fvf, const void *vertices, DWORD vertexCount,
                 const WORD *indices, DWORD indexCount, DWORD flags);
void captureOverlay(IDirectDrawSurface4 *surface);
void setTexture(DWORD stage, DWORD textureId, IDirectDrawSurface4 *surface);
void textureDirty(IDirectDrawSurface4 *surface, const DDSURFACEDESC2 *lockedDesc = nullptr);
void setRenderState(DWORD state, DWORD value);
bool getRenderState(DWORD state, DWORD *value);
void setTextureStageState(DWORD stage, DWORD state, DWORD value);
void setGameTickTiming(uint32_t tickMicroseconds);
void setGameRenderTimings(uint32_t prepareMicroseconds, uint32_t drawMicroseconds);
void endFrame();

}

#endif
