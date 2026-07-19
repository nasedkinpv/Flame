#include "dk2/Idx3b.h"
#include "dk2/RenderData.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f_arr1024.h"
#include "dk2/Vec3f_arr1024.h"
#include "dk2/Vertex1C.h"
#include "dk2/VerticesData.h"
#include "dk2/utils/Mat3x3f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>


namespace {

// world transform: tmp = g_mat_77F3A8 * v + g_vec_77F4C0
dk2::Vec3f worldTransform(dk2::Vec3f *v) {
    dk2::Vec3f tmp;
    dk2::g_mat_77F3A8.sub_594E10(v, &tmp);
    const __m128 sum = _mm_add_ps(
            _mm_set_ps(0.0f, tmp.z, tmp.y, tmp.x),
            _mm_set_ps(0.0f, dk2::g_vec_77F4C0.z, dk2::g_vec_77F4C0.y, dk2::g_vec_77F4C0.x));
    _mm_storel_pi(reinterpret_cast<__m64 *>(&tmp), sum);
    _mm_store_ss(&tmp.z, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 2, 2, 2)));
    return tmp;
}

uint32_t floatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// the originals branch on the raw sign bit of a stored float (incl. -0.0),
// not on an ordered compare — replicate exactly
uint32_t signBit(float value) { return floatBits(value) >> 31; }

uint32_t colourComponent(float value, bool useFogScale, int fogScale) {
    const float firstBias = *reinterpret_cast<const float *>(0x0066FE78);
    const float secondBias = *reinterpret_cast<const float *>(0x0066FE8C);
    float encoded = value - firstBias;
    encoded = encoded - secondBias;
    uint32_t component = (floatBits(encoded) & 0x007FFFFF) - 0x00400000;
    if (useFogScale) {
        component = (component * static_cast<uint32_t>(fogScale)) >> 16;
    }
    return component > 0xFF ? 0xFF : component;
}

void writeVertex1C(
        int stream,
        int &textureStage,
        const dk2::RenderData &render,
        const dk2::Vec3f &colour,
        const dk2::Uv2f *uvs,
        bool useFogScale,
        uint8_t textureCount) {
    auto *vertex = reinterpret_cast<dk2::Vertex1C *>(
            reinterpret_cast<uint8_t *>(dk2::g_vertices[stream].verticies1C_pos)
            + dk2::g_flexibleVertex_size * render.vtxIdx);
    vertex->x = render.xC;
    vertex->y = render.y10;
    vertex->z = render.z14;
    vertex->rhv__colorWeight = render.f18;

    const uint32_t red = colourComponent(colour.x, useFogScale, render.f24);
    const uint32_t green = colourComponent(colour.y, useFogScale, render.f24);
    const uint32_t blue = colourComponent(colour.z, useFogScale, render.f24);
    const uint32_t alpha = *reinterpret_cast<const uint32_t *>(0x00779380);
    vertex->diffuse = static_cast<int>((((red << 8) + green) << 8) + alpha + blue);

    auto *outUv = reinterpret_cast<float *>(
            reinterpret_cast<uint8_t *>(vertex) + 0x14);
    const auto *uScale = reinterpret_cast<const float *>(0x00779368);
    const auto *uOffset = reinterpret_cast<const float *>(0x0077F480);
    const auto *vScale = reinterpret_cast<const float *>(0x0076F340);
    const auto *vOffset = reinterpret_cast<const float *>(0x0077F3D8);
    for (uint8_t texture = 0; texture < textureCount; ++texture, ++textureStage) {
        outUv[texture * 2] = uScale[textureStage] * uvs[textureStage].u
                           + uOffset[textureStage];
        outUv[texture * 2 + 1] = vScale[textureStage] * uvs[textureStage].v
                               + vOffset[textureStage];
    }
}

// unnamed literal-pool floats at 0066FE28+idx*4, addressed via float_data2
const float *litPool() { return reinterpret_cast<const float *>(&dk2::float_data2); }

uint32_t frustumOutcode(const dk2::Vec3f &v) {
    return signBit(v.z + v.y)
         | signBit(v.z - v.y) << 1
         | signBit(v.x + v.z) << 2
         | signBit(v.z - v.x) << 3;
}

int createClipVertex(int outside, int other, uint32_t plane) {
    const dk2::Vec3f &a = dk2::RenderData_instance_arr[outside].vec;
    const dk2::Vec3f &b = dk2::RenderData_instance_arr[other].vec;
    float t;
    switch (plane) {
    case 4: t = (a.x + a.z) / (((a.x - b.x) + a.z) - b.z); break;
    case 8: t = (a.z - a.x) / (((b.x - a.x) + a.z) - b.z); break;
    case 1: t = (a.y + a.z) / (((a.y - b.y) + a.z) - b.z); break;
    default: t = (a.z - a.y) / (((b.y - a.y) + a.z) - b.z); break;
    }

    const int idx = dk2::g_outIdx++;
    dk2::RenderData &dst = dk2::RenderData_instance_arr[idx];
    dst.vec.x = (b.x - a.x) * t + a.x;
    dst.vec.y = (b.y - a.y) * t + a.y;
    dst.vec.z = (b.z - a.z) * t + a.z;
    switch (plane) {
    case 4: dst.vec.z = -dst.vec.x; break;
    case 8: dst.vec.z = dst.vec.x; break;
    case 1: dst.vec.z = -dst.vec.y; break;
    default: dst.vec.z = dst.vec.y; break;
    }
    uint32_t remainingPlanes;
    switch (plane) {
    case 4: remainingPlanes = 1 | 2 | 8; break;
    case 8: remainingPlanes = 1 | 2; break;
    case 1: remainingPlanes = 2; break;
    default: remainingPlanes = 0; break;
    }
    dst._viewOffsets = (int) (frustumOutcode(dst.vec) & remainingPlanes);

    dk2::SceneObject2E *obj = dk2::g_pSceneObject2E;
    for (int i = 0; i < obj->propsCount; ++i) {
        const dk2::Vec3f &va = dk2::g_vectors[i].arr[outside];
        const dk2::Vec3f &vb = dk2::g_vectors[i].arr[other];
        dk2::Vec3f &vd = dk2::g_vectors[i].arr[idx];
        vd.x = (vb.x - va.x) * t + va.x;
        vd.y = (vb.y - va.y) * t + va.y;
        vd.z = (vb.z - va.z) * t + va.z;
    }
    for (int i = 0; i < obj->surfhCount; ++i) {
        const dk2::Uv2f &ua = dk2::Uv2f_arr_instance[i].arr[outside];
        const dk2::Uv2f &ub = dk2::Uv2f_arr_instance[i].arr[other];
        dk2::Uv2f &ud = dk2::Uv2f_arr_instance[i].arr[idx];
        ud.u = (ub.u - ua.u) * t + ua.u;
        ud.v = (ub.v - ua.v) * t + ua.v;
    }
    return idx;
}

}


int *dk2::applyIndxs_sub_58AC20() {
    const bool perspectiveClip = reinterpret_cast<int *>(&g_bottom_77937C)[-1] != 0;
    for (int i = 0; i < g_Idx3b_arr_count; ++i) {
        if (perspectiveClip) {
            adjustAndAddToRender_sub_58BB60(&g_Idx3b_arr_instance[i]);
        } else {
            adjustAndAddToRender_sub_58CC40(&g_Idx3b_arr_instance[i]);
        }
    }

    const int vertexCount = g_outIdx;
    std::memset(g_idxFlags, 0, vertexCount);
    for (int i = 0; i < vertexCount; ++i) RenderData_instance_arr[i].vtxIdx = -1;
    g_Idx3b_arr_count = 0;
    return vertexCount > 0 ? &RenderData_instance_arr[vertexCount].vtxIdx : nullptr;
}


int __cdecl dk2::sub_58ACB0(int a1, Vec3f *v) {
    Vec3f tmp = worldTransform(v);
    return sub_58AF70(a1, &tmp.x);
}


int __cdecl dk2::sub_58AD10(int a1, Vec3f *v) {
    Vec3f tmp = worldTransform(v);
    return RenderData_addToArr(a1, &tmp);
}


int __cdecl dk2::sub_58AD70(int a1, float *v) {
    // original scales by two unnamed floats right after g_mat_77F498 and before g_zAdd3_7793A0
    const float sx = (&g_vec_77F4C0.x)[-1];                          // 0077F4BC
    const float sy = (&g_zAdd3_7793A0)[-1];  // 0077939C
    Vec3f tmp{v[0] * sx, v[1] * sy, v[2]};
    return sub_58AF70(a1, &tmp.x);
}


// 0058ADC0: project an already view-space vertex. Unlike sub_58AF70 this is
// used for vertices created by the clipper, so the outcode is always cleared.
int __cdecl dk2::RenderData_addToArr(int idx, Vec3f *v) {
    const float *fd2 = litPool();
    RenderData &r = RenderData_instance_arr[idx];
    g_idxFlags[idx] = 1;
    r.vec = *v;

    const float rz = fd2[7] / v->z;                                  // 0066FE44 / z
    r.y10 = (&g_top_780938)[1] * rz * v->y
          + reinterpret_cast<float *>(&g_renMode_77F928)[2];
    r.xC = (&g_vec_77F4C0.x)[3] * rz * v->x + (&g_right_77F4EC)[1];

    if (reinterpret_cast<int *>(&g_vecCol2)[0x50 / 4] & 8) {         // 0077F450
        const double k80 = *reinterpret_cast<const double *>(fd2 + 22);
        const float e8 = reinterpret_cast<const float *>(&__addTriangleFun)[1];
        const float fog =
                (e8 - (v->z - (float) ((fabsf(v->x) + fabsf(v->y)) * k80)))
                * g_zAdd3Arr_77F4D8[2];
        if (signBit(fog)) {
            g_idxFlags[idx] |= 0x10;
            r.f24 = 0;
        } else if (signBit(fog - fd2[7])) {
            g_idxFlags[idx] |= 0x10;
            const float fv = fd2[21] - (fd2[20] - fog * fd2[24]);
            r.f24 = (int) (floatBits(fv) & 0x7fffff) - 0x400000;
        }
    }

    const float sel = fd2[5] - v->z;
    r.z14 = signBit(sel) ? g_zAdd3_7793A0 - g_zMul3_77F934 * rz
                         : g_zMul2_77F490 * v->z + g_zAdd2_77F4D0;
    if (signBit(fd2[3] - r.z14)) r.z14 = fd2[3];
    r.f18 = rz;
    r._viewOffsets = 0;
    return 0;
}


// 0058AF70: project a view-space vertex into RenderData_instance_arr[idx].
// Frustum outcode from single-op sums/differences; optional fog term when
// bit 3 of *(int *)0077F450 is set; perspective projection with two depth
// formulas selected by the sign of (0066FE3C - z).
int __cdecl dk2::sub_58AF70(int idx, float *v) {
    RenderData &r = RenderData_instance_arr[idx];
    g_idxFlags[idx] = 1;
    const float x = v[0], y = v[1], z = v[2];
    r.vec.x = x;
    r.vec.y = y;
    r.vec.z = z;
    uint32_t code = signBit(z + y)
                  | signBit(z - y) << 1
                  | signBit(x + z) << 2
                  | signBit(z - x) << 3;
    if (code == 0) {
        const float *fd2 = litPool();
        if (reinterpret_cast<int *>(&g_vecCol2)[0x50 / 4] & 8) {  // 0077F450
            const double k80 = *reinterpret_cast<const double *>(fd2 + 22);       // 0066FE80
            const float e8 = reinterpret_cast<const float *>(&__addTriangleFun)[1];  // 0077F4E8
            const float fog =
                    (e8 - (z - (float) ((fabsf(x) + fabsf(y)) * k80))) * g_zAdd3Arr_77F4D8[2];  // *0077F4E0
            if (signBit(fog)) {
                code |= 0x20;
                g_idxFlags[idx] |= 0x10;
                r.f24 = 0;
            } else if (signBit(fog - fd2[7])) {  // fog < 0066FE44
                g_idxFlags[idx] |= 0x10;
                const float fv = fd2[21] - (fd2[20] - fog * fd2[24]);  // 66FE7C-(66FE78-fog*66FE88)
                r.f24 = (int) (floatBits(fv) & 0x7fffff) - 0x400000;
            }
        }
        const float rz = fd2[7] / z;                                              // 0066FE44 / z
        const float scaleY = (&g_top_780938)[1] * rz;                             // 0078093C
        const float scaleX = (&g_vec_77F4C0.x)[3] * rz;                           // 0077F4CC
        const float sy = scaleY * y + reinterpret_cast<float *>(&g_renMode_77F928)[2];  // +0077F930
        const float sx = scaleX * x + (&g_right_77F4EC)[1];                       // +0077F4F0
        const float sel = fd2[5] - z;                                             // 0066FE3C - z
        r.xC = sx;
        r.y10 = sy;
        r.z14 = signBit(sel) ? g_zAdd3_7793A0 - g_zMul3_77F934 * rz
                             : g_zMul2_77F490 * z + g_zAdd2_77F4D0;
        if (signBit(fd2[3] - r.z14)) r.z14 = fd2[3];                              // clamp to 0066FE34
        r.f18 = rz;
    }
    r._viewOffsets = (int) code;
    return 0;  // eax is garbage in the original; no caller reads it
}


// 0058B190: pre-transformed (screen-space) vertex path, target of g_fun_779394.
int __cdecl dk2::sub_58B190(int idx, Vec3f *v) {
    const float *fd2 = litPool();
    const float x = v->x, y = v->y;
    float z = v->z;
    if (signBit(z - fd2[4])) z = fd2[4];  // clamp to 0066FE38
    if (signBit(fd2[7] - z)) z = fd2[7];  // clamp to 0066FE44
    RenderData &r = RenderData_instance_arr[idx];
    r.f18 = 1.0f;
    r.xC = x;
    r.y10 = y;
    r.z14 = g_zMul2_77F490 * z + g_zAdd2_77F4D0;
    r.vec.x = x;
    r.vec.y = y;
    r.vec.z = z;
    r._viewOffsets = (int) (signBit(y - g_top_780938)
                          | signBit(g_bottom_77937C - y) << 1
                          | signBit(x - g_left_77F3F4) << 2
                          | signBit(g_right_77F4EC - x) << 3);
    return 0;  // eax is garbage in the original; no caller reads it
}


// 0058B2A0: per-vertex attribute copy (positions + uv sets) for the cached
// path (flags bit 2).  The original still defers to renderFun_sub_58B440
// when bit 3 is set, after refreshing these cached arrays.
uint8_t __cdecl dk2::renderFun_sub_58B2A0(int idx, Vec3f *vecs, Uv2f *uvs) {
    if (!(g_idxFlags[idx] & 4)) return (uint8_t) renderFun_sub_58B440(idx, vecs, uvs);
    SceneObject2E *obj = g_pSceneObject2E;
    const int nVec = obj->propsCount;
    for (int i = 0; i < nVec; ++i) g_vectors[i].arr[idx] = vecs[i];
    const int nUv = obj->surfhCount;
    for (int i = 0; i < nUv; ++i) Uv2f_arr_instance[i].arr[idx] = uvs[i];
    uint8_t flags = g_idxFlags[idx];
    if (flags & 8) return (uint8_t) renderFun_sub_58B440(idx, vecs, uvs);
    flags &= 0xFD;
    g_idxFlags[idx] = flags;
    return flags;
}


// 0058B370: cached attribute path for the alternative flexible-vertex format.
// It mirrors 58B2A0 but delegates cache misses to the original 58B680 path.
uint8_t __cdecl dk2::renderFun_sub_58B370(int idx, Vec3f *vecs, Uv2f *uvs) {
    if (!(g_idxFlags[idx] & 4)) return (uint8_t) renderFun_sub_58B680(idx, vecs, uvs);
    SceneObject2E *obj = g_pSceneObject2E;
    for (int i = 0; i < obj->propsCount; ++i) g_vectors[i].arr[idx] = vecs[i];
    for (int i = 0; i < obj->surfhCount; ++i) Uv2f_arr_instance[i].arr[idx] = uvs[i];
    uint8_t flags = g_idxFlags[idx];
    if (flags & 8) return (uint8_t) renderFun_sub_58B680(idx, vecs, uvs);
    flags &= 0xFD;
    g_idxFlags[idx] = flags;
    return flags;
}


// 0058B440: emit D3DFVF_XYZRHW | D3DFVF_DIFFUSE vertices.  The original
// converts each colour component with two x87 mantissa-bias operations and
// transforms every UV with x87; MSVC's 32-bit SSE2 scalar operations preserve
// the same single-precision rounding without the Rosetta x87 cost.
char __cdecl dk2::renderFun_sub_58B440(int idx, Vec3f *vecs, Uv2f *uvs) {
    const uint8_t oldFlags = g_idxFlags[idx];
    g_idxFlags[idx] = oldFlags & 0xFD;
    SceneObject2E *obj = g_pSceneObject2E;
    const uint8_t streamCount = obj->propsCount;
    const bool useFogScale = (oldFlags & 0x10) != 0;
    const RenderData &render = RenderData_instance_arr[idx];
    int textureStage = 0;
    for (uint8_t stream = 0; stream < streamCount; ++stream) {
        const uint8_t configuredTextures = static_cast<uint8_t>(
                obj->numTextureSamplers_x2[stream]);
        // The original always writes stage zero, then uses this field only to
        // decide whether it must write additional stages.
        const uint8_t textureCount = configuredTextures ? configuredTextures : 1;
        writeVertex1C(
                stream,
                textureStage,
                render,
                vecs[stream],
                uvs,
                useFogScale,
                textureCount);
    }
    return static_cast<char>(streamCount);
}


// 0058B940: triangle outcode test; fully-outside rejected, partially-outside
// queued into g_Idx3b_arr_instance for clipping, fully-inside emitted.
void __cdecl dk2::addTriangleToRender2(int a, int b, int c) {
    const int oa = RenderData_instance_arr[a]._viewOffsets;
    const int ob = RenderData_instance_arr[b]._viewOffsets;
    const int oc = RenderData_instance_arr[c]._viewOffsets;
    if (ob & (oc & oa)) return;
    if ((oc | oa | ob) != 0) {
        const int n = g_Idx3b_arr_count;
        g_idxFlags[a] |= 6;
        g_idxFlags[b] |= 6;
        g_idxFlags[c] |= 6;
        g_Idx3b_arr_instance[n].i = (uint8_t) a;
        g_Idx3b_arr_instance[n].j = (uint8_t) b;
        g_Idx3b_arr_instance[n].k = (uint8_t) c;
        g_Idx3b_arr_count = n + 1;
    } else {
        addTriangleToRender1(a, b, c);
    }
}


// 0058B9D0: screen-space backface cull, vertex slot allocation, then either
// depth-keyed insert (MyEntryBuf_Triangle24_add) or direct index emit.
void __cdecl dk2::addTriangleToRender1(int a, int b, int c) {
    RenderData &ra = RenderData_instance_arr[a];
    RenderData &rb = RenderData_instance_arr[b];
    RenderData &rc = RenderData_instance_arr[c];
    if (rc._viewOffsets & (rb._viewOffsets & ra._viewOffsets)) return;
    const float cross = (rb.xC - ra.xC) * (rc.y10 - ra.y10)
                      - (rb.y10 - ra.y10) * (rc.xC - ra.xC);
    const float *fd2 = litPool();
    if (!(cross >= fd2[6])) return;  // fcomp 0066FE40; NaN rejects like C0=1
    if (ra.vtxIdx < 0) {
        ra.vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[a] |= 0xA;
    }
    if (rb.vtxIdx < 0) {
        rb.vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[b] |= 0xA;
    }
    if (rc.vtxIdx < 0) {
        rc.vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[c] |= 0xA;
    }
    if (g_pSceneObject2E->drawFlags_x2[0] & 0x200) {
        const float key = (rb.vec.z + ra.vec.z + rc.vec.z) * g_zMul_77F3E8
                        + g_zAdd_77F924 - fd2[20] - fd2[25];  // -66FE78 -66FE8C
        const int ki = (int) (floatBits(key) & 0x7fffff) - 0x400000;
        MyEntryBuf_Triangle24_add(ra.vtxIdx, rb.vtxIdx, rc.vtxIdx, ki);
    } else {
        int16_t *w = reinterpret_cast<int16_t *>(g_lpwTrianglesIndices);
        w[0] = (int16_t) ra.vtxIdx;
        w[1] = (int16_t) rb.vtxIdx;
        w[2] = (int16_t) rc.vtxIdx;
        g_lpwTrianglesIndices = reinterpret_cast<Idx3s *>(w + 3);
        DrawTriangleList_trianglesCount += 1;
    }
}


// 0058BB60: clip a partially visible triangle against the four perspective
// planes, interpolate every active vertex/UV stream, then emit a triangle fan.
int __cdecl dk2::adjustAndAddToRender_sub_58BB60(Idx3b *triangle) {
    int polygon[32]{triangle->i, triangle->j, triangle->k};
    int scratch[32];
    int count = 3;
    const uint32_t planes[] = {4, 8, 1, 2};
    for (uint32_t plane : planes) {
        int outCount = 0;
        int previous = polygon[count - 1];
        bool previousInside =
                (RenderData_instance_arr[previous]._viewOffsets & plane) == 0;
        for (int i = 0; i < count; ++i) {
            const int current = polygon[i];
            const bool currentInside =
                    (RenderData_instance_arr[current]._viewOffsets & plane) == 0;
            if (currentInside) {
                if (!previousInside) scratch[outCount++] = createClipVertex(previous, current, plane);
                scratch[outCount++] = current;
            } else if (previousInside) {
                scratch[outCount++] = createClipVertex(current, previous, plane);
            }
            previous = current;
            previousInside = currentInside;
        }
        if (outCount < 3) return 0;
        count = outCount;
        std::memcpy(polygon, scratch, sizeof(int) * count);
    }

    for (int i = 0; i < count; ++i) {
        RenderData &r = RenderData_instance_arr[polygon[i]];
        RenderData_addToArr(polygon[i], &r.vec);
    }
    for (int i = 1; i + 1 < count; ++i) {
        addTriangleToRender1(polygon[0], polygon[i], polygon[i + 1]);
    }

    SceneObject2E *obj = g_pSceneObject2E;
    for (int i = 0; i < count; ++i) {
        const int idx = polygon[i];
        if (!(g_idxFlags[idx] & 2)) continue;
        Vec3f vectors[4];
        Uv2f uvs[4];
        for (int j = 0; j < obj->propsCount; ++j) vectors[j] = g_vectors[j].arr[idx];
        for (int j = 0; j < obj->surfhCount; ++j) uvs[j] = Uv2f_arr_instance[j].arr[idx];
        if (__renderFun == reinterpret_cast<decltype(__renderFun)>(0x0058B2A0)) {
            renderFun_sub_58B2A0(idx, vectors, uvs);
        } else {
            __renderFun(idx, vectors, uvs);
        }
    }
    return 0;
}


// 0059F2F0: corner slide helper - applies (+-fa, fb) to x/z and/or y/z
// depending on four neighbour flags; fa is scaled by *(float *)00670700 when
// both blocks run, and the second block sees fb = 0 if the first block ran.
int __cdecl dk2::sub_59F2F0(float *v, float fa, float fb, int d4, int d5, int d6, int d7) {
    if ((d7 | d5) != (d6 | d4)) {
        if ((d7 | d6) != (d5 | d4)) fa *= *reinterpret_cast<const float *>(0x00670700);
        if (d6 | d4) {
            v[2] += fb;
            v[0] += -fa;
        } else {
            v[0] += fa;
            v[2] += fb;
        }
        fb = 0.0f;
    }
    if ((d7 | d6) != (d5 | d4)) {
        if (d5 | d4) {
            v[2] += fb;
            v[1] += -fa;
        } else {
            v[1] += fa;
            v[2] += fb;
        }
    }
    return 0;  // eax is the vec pointer in the original; no caller reads it
}


// 0059C2A0: convert two Vec3i map positions (plus the CRenderInfo int16
// offsets at +0x51/+0x53/+0x55) into scaled Vec3f world positions.
// x/y scale at 006704D0, z scale at 00670504; values stay far below 2^24,
// so double intermediates reproduce the x87 fild+fmul single rounding.
dk2::Vec3f *__cdecl dk2::static_CRenderInfo_sub_59C2A0(
        CRenderInfo *info, Vec3i *a, Vec3i *b, Vec3f *outA, Vec3f *outB) {
    const auto *off = reinterpret_cast<const int16_t *>(
            reinterpret_cast<const char *>(info) + 0x51);
    const double kxy = *reinterpret_cast<const float *>(0x006704D0);
    const double kz = *reinterpret_cast<const float *>(0x00670504);
    const int ax = a->x + off[0], ay = a->y + off[1], az = a->z + off[2];
    const int bx = b->x + off[0], by = b->y + off[1], bz = b->z + off[2];
    outB->x = (float) (bx * kxy);
    outB->y = (float) (by * kxy);
    outB->z = (float) (bz * kz);
    outA->x = (float) (ax * kxy);
    outA->y = (float) (ay * kxy);
    outA->z = (float) (az * kz);
    return outA;
}
