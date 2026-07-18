#include "dk2/Idx3b.h"
#include "dk2/RenderData.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f_arr1024.h"
#include "dk2/Vec3f_arr1024.h"
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

// unnamed literal-pool floats at 0066FE28+idx*4, addressed via float_data2
const float *litPool() { return reinterpret_cast<const float *>(&dk2::float_data2); }

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


// 0058AF70: project a view-space vertex into RenderData_instance_arr[idx].
// Frustum outcode from single-op sums/differences; optional fog term when
// bit 3 of *(int *)0077F450 is set; perspective projection with two depth
// formulas selected by the sign of (0066FE3C - z).
int __cdecl dk2::sub_58AF70(int idx, float *v) {
    RenderData &r = RenderData_instance_arr[idx];
    g_idxFlags[idx] = 1;
    const float x = v[0], y = v[1], z = v[2];
    r.f0_vec.x = x;
    r.f0_vec.y = y;
    r.f0_vec.z = z;
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
                r.field_24 = 0;
            } else if (signBit(fog - fd2[7])) {  // fog < 0066FE44
                g_idxFlags[idx] |= 0x10;
                const float fv = fd2[21] - (fd2[20] - fog * fd2[24]);  // 66FE7C-(66FE78-fog*66FE88)
                r.field_24 = (int) (floatBits(fv) & 0x7fffff) - 0x400000;
            }
        }
        const float rz = fd2[7] / z;                                              // 0066FE44 / z
        const float scaleY = (&g_top_780938)[1] * rz;                             // 0078093C
        const float scaleX = (&g_vec_77F4C0.x)[3] * rz;                           // 0077F4CC
        const float sy = scaleY * y + reinterpret_cast<float *>(&g_renMode_77F928)[2];  // +0077F930
        const float sx = scaleX * x + (&g_right_77F4EC)[1];                       // +0077F4F0
        const float sel = fd2[5] - z;                                             // 0066FE3C - z
        r.fC_xC = sx;
        r.f10_y10 = sy;
        r.f14_z14 = signBit(sel) ? g_zAdd3_7793A0 - g_zMul3_77F934 * rz
                                 : g_zMul2_77F490 * z + g_zAdd2_77F4D0;
        if (signBit(fd2[3] - r.f14_z14)) r.f14_z14 = fd2[3];                      // clamp to 0066FE34
        r.field_18 = rz;
    }
    r.f1C__viewOffsets = (int) code;
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
    r.field_18 = 1.0f;
    r.fC_xC = x;
    r.f10_y10 = y;
    r.f14_z14 = g_zMul2_77F490 * z + g_zAdd2_77F4D0;
    r.f0_vec.x = x;
    r.f0_vec.y = y;
    r.f0_vec.z = z;
    r.f1C__viewOffsets = (int) (signBit(y - g_top_780938)
                              | signBit(g_bottom_77937C - y) << 1
                              | signBit(x - g_left_77F3F4) << 2
                              | signBit(g_right_77F4EC - x) << 3);
    return 0;  // eax is garbage in the original; no caller reads it
}


// 0058B2A0: per-vertex attribute copy (positions + uv sets) for the cached
// path (flags bit 2), otherwise defers to the original renderFun_sub_58B440.
uint8_t __cdecl dk2::renderFun_sub_58B2A0(int idx, Vec3f *vecs, Uv2f *uvs) {
    if (!(g_idxFlags[idx] & 4)) return (uint8_t) renderFun_sub_58B440(idx, vecs, uvs);
    SceneObject2E *obj = g_pSceneObject2E;
    const int nVec = obj->f1E_propsCount;
    for (int i = 0; i < nVec; ++i) g_vectors[i].f0_arr[idx] = vecs[i];
    const int nUv = obj->f1D_surfhCount;
    for (int i = 0; i < nUv; ++i) Uv2f_arr_instance[i].f0_arr[idx] = uvs[i];
    uint8_t flags = g_idxFlags[idx];
    if (!(flags & 8)) {
        flags &= 0xFD;
        g_idxFlags[idx] = flags;
    }
    return flags;
}


// 0058B940: triangle outcode test; fully-outside rejected, partially-outside
// queued into g_Idx3b_arr_instance for clipping, fully-inside emitted.
void __cdecl dk2::addTriangleToRender2(int a, int b, int c) {
    const int oa = RenderData_instance_arr[a].f1C__viewOffsets;
    const int ob = RenderData_instance_arr[b].f1C__viewOffsets;
    const int oc = RenderData_instance_arr[c].f1C__viewOffsets;
    if (ob & (oc & oa)) return;
    if ((oc | oa | ob) != 0) {
        const int n = g_Idx3b_arr_count;
        g_idxFlags[a] |= 6;
        g_idxFlags[b] |= 6;
        g_idxFlags[c] |= 6;
        g_Idx3b_arr_instance[n].f0_i = (uint8_t) a;
        g_Idx3b_arr_instance[n].f1_j = (uint8_t) b;
        g_Idx3b_arr_instance[n].f2_k = (uint8_t) c;
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
    if (rc.f1C__viewOffsets & (rb.f1C__viewOffsets & ra.f1C__viewOffsets)) return;
    const float cross = (rb.fC_xC - ra.fC_xC) * (rc.f10_y10 - ra.f10_y10)
                      - (rb.f10_y10 - ra.f10_y10) * (rc.fC_xC - ra.fC_xC);
    const float *fd2 = litPool();
    if (!(cross >= fd2[6])) return;  // fcomp 0066FE40; NaN rejects like C0=1
    if (ra.f20_vtxIdx < 0) {
        ra.f20_vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[a] |= 0xA;
    }
    if (rb.f20_vtxIdx < 0) {
        rb.f20_vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[b] |= 0xA;
    }
    if (rc.f20_vtxIdx < 0) {
        rc.f20_vtxIdx = DrawTriangleList_verticesCount++;
        g_idxFlags[c] |= 0xA;
    }
    if (g_pSceneObject2E->f10_drawFlags_x2[0] & 0x200) {
        const float key = (rb.f0_vec.z + ra.f0_vec.z + rc.f0_vec.z) * g_zMul_77F3E8
                        + g_zAdd_77F924 - fd2[20] - fd2[25];  // -66FE78 -66FE8C
        const int ki = (int) (floatBits(key) & 0x7fffff) - 0x400000;
        MyEntryBuf_Triangle24_add(ra.f20_vtxIdx, rb.f20_vtxIdx, rc.f20_vtxIdx, ki);
    } else {
        int16_t *w = reinterpret_cast<int16_t *>(g_lpwTrianglesIndices);
        w[0] = (int16_t) ra.f20_vtxIdx;
        w[1] = (int16_t) rb.f20_vtxIdx;
        w[2] = (int16_t) rc.f20_vtxIdx;
        g_lpwTrianglesIndices = reinterpret_cast<Idx3s *>(w + 3);
        DrawTriangleList_trianglesCount += 1;
    }
}
