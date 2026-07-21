#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>
#include <ddraw.h>
#include <cstdint>

namespace gog::metal_bridge {

bool isEnabled();
bool metalShadowsEnabled();
// Enabled by default for the native bridge. Set DK2_HEADLESS_DDRAW=0 for the
// legacy WineD3D A/B fallback.
bool headlessDirectDrawEnabled();
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
void overlayBlt(IDirectDrawSurface4 *destination, const RECT *destinationRect,
                IDirectDrawSurface4 *source, const RECT *sourceRect,
                DWORD flags);
// Publishes the current game cursor as an independent final Metal draw.  The
// caller can then skip the legacy Blt into the black/white overlay surface.
bool cursor(IDirectDrawSurface *source, DWORD width, DWORD height, DWORD colorKey,
            LONG mouseX, LONG mouseY, LONG hotspotX, LONG hotspotY);
void hideCursor();
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

// --- world-space mesh pipeline (introduced in v9; retained/deformed v13) ---
// Register an object-space mesh once; safe to call repeatedly with the same
// id (later calls are no-ops unless the consumer session changed). Returns
// false when the bridge is disabled.
struct DK2MMeshVertexData;  // = DK2MMeshVertex from the protocol header
bool meshRegister(uint32_t meshId, const void *vertices, uint32_t vertexCount,
                  const uint16_t *indices, uint32_t indexCount);
// Per-frame camera (column-major world->clip 4x4) + piecewise depth params
// {z_mul2, z_add2, z_add3, z_mul3*F, far_threshold, depth_cap}.
void cameraSet(const float viewProj[16], const float depthParams[6]);
// Per-frame light list + the engine's 256-entry falloff LUT + ambient.
void lightsSet(const void *lights, uint32_t lightCount,
               float ambientR, float ambientG, float ambientB,
               const float falloffLut[256]);
// One mesh instance; world = row-major 3x4. Depth/blend context comes from
// the surrounding setRenderState stream, same as drawIndexed.
void drawMesh(uint32_t meshId, uint32_t textureId, const float world[12],
              const float uvTransform[4],
              uint32_t tint, uint32_t flags,
              const uint16_t *lightIndices, uint32_t lightCount,
              float ambientR, float ambientG, float ambientB);
// Retained topology/attributes with per-frame packed float3 positions.
void drawMeshDeformed(uint32_t meshId, uint32_t textureId,
                      const float *positions, uint32_t vertexCount,
                      const float world[12], const float uvTransform[4],
                      uint32_t tint, uint32_t flags,
                      const uint16_t *lightIndices, uint32_t lightCount,
                      float ambientR, float ambientG, float ambientB);
// Inline world-space draw for deformed geometry: vertices are DK2MMeshVertex
// (world space), indices uint16; travels with the frame, GPU still projects
// and lights. World transform is implicitly identity.
void drawMeshInline(uint32_t textureId, const void *vertices, uint32_t vertexCount,
                    const uint16_t *indices, uint32_t indexCount, uint32_t tint,
                    uint32_t flags, const uint16_t *lightIndices,
                    uint32_t lightCount, float ambientR, float ambientG,
                    float ambientB);
// Last begun frame's dimensions (stable during the game's prepare phase).
void frameSize(uint32_t *width, uint32_t *height);
// Monotonic finished-frame counter - stable frame identity for callers.
uint32_t frameCounter();
// Capture-only texture registration for mesh draws (no stage-state binding).
void ensureTexture(DWORD textureId, IDirectDrawSurface4 *surface);
// Bridge id for a surface with no FakeTexture (synthetic id namespace).
uint32_t ensureSurfaceTexture(IDirectDrawSurface4 *surface);
// Bridge id for a raw BGRA32 CPU buffer (engine surface page).
uint32_t ensureBufferTexture(const void *key, const void *pixels, uint32_t width,
                             uint32_t height, uint32_t pitchBytes);
// Ends producer and host ownership for a texture. `surfaceReleased` resolves
// synthetic ids by their surface/buffer key; `textureReleased` is used by a
// FakeTexture whose bridge id is already known.
void surfaceReleased(const void *key);
void textureReleased(DWORD textureId, const void *key = nullptr);
// Named-atlas map (HD resource pack): the rect [x,y,w,h] of the page surface
// identified by `pageKey` (the CEngineSurfaceBase*/IDirectDrawSurface4* the
// producer keys its texture ids by) was composited from resource `name`.
// Mip-suffixed names are filtered producer-side: "…MM0" is stripped, deeper
// mips are dropped. Reports made before the page has a texture id are queued
// and flushed when the id first appears.
// Atlas page repack: drop the page's map on both sides (see
// DK2M_COMMAND_PAGE_ATLAS_RESET).
void atlasPageReset(const void *pageKey);
void reportAtlasRect(const void *pageKey, const char *name, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h);
// Replaces the CPU coverage build for one original DK2 shadow surface. The
// handle key is resolved to its current atlas placement only when that atlas
// is actually bound, after the engine's surface packer has run.
void shadowMask(const void *handleKey, const void *triangles,
                uint32_t triangleCount, uint32_t mode);

}

#endif
