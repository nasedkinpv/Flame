#include "dk2/Obj57AD20.h"

#include "dk2/CEngineDDSurface.h"
#include "dk2/CEngineSurfaceBase.h"
#include "dk2/SurfaceHolder.h"
#include "dk2/MyCESurfHandle.h"
#include "dk2/MyCESurfScale.h"
#include "dk2/MyScaledSurface.h"
#include "dk2/Obj57BCB0.h"
#include "dk2/Obj58EF60.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"
#include <fake/FakeTexture.h>
#include <metal_bridge/DK2BridgeProtocol.h>
#include <metal_bridge/MetalBridgeProducer.h>
#include <tools/flame_config.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <emmintrin.h>
#include <vector>

// Reroutes the translated deformed-mesh emitter (sub_57B6D0) to the Metal
// bridge's world-space mesh pipeline: the GPU does projection and per-vertex
// point-light accumulation instead of the original per-vertex CPU loop.
flame_config::define_flame_option<bool> o_gog_meshGpuPath(
    "gog:MeshGpuPath", flame_config::OG_Config,
    "Emit dynamic meshes through the Metal world-space pipeline (GPU transform + lighting)",
    false
);


namespace {

#pragma pack(push, 1)
struct MeshVertex {
    dk2::Vec3f position;
    uint32_t packedUv;
    dk2::Vec3f normal;
    dk2::Vec3f color;
};

struct MeshEntry {
    uint32_t unused;
    int surfaceIndex;
    const uint8_t *triangleIndices;
    MeshVertex *vertices;
    uint8_t triangleCount;
    uint8_t padding[3];
};

struct SpatialSphere {
    uint32_t unused;
    uint32_t flags;
    dk2::Vec3f center;
    uint8_t padding[0x0C];
    float radius;
};
#pragma pack(pop)

static_assert(sizeof(MeshVertex) == 0x28);
static_assert(offsetof(MeshVertex, position) == 0x00);
static_assert(offsetof(MeshVertex, color) == 0x1C);
static_assert(sizeof(MeshEntry) == 0x14);
static_assert(offsetof(SpatialSphere, flags) == 0x04);
static_assert(offsetof(SpatialSphere, center) == 0x08);
static_assert(offsetof(SpatialSphere, radius) == 0x20);

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

void transformPosition(VertexFun fun, uint32_t index, dk2::Vec3f *position) {
    if (fun == reinterpret_cast<VertexFun>(0x0058ACB0)) {
        dk2::sub_58ACB0(index, position);
    } else if (fun == reinterpret_cast<VertexFun>(0x0058AD10)) {
        dk2::sub_58AD10(index, position);
    } else {
        fun(index, position);
    }
}

void transformVertex(VertexFun fun, uint32_t index, MeshVertex *vertex) {
    transformPosition(fun, index, &vertex->position);
}

void emitTriangle(TriangleFun fun, uint32_t a, uint32_t b, uint32_t c) {
    if (fun == reinterpret_cast<TriangleFun>(0x0058B940)) {
        dk2::addTriangleToRender2(a, b, c);
    } else if (fun == reinterpret_cast<TriangleFun>(0x0058B9D0)) {
        dk2::addTriangleToRender1(a, b, c);
    } else {
        fun(a, b, c);
    }
}

void emitVertex(RenderFun fun, uint32_t index, dk2::Vec3f *vectors, dk2::Uv2f *uvs) {
    if (fun == reinterpret_cast<RenderFun>(0x0058B2A0)) {
        dk2::renderFun_sub_58B2A0(index, vectors, uvs);
    } else {
        fun(index, vectors, uvs);
    }
}

// --- GPU mesh path helpers (metal bridge protocol v9) ---

struct SceneLightForGpu {   // mirrors Obj57BCB0.cpp's SceneLight view
    uint32_t unused;
    uint32_t flags;
    dk2::Vec3f position;
    dk2::Vec3f color;
    float queryRadius;
    float distanceSquaredLimit;
    float attenuationScale;
    uint8_t padding[8];
    float facingScale;
};

bool meshGpuActive() {
    return *o_gog_meshGpuPath && gog::metal_bridge::isEnabled();
}

// The colour floats the engine carries per vertex/ambient are stored in the
// float-bias encoding (value = bias1 + bias2 + n); the CPU path recovers n
// via mantissa extraction in colourComponent. Recover it explicitly here.
float debiasColour(float value) {
    const float firstBias = *reinterpret_cast<const float *>(0x0066FE78);
    const float secondBias = *reinterpret_cast<const float *>(0x0066FE8C);
    return value - firstBias - secondBias;
}

uint32_t packBaseColor(const dk2::Vec3f &colour) {
    // Per-vertex colours are plain 0..255 floats - the float-bias encoding
    // lives in the ambient globals only (the CPU path subtracts the bias once
    // from the accumulated sum). Debiasing both painted GPU meshes black.
    auto clampByte = [](float v) -> uint32_t {
        return v <= 0.0f ? 0u : (v >= 255.0f ? 255u : static_cast<uint32_t>(v));
    };
    return (clampByte(colour.x) << 16) | (clampByte(colour.y) << 8) | clampByte(colour.z);
}

// viewProj = P * [M|T] assembled from the same globals RenderData_addToArr
// projects with: screen_x = Ax*F/z*x + Cx, screen_y = Ay*F/z*y + Cy, near
// depth = zAdd3 - zMul3*F/z, view = g_mat_77F3A8 * v + g_vec_77F4C0.
void emitMeshCamera() {
    uint32_t w = 0, h = 0;
    gog::metal_bridge::frameSize(&w, &h);
    if (!w || !h) return;
    const float F = *reinterpret_cast<const float *>(0x0066FE44);
    const float Ax = *reinterpret_cast<const float *>(0x0077F4CC);
    const float Cx = *reinterpret_cast<const float *>(0x0077F4F0);
    const float Ay = *reinterpret_cast<const float *>(0x0078093C);
    const float Cy = *reinterpret_cast<const float *>(0x0077F930);
    const float zAdd3 = dk2::g_zAdd3_7793A0;
    const float zMul3 = dk2::g_zMul3_77F934;
    const auto &M = dk2::g_mat_77F3A8;
    const auto &T = dk2::g_vec_77F4C0;
    const float sx = 2.0f * Ax * F / static_cast<float>(w);
    const float ox = 2.0f * Cx / static_cast<float>(w) - 1.0f;
    const float sy = -2.0f * Ay * F / static_cast<float>(h);
    const float oy = 1.0f - 2.0f * Cy / static_cast<float>(h);
    // sub_594E10 computes out = M^T * v (out_i = sum_c m[c][i] * v_c), so the
    // view rotation rows here are the matrix COLUMNS.
    float R[4][4] = {};
    for (int c = 0; c < 3; ++c) {
        R[0][c] = sx * M.m[c][0] + ox * M.m[c][2];
        R[1][c] = sy * M.m[c][1] + oy * M.m[c][2];
        R[2][c] = zAdd3 * M.m[c][2];
        R[3][c] = M.m[c][2];
    }
    R[0][3] = sx * T.x + ox * T.z;
    R[1][3] = sy * T.y + oy * T.z;
    R[2][3] = zAdd3 * T.z - zMul3 * F;
    R[3][3] = T.z;
    float columnMajor[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            columnMajor[c * 4 + r] = R[r][c];
    // piecewise depth: near = zMul2*z + zAdd2, far = zAdd3 - zMul3*F/z,
    // switch where fd2[5] < z, capped at fd2[3] - replicated in the shader
    const float depthParams[6] = {
        dk2::g_zMul2_77F490, dk2::g_zAdd2_77F4D0,
        zAdd3, zMul3 * F,
        *reinterpret_cast<const float *>(0x0066FE3C),
        *reinterpret_cast<const float *>(0x0066FE34)};
    gog::metal_bridge::cameraSet(columnMajor, depthParams);
}

// Each emitter call carries only its spatially selected light subset, so
// accumulate a per-frame union (deduped by light pointer) and resend the
// growing list; the producer keeps only the last (fullest) payload.
void emitFrameLights(uint32_t *lightData) {
    const auto *lut = reinterpret_cast<const float *>(0x007818A0);
    static std::vector<const SceneLightForGpu *> seen;
    static std::vector<DK2MLight> scratch;
    static uint32_t lastFrame = 0xFFFFFFFFu;
    const uint32_t stamp = gog::metal_bridge::frameCounter();
    if (stamp != lastFrame) {
        lastFrame = stamp;
        seen.clear();
        scratch.clear();
    }
    if (!lightData) {
        gog::metal_bridge::lightsSet(scratch.data(), static_cast<uint32_t>(scratch.size()),
                                     0.0f, 0.0f, 0.0f, lut);
        return;
    }
    const int32_t total = static_cast<int32_t>(lightData[0]) + static_cast<int32_t>(lightData[1]);
    const auto lights = reinterpret_cast<const SceneLightForGpu *const *>(
            reinterpret_cast<const uint8_t *>(lightData) + 0x38);
    for (int32_t i = 0; i < total && i < 512; ++i) {
        if (!lights[i]) continue;
        bool known = false;
        for (const auto *p : seen) {
            if (p == lights[i]) { known = true; break; }
        }
        if (known || scratch.size() >= 512) continue;
        seen.push_back(lights[i]);
        const SceneLightForGpu &s = *lights[i];
        DK2MLight light = {};
        light.px = s.position.x;
        light.py = s.position.y;
        light.pz = s.position.z;
        light.r = s.color.x / 255.0f;
        light.g = s.color.y / 255.0f;
        light.b = s.color.z / 255.0f;
        light.dist_sq_limit = s.distanceSquaredLimit;
        light.atten_scale = s.attenuationScale;
        light.facing_scale = s.facingScale;
        scratch.push_back(light);
    }
    static bool loggedLights = false;
    if (!loggedLights && !scratch.empty()) {
        loggedLights = true;
        patch::log::dbg("mesh gpu lights: total=%d serialized=%u first pos=(%f %f %f) "
                        "col=(%f %f %f) limit=%f atten=%f facing=%f lut0=%f lut16=%f",
                        total, static_cast<uint32_t>(scratch.size()),
                        static_cast<double>(scratch[0].px), static_cast<double>(scratch[0].py),
                        static_cast<double>(scratch[0].pz),
                        static_cast<double>(scratch[0].r), static_cast<double>(scratch[0].g),
                        static_cast<double>(scratch[0].b),
                        static_cast<double>(scratch[0].dist_sq_limit),
                        static_cast<double>(scratch[0].atten_scale),
                        static_cast<double>(scratch[0].facing_scale),
                        static_cast<double>(lut[0]), static_cast<double>(lut[16]));
    }
    gog::metal_bridge::lightsSet(scratch.data(), static_cast<uint32_t>(scratch.size()),
                                 0.0f, 0.0f, 0.0f, lut);
}

// SEH-guarded: level transitions leave stale cesurf/devTex pointers behind,
// and this three-hop chain must degrade to "no texture", never fault.
// devTex is only sometimes our FakeTexture (the legacy SetTexture path made
// it); terrain atlas pages carry a game-side object there whose +0x10 reads
// as garbage. Trust the pointer only when its vtable is literally ours.
bool isOurFakeTexture(void *p) {
    static const void *vtbl = [] {
        gog::FakeTexture probe(nullptr, nullptr);
        return *reinterpret_cast<void **>(&probe);
    }();
    return p && *reinterpret_cast<void **>(p) == vtbl;
}

struct ResolveStats {
    uint32_t calls, nullSurface, noCandidates, cesurfNull, devNull, fakeHit, rawHit, faults;
};
ResolveStats g_resolveStats = {};

uint32_t resolveBridgeTextureIdGuarded(dk2::MyCESurfHandle *slotHandle,
                                       uint32_t *bridgeIdOut, void **bridgeSurfaceOut) {
    __try {
        ++g_resolveStats.calls;
        if (!slotHandle) { ++g_resolveStats.nullSurface; return 0; }
        // Exactly ONE candidate: the handle occupying the scene object's
        // stage slot - its holder placement is what the UV tables describe.
        // Binding any other page (base vs reduction) shows the wrong region.
        dk2::MyCESurfHandle *candidates[1] = {slotHandle};
        const int candidateCount = 1;
        for (int i = 0; i < candidateCount; ++i) {
            dk2::MyCESurfHandle *handle = candidates[i];
            // The actual GPU texture is the HOLDER page the handle was packed
            // into (paint() blends the handle's CPU rect into holder->surf,
            // and the stage-0 UV tables map into holder space) - the 128x128
            // DD-backed atlas pages. Prefer it over the handle's own cesurf.
            if (handle->holder_parent && handle->holder_parent->surf) {
                auto *page = reinterpret_cast<dk2::CEngineDDSurface *>(
                        handle->holder_parent->surf);
                auto *pageTex = reinterpret_cast<gog::FakeTexture *>(page->devTex);
                if (pageTex && isOurFakeTexture(pageTex)) {
                    ++g_resolveStats.fakeHit;
                    *bridgeIdOut = pageTex->bridgeId();
                    *bridgeSurfaceOut = pageTex->bridgeSurface();
                    return 1;
                }
                if (page->surfCreated && page->ddSurf &&
                    reinterpret_cast<uintptr_t>(page->ddSurf) > 0x10000) {
                    ++g_resolveStats.rawHit;
                    *bridgeIdOut = 0;
                    *bridgeSurfaceOut = page->ddSurf;
                    return 1;
                }
            }
            // the engine creates cesurf lazily at paint time; do the same
            if (!handle->cesurf) handle->create();
            if (!handle->cesurf) { ++g_resolveStats.cesurfNull; continue; }
            auto *dd = reinterpret_cast<dk2::CEngineDDSurface *>(handle->cesurf);
            auto *fake = reinterpret_cast<gog::FakeTexture *>(dd->devTex);
            if (fake && isOurFakeTexture(fake)) {
                ++g_resolveStats.fakeHit;
                *bridgeIdOut = fake->bridgeId();
                *bridgeSurfaceOut = fake->bridgeSurface();
                return 1;
            }
            // No lazily-created FakeTexture (the GPU path bypasses legacy
            // SetTexture, so it never gets made) - hand the raw DD surface
            // back for synthetic-id registration.
            // Not a DD-backed surface (terrain atlas pages are plain CPU
            // CEngineSurface objects): capture through the base-class
            // virtuals, which every subclass implements.
            auto *base = reinterpret_cast<dk2::CEngineSurfaceBase *>(handle->cesurf);
            const uint32_t bytesPerPixel = base->fC_desc
                ? *reinterpret_cast<const uint32_t *>(
                      reinterpret_cast<const uint8_t *>(base->fC_desc) + 8)
                : 0;
            if (bytesPerPixel == 4 && base->width > 0 && base->height > 0) {
                void *pixels = base->v_lockBuf();
                if (pixels) {
                    ++g_resolveStats.rawHit;
                    // lineWidth is zero on plain CPU pages - fall back to a
                    // tight row stride
                    const uint32_t stridePixels = base->lineWidth > base->width
                        ? static_cast<uint32_t>(base->lineWidth)
                        : static_cast<uint32_t>(base->width);
                    static bool loggedPage = false;
                    if (!loggedPage) {
                        loggedPage = true;
                        patch::log::dbg("mesh page: w=%d h=%d lineWidth=%d bpp=%u",
                                        base->width, base->height, base->lineWidth,
                                        bytesPerPixel);
                    }
                    const uint32_t id = gog::metal_bridge::ensureBufferTexture(
                        base, pixels,
                        static_cast<uint32_t>(base->width),
                        static_cast<uint32_t>(base->height),
                        stridePixels * 4u);
                    base->v_unlockBuf(reinterpret_cast<int>(pixels));
                    if (id) {
                        *bridgeIdOut = id;
                        *bridgeSurfaceOut = nullptr;
                        return 2;  // already registered, no ensureTexture needed
                    }
                    continue;
                }
            }
            ++g_resolveStats.devNull;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ++g_resolveStats.faults;
        return 0;
    }
}

uint32_t resolveBridgeTextureId(dk2::MyCESurfHandle *slotHandle) {
    static std::vector<const void *> badSurfaces;
    for (const void *bad : badSurfaces) {
        if (bad == slotHandle) return 0;
    }
    uint32_t bridgeId = 0;
    void *bridgeSurface = nullptr;
    const uint32_t resolveMode = resolveBridgeTextureIdGuarded(slotHandle, &bridgeId, &bridgeSurface);
    if (!resolveMode) {
        if (badSurfaces.size() < 4096) badSurfaces.push_back(slotHandle);
        return 0;
    }
    static DWORD lastStatsTick = 0;
    static uint32_t retZero = 0, retNonzero = 0, sampleId = 0;
    if (bridgeId) { ++retNonzero; if (!sampleId) sampleId = bridgeId; }
    else ++retZero;
    const DWORD statsTick = GetTickCount();
    if (statsTick - lastStatsTick > 3000) {
        lastStatsTick = statsTick;
        patch::log::dbg("mesh tex resolve: calls=%u nullSurf=%u noCand=%u cesurfNull=%u "
                        "devNull=%u fakeHit=%u rawHit=%u faults=%u retZero=%u retNonzero=%u sample=%u",
                        g_resolveStats.calls, g_resolveStats.nullSurface,
                        g_resolveStats.noCandidates, g_resolveStats.cesurfNull,
                        g_resolveStats.devNull, g_resolveStats.fakeHit,
                        g_resolveStats.rawHit, g_resolveStats.faults, retZero, retNonzero, sampleId);
    }
    if (resolveMode == 2) return bridgeId;  // buffer texture already registered
    if (bridgeId) {
        // capture-only registration: never disturbs stage-0 binding state
        gog::metal_bridge::ensureTexture(
            bridgeId, static_cast<IDirectDrawSurface4 *>(bridgeSurface));
        return bridgeId;
    }
    if (bridgeSurface) {
        // surface without a FakeTexture: synthetic-id registration
        return gog::metal_bridge::ensureSurfaceTexture(
            static_cast<IDirectDrawSurface4 *>(bridgeSurface));
    }
    return 0;
}

// SEH-guarded raw copy out of a MeshEntry: not every entry reaching
// sub_57B6D0 carries the layout this reroute assumes (a level-load crash
// showed entry.vertices holding float-looking garbage), and a bad pointer
// must mean "fall back to the CPU path", not a page fault. Plain loops only
// inside __try - no C++ objects, so no unwinding is needed.
int copyEntryGuarded(const MeshEntry &entry, uint32_t indexCount,
                     uint32_t *vertexCountOut,
                     DK2MMeshVertex *outVertices, uint32_t outCapacity,
                     uint16_t *outIndices,
                     float uvScale, float uS, float vS, float uO, float vO) {
    __try {
        uint32_t maxIndex = 0;
        for (uint32_t i = 0; i < indexCount; ++i) {
            if (entry.triangleIndices[i] > maxIndex) maxIndex = entry.triangleIndices[i];
            outIndices[i] = entry.triangleIndices[i];
        }
        const uint32_t vertexCount = maxIndex + 1;
        if (vertexCount > outCapacity) return 0;
        for (uint32_t v = 0; v < vertexCount; ++v) {
            const MeshVertex &src = entry.vertices[v];
            DK2MMeshVertex &dst = outVertices[v];
            dst.px = src.position.x;
            dst.py = src.position.y;
            dst.pz = src.position.z;
            dst.nx = src.normal.x;
            dst.ny = src.normal.y;
            dst.nz = src.normal.z;
            dst.u = uS * (static_cast<float>(src.packedUv & 0xFFFF) * uvScale) + uO;
            dst.v = vS * (static_cast<float>(src.packedUv >> 16) * uvScale) + vO;
            dst.base_color = packBaseColor(src.color);
        }
        *vertexCountOut = vertexCount;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Emit one MeshEntry through the bridge's inline world-space path. The UV
// stage-0 scale/offset tables were just written by __renderFun_setSceneObject2E
// for this very scene object, so reading them here matches writeVertex1C.
bool drawEntryOnGpu(dk2::SceneObject2E *scene, MeshEntry &entry,
                    dk2::MyScaledSurface *surface,
                    const dk2::Vec3f &ambient, uint32_t *lightData) {
    if (!entry.triangleCount || !entry.vertices || !entry.triangleIndices) return false;
    const uint32_t indexCount = static_cast<uint32_t>(entry.triangleCount) * 3u;
    // uint8 indices bound both buffers: at most 256 vertices, 255*3 indices
    static DK2MMeshVertex vertices[256];
    static uint16_t indices[765];
    // The UV scale/offset tables are per scene-object STAGE SLOT: find the
    // slot our surface's handle occupies instead of assuming slot 0
    // (multi-texture objects put it elsewhere, shifting every UV).
    int stageSlot = 0;
    dk2::MyCESurfHandle *slotHandle = surface ? surface->surfh : nullptr;
    if (scene) {
        for (int i = 0; i < 4 && i < scene->surfhCount; ++i) {
            if (surface && scene->surfh_x4[i] == surface->surfh) {
                stageSlot = i;
                slotHandle = scene->surfh_x4[i];
                break;
            }
        }
        // reduction in effect: the slot holds a different handle entirely -
        // trust the scene slot, its placement is what the tables map into
        if (scene->surfhCount == 1 && scene->surfh_x4[0]) {
            slotHandle = scene->surfh_x4[0];
            stageSlot = 0;
        }
    }
    const float uvScale = *reinterpret_cast<const float *>(0x0066FB58);
    const float uS = reinterpret_cast<const float *>(0x00779368)[stageSlot];
    const float vS = reinterpret_cast<const float *>(0x0076F340)[stageSlot];
    const float uO = reinterpret_cast<const float *>(0x0077F480)[stageSlot];
    const float vO = reinterpret_cast<const float *>(0x0077F3D8)[stageSlot];
    // Negative cache: a bad entry pointer would otherwise fault (and ride
    // Wine's fragile WOW64 SEH dispatch) on every single frame - fault once,
    // remember, and fall back instantly afterwards.
    static std::vector<const void *> badEntries;
    for (const void *bad : badEntries) {
        if (bad == entry.vertices) return false;
    }
    uint32_t vertexCount = 0;
    if (!copyEntryGuarded(entry, indexCount, &vertexCount, vertices, 256, indices,
                          uvScale, uS, vS, uO, vO)) {
        if (badEntries.size() < 4096) badEntries.push_back(entry.vertices);
        static bool loggedBadEntry = false;
        if (!loggedBadEntry) {
            loggedBadEntry = true;
            patch::log::dbg("mesh gpu path: unreadable entry (vertices=%p indices=%p), "
                            "falling back to CPU emission",
                            entry.vertices, entry.triangleIndices);
        }
        return false;
    }
    const uint32_t alphaTerm = *reinterpret_cast<const uint32_t *>(0x00779380);
    const uint32_t tint = (alphaTerm & 0xFF000000u) | 0x00FFFFFFu;
    const uint32_t textureId = resolveBridgeTextureId(slotHandle);
    // ponytail: one-shot raw-value dump for the missing-mesh hunt (menu
    // columns went from black to invisible across colour fixes). Shows
    // whether vertex colours/ambient are biased and what alpha rides along.
    static DWORD lastProbeTick = 0;
    const DWORD nowTick = GetTickCount();
    if (nowTick - lastProbeTick > 3000) {
        lastProbeTick = nowTick;
        patch::log::dbg("mesh gpu probe: v0.color=(%f %f %f) ambient=(%f %f %f) "
                        "alphaTerm=%08X texId=%u verts=%u packed0=%08X uv0=(%f %f) "
                        "uvTables=(%f %f %f %f) n0=(%f %f %f)",
                        static_cast<double>(entry.vertices[0].color.x),
                        static_cast<double>(entry.vertices[0].color.y),
                        static_cast<double>(entry.vertices[0].color.z),
                        static_cast<double>(ambient.x),
                        static_cast<double>(ambient.y),
                        static_cast<double>(ambient.z),
                        alphaTerm, textureId, vertexCount, vertices[0].base_color,
                        static_cast<double>(vertices[0].u), static_cast<double>(vertices[0].v),
                        static_cast<double>(*reinterpret_cast<const float *>(0x00779368)),
                        static_cast<double>(*reinterpret_cast<const float *>(0x0076F340)),
                        static_cast<double>(*reinterpret_cast<const float *>(0x0077F480)),
                        static_cast<double>(*reinterpret_cast<const float *>(0x0077F3D8)),
                        static_cast<double>(entry.vertices[0].normal.x),
                        static_cast<double>(entry.vertices[0].normal.y),
                        static_cast<double>(entry.vertices[0].normal.z));
    }
    emitMeshCamera();
    emitFrameLights(lightData);
    // Colours are plain 0..255 floats everywhere (probe confirmed zeros for
    // both vertex colour and ambient); the "bias" constants in the engine's
    // encoding helpers are themselves negative magic numbers, so nothing here
    // needs debiasing.
    gog::metal_bridge::drawMeshInline(
        textureId, vertices, vertexCount, indices, indexCount, tint,
        DK2M_DRAW_MESH_LIT,
        ambient.x / 255.0f,
        ambient.y / 255.0f,
        ambient.z / 255.0f);
    return true;
}

void processVertex(
        uint32_t index,
        MeshVertex *vertex,
        const dk2::Vec3f &ambient,
        dk2::Obj57BCB0 &lights,
        RenderFun renderFun) {
    if (!(dk2::g_idxFlags[index] & 2)) {
        return;
    }

    dk2::Vec3f color{
            ambient.x + vertex->color.x,
            ambient.y + vertex->color.y,
            ambient.z + vertex->color.z};
    lights.sub_57BF00(&color.x, &vertex->position.x, &vertex->normal.x);

    const float uvScale = *reinterpret_cast<const float *>(0x0066FB58);
    const dk2::Uv2f uv{
            static_cast<float>(vertex->packedUv & 0xFFFF) * uvScale,
            static_cast<float>(vertex->packedUv >> 16) * uvScale};
    dk2::Vec3f vectors[2]{color, color};
    dk2::Uv2f uvs[2]{uv, uv};
    emitVertex(renderFun, index, vectors, uvs);
}

}


namespace dk2 {

int __fastcall sub_57BBF0(
        int32_t *opaqueCollection, void *,
        float x, float y, float z, float radius, int mask) {
    const int32_t first = opaqueCollection[0];
    int32_t begin = 0;
    int32_t end = first + opaqueCollection[1];
    uint32_t resultBit = 1;
    if ((mask & 1) != 0) {
        begin = first;
        resultBit <<= static_cast<uint32_t>(first) & 31;
    }
    if ((mask & 0x20) != 0) {
        end = first;
    }
    if (begin >= end) {
        return 0;
    }

    const auto spheres = reinterpret_cast<const SpatialSphere *const *>(
            reinterpret_cast<const uint8_t *>(opaqueCollection) + 0x38);
    const __m128 px = _mm_set1_ps(x);
    const __m128 py = _mm_set1_ps(y);
    const __m128 pz = _mm_set1_ps(z);
    const __m128 queryRadius = _mm_set1_ps(radius);
    uint32_t result = 0;

    const int32_t candidateCount = end - begin;
    if (candidateCount <= 3) {
        uint32_t bit = resultBit;
        const uint32_t queryMask = static_cast<uint32_t>(mask);
        for (int32_t i = begin; i < end; ++i, bit <<= 1) {
            const SpatialSphere *item = spheres[i];
            if ((item->flags & queryMask) != queryMask) continue;
            const __m128 dx = _mm_sub_ss(_mm_set_ss(x), _mm_set_ss(item->center.x));
            const __m128 dy = _mm_sub_ss(_mm_set_ss(y), _mm_set_ss(item->center.y));
            const __m128 dz = _mm_sub_ss(_mm_set_ss(z), _mm_set_ss(item->center.z));
            const __m128 distanceSquared = _mm_add_ss(
                    _mm_add_ss(_mm_mul_ss(dx, dx), _mm_mul_ss(dy, dy)),
                    _mm_mul_ss(dz, dz));
            const __m128 radiusSum = _mm_add_ss(
                    _mm_set_ss(radius), _mm_set_ss(item->radius));
            if ((_mm_movemask_ps(_mm_sub_ss(
                    distanceSquared, _mm_mul_ss(radiusSum, radiusSum))) & 1) != 0) {
                result |= bit;
            }
        }
        return static_cast<int>(result);
    }

    for (int32_t i = begin; i < end; i += 4) {
        const int32_t remaining = end - i;
        const int32_t laneCount = remaining < 4 ? remaining : 4;
        const SpatialSphere *items[4];
        for (int lane = 0; lane < 4; ++lane) {
            items[lane] = spheres[i + (lane < laneCount ? lane : 0)];
        }

        uint32_t eligible = 0;
        const uint32_t queryMask = static_cast<uint32_t>(mask);
        for (int lane = 0; lane < laneCount; ++lane) {
            if ((items[lane]->flags & queryMask) == queryMask) {
                eligible |= 1u << lane;
            }
        }
        if (eligible != 0) {
            const __m128 dx = _mm_sub_ps(px, _mm_set_ps(
                    items[3]->center.x, items[2]->center.x,
                    items[1]->center.x, items[0]->center.x));
            const __m128 dy = _mm_sub_ps(py, _mm_set_ps(
                    items[3]->center.y, items[2]->center.y,
                    items[1]->center.y, items[0]->center.y));
            const __m128 dz = _mm_sub_ps(pz, _mm_set_ps(
                    items[3]->center.z, items[2]->center.z,
                    items[1]->center.z, items[0]->center.z));
            const __m128 distanceSquared = _mm_add_ps(
                    _mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)),
                    _mm_mul_ps(dz, dz));
            const __m128 radiusSum = _mm_add_ps(queryRadius, _mm_set_ps(
                    items[3]->radius, items[2]->radius,
                    items[1]->radius, items[0]->radius));
            const uint32_t overlaps = static_cast<uint32_t>(_mm_movemask_ps(
                    _mm_sub_ps(distanceSquared, _mm_mul_ps(radiusSum, radiusSum))))
                    & eligible;
            result |= overlaps * resultBit;
        }
        resultBit <<= 4;
    }
    return static_cast<int>(result);
}

}  // namespace dk2


int *dk2::Obj57AD20::sub_57A9A0(
        int entryIndex,
        SceneObject2E *scene,
        uint32_t *lights,
        int a4,
        int selectExtendedPath,
        int a6,
        int a7,
        float scale) {
    if (selectExtendedPath) {
        using OriginalExtendedFun = int *(__thiscall *)(
                Obj57AD20 *, int, SceneObject2E *, int, uint32_t *,
                int, int, int, float);
        return reinterpret_cast<OriginalExtendedFun>(0x0057B0E0)(
                this, entryIndex, scene, a4, lights,
                selectExtendedPath, a6, a7, scale);
    }
    return sub_57B6D0(entryIndex, scene, a4, lights, a6, a7, scale);
}


// Staged until the extended spatial-mesh path has a frame-level differential
// test.  sub_57A9A0 deliberately dispatches to the original entry for now.
int *dk2::Obj57AD20::sub_57B0E0(
        int entryIndex,
        SceneObject2E *scene,
        int a3,
        uint32_t *lightData,
        int vectorField,
        int fieldOriginX,
        int fieldOriginY,
        float scale) {
    auto &entry = reinterpret_cast<MeshEntry *>(f4)[entryIndex];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(entry.surfaceIndex);
    __renderFun_setSceneObject2E(scene, 1, nullptr, nullptr, scale, a3 == 0);

    Vec3f ambient{
            vec_14.x + g_vec_760A98.x + surface->vec.x,
            vec_14.y + g_vec_760A98.y + surface->vec.y,
            vec_14.z + g_vec_760A98.z + surface->vec.z};
    if (*reinterpret_cast<const int *>(0x00760B8C) != 0) {
        ambient = {255.0f, 0.0f, 0.0f};
    }

    Obj58EF60 sampler{
            vectorField,
            static_cast<float>(fieldOriginX - 1),
            static_cast<float>(fieldOriginY - 1)};
    Obj57BCB0 lights;
    lights.count = 0;
    lights.constructor(lightData, f2C);

    const VertexFun vertexFun = g_fun_779398;
    const TriangleFun triangleFun = __addTriangleFun;
    const RenderFun renderFun = __renderFun;
    const uint8_t *indices = entry.triangleIndices;
    for (uint32_t triangle = 0; triangle < entry.triangleCount; ++triangle, indices += 3) {
        const uint32_t a = indices[0];
        const uint32_t b = indices[1];
        const uint32_t c = indices[2];
        MeshVertex *va = &entry.vertices[a];
        MeshVertex *vb = &entry.vertices[b];
        MeshVertex *vc = &entry.vertices[c];

        if (g_idxFlags[a] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    va->position.x, va->position.y, va->position.z, &sampled.x);
            transformPosition(vertexFun, a, &sampled);
        }
        if (g_idxFlags[b] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    vb->position.x, vb->position.y, vb->position.z, &sampled.x);
            transformPosition(vertexFun, b, &sampled);
        }
        if (g_idxFlags[c] == 0) {
            Vec3f sampled;
            sampler.sub_58F030(
                    vc->position.x, vc->position.y, vc->position.z, &sampled.x);
            transformPosition(vertexFun, c, &sampled);
        }
        emitTriangle(triangleFun, a, b, c);
        processVertex(a, va, ambient, lights, renderFun);
        processVertex(b, vb, ambient, lights, renderFun);
        processVertex(c, vc, ambient, lights, renderFun);
    }
    return applyIndxs_sub_58AC20();
}


int *dk2::Obj57AD20::sub_57B6D0(
        int entryIndex,
        SceneObject2E *scene,
        int a3,
        uint32_t *lightData,
        int,
        int,
        float scale) {
    auto &entry = reinterpret_cast<MeshEntry *>(f4)[entryIndex];
    MyScaledSurface *surface = MyEntryBuf_MyScaledSurface_getByIdx(entry.surfaceIndex);
    __renderFun_setSceneObject2E(scene, 1, nullptr, nullptr, scale, a3 == 0);

    const Vec3f ambient{
            vec_14.x + g_vec_760A98.x + surface->vec.x,
            vec_14.y + g_vec_760A98.y + surface->vec.y,
            vec_14.z + g_vec_760A98.z + surface->vec.z};

    if (meshGpuActive() && drawEntryOnGpu(scene, entry, surface, ambient, lightData)) {
        return applyIndxs_sub_58AC20();
    }

    Obj57BCB0 lights;
    lights.count = 0;
    lights.constructor(lightData, f2C);

    const VertexFun vertexFun = g_fun_779398;
    const TriangleFun triangleFun = __addTriangleFun;
    const RenderFun renderFun = __renderFun;
    const uint8_t *indices = entry.triangleIndices;
    for (uint32_t triangle = 0; triangle < entry.triangleCount; ++triangle, indices += 3) {
        const uint32_t a = indices[0];
        const uint32_t b = indices[1];
        const uint32_t c = indices[2];
        MeshVertex *va = &entry.vertices[a];
        MeshVertex *vb = &entry.vertices[b];
        MeshVertex *vc = &entry.vertices[c];

        if (g_idxFlags[a] == 0) transformVertex(vertexFun, a, va);
        if (g_idxFlags[b] == 0) transformVertex(vertexFun, b, vb);
        if (g_idxFlags[c] == 0) transformVertex(vertexFun, c, vc);
        emitTriangle(triangleFun, a, b, c);
        processVertex(a, va, ambient, lights, renderFun);
        processVertex(b, vb, ambient, lights, renderFun);
        processVertex(c, vc, ambient, lights, renderFun);
    }
    return applyIndxs_sub_58AC20();
}
