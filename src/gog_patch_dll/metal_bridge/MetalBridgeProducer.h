#ifndef DK2_METAL_BRIDGE_PRODUCER_H
#define DK2_METAL_BRIDGE_PRODUCER_H

#include <Windows.h>
#include <ddraw.h>
#include <cstdint>

namespace gog::metal_bridge {

bool isEnabled();
bool metalShadowsEnabled();
// Live host-owned presentation scale shared by the final Metal cursor quad
// and the translated DK2 tooltip placement. Returns 1.0 without a host.
float cursorScale();
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
// One mesh instance; world = row-major 3x4. Mesh flags carry the original
// per-draw blend and depth contract because retained commands may be staged
// outside the surrounding legacy state stream.
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
// --- native scene mirror (Phase 1, LOG-ONLY; gate emit on
// flametal:native_scene_mirror, default off) ---
// Register one static scene object into the host-side mirror registry for
// future native culling. signature/vertex_count come PRE-COMPUTED from the
// guest (describe stays guest-side -- host cannot read x86 vertex buffers
// without shared-memory, out of scope). Versioned by an internal scene epoch
// that sceneReset() bumps (level-load / save-load); the host drops its
// registry on epoch change. Nothing is consumed yet -- observational only.
void sceneRegister(uint32_t objectId, uint32_t meshId, uint64_t signature,
                   uint32_t vertexCount, uint32_t materialFlags,
                   const float world[12], const float center[3], float radius);
void sceneReset();
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
bool bufferTextureNeedsRefresh(uint32_t textureId);
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
// Replaces the CPU coverage build for one original DK2 shadow surface.
// Immediate-mode: `pageSurface` and the rect are the placement resolved at
// capture time by the caller (finishIfCurrent); the producer resolves the
// bridge texture id, stamps the page's CURRENT generation (v15) and emits
// the mask into the current frame. While a page generation hosts masks, HD
// atlas-map reports for it are dropped; a repack opens a mask-free
// generation and lifts the gate.
void shadowMaskCaptured(IDirectDrawSurface4 *pageSurface, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const void *triangles,
                        uint32_t triangleCount, uint32_t mode);
// v15 semantic decal scope: the scene walk brackets the submission of a
// ToDraw batch whose EVERY SceneObject2E is a shadow decal (f2C_ >= 0x7D0);
// indexed draws emitted inside the scope carry
// DK2M_DRAW_INDEXED_SHADOW_DECAL. noteMixedShadowBatch() reports batches
// that mix shadow and non-shadow objects (expected none; those shadows
// fall back to sampling the page).
void shadowDecalScope(bool active);
void noteMixedShadowBatch();

}

#endif
