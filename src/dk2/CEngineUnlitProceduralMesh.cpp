#include "dk2/engine/primitive/CEngineUnlitProceduralMesh.h"

#include "dk2/Idx3s.h"
#include "dk2/SceneObject2E.h"
#include "dk2/Uv2f.h"
#include "dk2/utils/Vec3f.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"

#include <algorithm>
#include <cstdint>

// dk2::CEngineUnlitProceduralMesh::fun_5884F0 -- per-frame emission of a
// batch (<=256 triangles) of procedurally generated unlit billboard/particle
// triangles (smoke, effect sprites, ...).
//
// Translated from 0x005884F0..0x005888A0.


namespace {

using VertexFun = int (__cdecl *)(uint32_t, dk2::Vec3f *);
using TriangleFun = int (__cdecl *)(uint32_t, uint32_t, uint32_t);
using RenderFun = int (__cdecl *)(uint32_t, dk2::Vec3f *, dk2::Uv2f *);

// Each procedurally generated corner references a row in the mesh's
// "bufPos" table (see below) via a 16-bit row index; the 8-byte record
// packs the three corner references for one generated triangle.
// TODO(verify): field names are invented -- only the byte layout (three
// consecutive uint16_t + 2 bytes padding) is confirmed from the disassembly.
#pragma pack(push, 1)
struct CornerRefs {
    uint16_t a;
    uint16_t b;
    uint16_t c;
    uint16_t reserved6;
};
#pragma pack(pop)
static_assert(sizeof(CornerRefs) == 8);

// One generated-vertex descriptor: uv, unlit colour (pre color-scale
// constant multiply), an unidentified float (never read here), and the
// row's position index into compBuf.
// TODO(verify): offset 0x14 float and the two bytes past geomIndex are
// never touched by this function; names/purpose unconfirmed.
#pragma pack(push, 1)
struct BufPosEntry {
    float u;
    float v;
    float r;
    float g;
    float b;
    float reserved14;
    uint16_t geomIndex;
    uint16_t reserved1A;
};
#pragma pack(pop)
static_assert(sizeof(BufPosEntry) == 0x1C);

// TODO(verify): unnamed global function pointer at 0x0077F92C, same shape
// as dk2::g_fun_779398 (see dk2_globals.h) but not present in the generated
// header -- no other translated function references this address yet.
VertexFun altVertexFun() {
    return *reinterpret_cast<VertexFun *>(0x0077F92C);
}

void transformVertex(VertexFun fun, uint32_t index, dk2::Vec3f *position) {
    if (fun == reinterpret_cast<VertexFun>(0x0058ACB0)) {
        dk2::sub_58ACB0(index, position);
    } else if (fun == reinterpret_cast<VertexFun>(0x0058AD10)) {
        dk2::sub_58AD10(index, position);
    } else {
        fun(index, position);
    }
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

void emitVertex(RenderFun fun, uint32_t index, dk2::Vec3f *colour, dk2::Uv2f *uv) {
    if (fun == reinterpret_cast<RenderFun>(0x0058B2A0)) {
        dk2::renderFun_sub_58B2A0(index, colour, uv);
    } else {
        fun(index, colour, uv);
    }
}

// Transform the generated position for one triangle corner (only if it has
// not already been produced this frame -- g_idxFlags[index] == 0).
void transformCorner(
        VertexFun vertexFun,
        const BufPosEntry *bufPosTable,
        const dk2::Vec3f *compBufTable,
        uint32_t index,
        uint16_t cornerRef) {
    if (dk2::g_idxFlags[index] != 0) return;
    const BufPosEntry &row = bufPosTable[cornerRef];
    dk2::Vec3f position = compBufTable[row.geomIndex];
    transformVertex(vertexFun, index, &position);
}

// Emit colour+uv for one triangle corner (only if it was actually consumed
// this frame -- g_idxFlags[index] & 2).
void emitCorner(
        RenderFun renderFun,
        const BufPosEntry *bufPosTable,
        uint32_t index,
        uint16_t cornerRef,
        float colourScale) {
    if (!(dk2::g_idxFlags[index] & 2)) return;
    const BufPosEntry &row = bufPosTable[cornerRef];
    dk2::Vec3f colour{
            row.r * colourScale,
            row.g * colourScale,
            row.b * colourScale};
    dk2::Uv2f uv{row.u, row.v};
    emitVertex(renderFun, index, &colour, &uv);
}

}  // namespace


int *dk2::CEngineUnlitProceduralMesh::fun_5884F0(int mode, SceneObject2E *scene) {
    __renderFun_setSceneObject2E(scene, 1, nullptr, nullptr, 1.0f, 0);

    const int totalCount = static_cast<int>(field_8);
    const int begin = mode << 8;
    const int end = std::min(begin + 0x100, totalCount);

    // wip: bring-up instrumentation (tower smoke/light-effect investigation,
    // removed once confirmed) - this function is never gated on
    // meshGpuActive(), so if it IS the menu's smoke/glow effect, it always
    // runs the legacy CPU emission path regardless of mesh_gpu_path.
    {
        static uint32_t calls = 0;
        if (calls < 5) {
            patch::log::dbg(
                "CEngineUnlitProceduralMesh::fun_5884F0: mode=%d "
                "totalCount=%d begin=%d end=%d (calls=%u)",
                mode, totalCount, begin, end, calls);
        }
        ++calls;
    }

    if (begin < end) {
        const auto *triangles = reinterpret_cast<const Idx3s *>(field_18);
        const auto *cornerRefs = reinterpret_cast<const CornerRefs *>(field_24);
        const auto *bufPosTable = reinterpret_cast<const BufPosEntry *>(bufPos);
        const auto *compBufTable = reinterpret_cast<const Vec3f *>(compBuf);
        const float colourScale = *reinterpret_cast<const float *>(0x0066FC18);

        // field_54 & 0x1000 selects which of the two generated-vertex
        // transform tables to use for every corner of this batch.
        // TODO(verify): meaning of this flag bit is unconfirmed.
        const VertexFun vertexFun =
                (field_54 & 0x1000) ? altVertexFun() : g_fun_779398;
        const TriangleFun triangleFun = __addTriangleFun;
        const RenderFun renderFun = __renderFun;

        for (int i = begin; i < end; ++i) {
            const Idx3s &tri = triangles[i];
            const CornerRefs &refs = cornerRefs[i];
            const uint32_t a = static_cast<uint16_t>(tri.i);
            const uint32_t b = static_cast<uint16_t>(tri.j);
            const uint32_t c = static_cast<uint16_t>(tri.k);

            transformCorner(vertexFun, bufPosTable, compBufTable, a, refs.a);
            transformCorner(vertexFun, bufPosTable, compBufTable, b, refs.b);
            transformCorner(vertexFun, bufPosTable, compBufTable, c, refs.c);

            emitTriangle(triangleFun, a, b, c);

            emitCorner(renderFun, bufPosTable, a, refs.a, colourScale);
            emitCorner(renderFun, bufPosTable, b, refs.b, colourScale);
            emitCorner(renderFun, bufPosTable, c, refs.c, colourScale);
        }
    }

    return applyIndxs_sub_58AC20();
}
