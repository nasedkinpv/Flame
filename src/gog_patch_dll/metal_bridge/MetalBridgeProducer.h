#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>
#include <ddraw.h>
#include <cstdint>

namespace gog::metal_bridge {

bool isEnabled();
void pollInput();
void beginFrame(DWORD width, DWORD height);
DWORD overlayClearColor();
// dirty-region reporting for the overlay surface: the per-frame colorfill
// resets the drawn region, every Lock/Blt on the overlay unions into it, and
// the capture then reads only what was actually drawn instead of the full
// screen every frame
void overlayCleared();
void overlayDrawn(const RECT *rect);  // null = whole surface
// DK2 draws its colour-keyed software cursor from a worker thread.  Capture it
// separately so the two-frame black/white overlay reconstruction does not turn
// cursor movement into translucent trails.
void overlayBltFast(IDirectDrawSurface4 *destination, DWORD x, DWORD y,
                    IDirectDrawSurface4 *source, const RECT *sourceRect,
                    DWORD flags);
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
