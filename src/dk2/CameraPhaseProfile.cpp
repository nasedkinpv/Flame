#include "dk2/MyDLVec2i.h"
#include "dk2/utils/MyCameraState.h"
#include "dk2_functions.h"
#include "patches/logging.h"

#include <cstdint>
#include <windows.h>


namespace dk2 {
bool installCameraPhaseProfiler();
}


namespace {

enum CameraPhase : uint32_t {
    Generate,
    CameraState,
    ZArrays,
    Projection,
    BuildVisibleA,
    BuildVisibleB,
    CreateMeshes,
    FinishLists,
    EmitSceneCells,
    PhaseCount,
};

struct CameraPhaseProfile {
    uint64_t ticks[PhaseCount]{};
    uint32_t frames = 0;

    void add(CameraPhase phase, uint64_t elapsed) {
        ticks[phase] += elapsed;
    }

    void finishFrame() {
        if (++frames != 300) return;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const uint64_t divisor = static_cast<uint64_t>(frequency.QuadPart) * frames;
        auto averageUs = [&](CameraPhase phase) -> uint64_t {
            return divisor ? ticks[phase] * 1000000u / divisor : 0;
        };
        patch::log::dbg(
                "PERF camera avg us: generate=%llu state=%llu zarrays=%llu "
                "projection=%llu visibleA=%llu visibleB=%llu meshes=%llu "
                "finish=%llu sceneEmit=%llu",
                averageUs(Generate), averageUs(CameraState), averageUs(ZArrays),
                averageUs(Projection), averageUs(BuildVisibleA),
                averageUs(BuildVisibleB), averageUs(CreateMeshes),
                averageUs(FinishLists), averageUs(EmitSceneCells));
        *this = {};
    }
};

CameraPhaseProfile g_profile;

enum PrimitiveKind : uint32_t {
    DynamicMesh,
    AnimatedMesh,
    StaticMesh,
    StaticHeightField,
    PrimitiveKindCount,
};

struct PrimitiveProfile {
    uint64_t ticks[PrimitiveKindCount]{};
    uint32_t calls[PrimitiveKindCount]{};

    void add(PrimitiveKind kind, uint64_t elapsed) {
        ticks[kind] += elapsed;
        ++calls[kind];
    }

    void finish(uint32_t frames) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const uint64_t divisor = static_cast<uint64_t>(frequency.QuadPart) * frames;
        auto averageUs = [&](PrimitiveKind kind) -> uint64_t {
            return divisor ? ticks[kind] * 1000000u / divisor : 0;
        };
        patch::log::dbg(
                "PERF render-list avg us: dynamic=%llu anim=%llu static=%llu "
                "height=%llu; calls/frame x100: dynamic=%u anim=%u static=%u height=%u",
                averageUs(DynamicMesh), averageUs(AnimatedMesh),
                averageUs(StaticMesh), averageUs(StaticHeightField),
                calls[DynamicMesh] * 100u / frames,
                calls[AnimatedMesh] * 100u / frames,
                calls[StaticMesh] * 100u / frames,
                calls[StaticHeightField] * 100u / frames);
        *this = {};
    }
};

PrimitiveProfile g_primitiveProfile;

uint64_t profileTicks() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

template <uintptr_t Target, PrimitiveKind Kind>
int __fastcall profilePrimitive(void *self, void *, void *context) {
    using Function = int (__thiscall *)(void *, void *);
    // QPC costs ~1.3us through Wine-on-Rosetta; two per call across ~6000
    // objects burned ~16ms/frame. Sample the same 30-of-300 window draw3d uses.
    if (g_profile.frames >= 30)
        return reinterpret_cast<Function>(Target)(self, context);
    const uint64_t started = profileTicks();
    const int result = reinterpret_cast<Function>(Target)(self, context);
    g_primitiveProfile.add(Kind, profileTicks() - started);
    return result;
}

template <typename Result, typename Function, typename... Args>
Result measure(CameraPhase phase, Function function, Args... args) {
    const uint64_t started = profileTicks();
    Result result = function(args...);
    g_profile.add(phase, profileTicks() - started);
    return result;
}

template <typename Function, typename... Args>
void measureVoid(CameraPhase phase, Function function, Args... args) {
    const uint64_t started = profileTicks();
    function(args...);
    g_profile.add(phase, profileTicks() - started);
}

dk2::MyDLVec2i *__cdecl profileGenerate() {
    using Function = dk2::MyDLVec2i *(__cdecl *)();
    return measure<dk2::MyDLVec2i *>(
            Generate, reinterpret_cast<Function>(0x00574820));
}

int __cdecl profileCameraState(dk2::MyCameraState *state) {
    using Function = int (__cdecl *)(dk2::MyCameraState *);
    return measure<int>(
            CameraState, reinterpret_cast<Function>(0x0058A6F0), state);
}

void __cdecl profileZArrays(
        int mode, float left, float right, float top, float bottom, int flags) {
    using Function = void (__cdecl *)(int, float, float, float, float, int);
    measureVoid(
            ZArrays, reinterpret_cast<Function>(0x0058A570),
            mode, left, right, top, bottom, flags);
}

int __cdecl profileProjection(float left, float right, float top, float bottom) {
    using Function = int (__cdecl *)(float, float, float, float);
    return measure<int>(
            Projection, reinterpret_cast<Function>(0x0058A4F0),
            left, right, top, bottom);
}

uint32_t *__cdecl profileBuildVisibleA(dk2::MyDLVec2i *list) {
    using Function = uint32_t *(__cdecl *)(dk2::MyDLVec2i *);
    return measure<uint32_t *>(
            BuildVisibleA, reinterpret_cast<Function>(0x005737E0), list);
}

void __cdecl profileBuildVisibleB(dk2::MyDLVec2i *list) {
    using Function = void (__cdecl *)(dk2::MyDLVec2i *);
    measureVoid(
            BuildVisibleB, reinterpret_cast<Function>(0x005735A0), list);
}

void __cdecl profileCreateMeshes() {
    using Function = void (__cdecl *)();
    measureVoid(CreateMeshes, reinterpret_cast<Function>(0x005725D0));
}

int __cdecl profileFinishLists() {
    using Function = int (__cdecl *)();
    return measure<int>(FinishLists, reinterpret_cast<Function>(0x00576230));
}

void __stdcall profileSceneEmission() {
    using Function = void (__stdcall *)();
    measureVoid(EmitSceneCells, reinterpret_cast<Function>(0x00572CF0));
    if (g_profile.frames == 29) g_primitiveProfile.finish(30);
    g_profile.finishFrame();
}

struct CallPatch {
    uintptr_t address;
    uintptr_t target;
    uintptr_t replacement;
    const char *name;
};

bool patchCall(const CallPatch &entry) {
    auto *call = reinterpret_cast<uint8_t *>(entry.address);
    if (call[0] != 0xE8) {
        patch::log::err("camera profile: %08X is not a relative call (%s)",
                        entry.address, entry.name);
        return false;
    }
    const int32_t oldDisplacement = *reinterpret_cast<const int32_t *>(call + 1);
    const uintptr_t currentTarget = entry.address + 5 + oldDisplacement;
    if (currentTarget != entry.target) {
        patch::log::err(
                "camera profile: unexpected target %08X at %08X (%s)",
                currentTarget, entry.address, entry.name);
        return false;
    }
    const int32_t displacement = static_cast<int32_t>(
            entry.replacement - (entry.address + 5));
    DWORD oldProtection = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
        patch::log::err("camera profile: VirtualProtect failed: %08X",
                        GetLastError());
        return false;
    }
    *reinterpret_cast<int32_t *>(call + 1) = displacement;
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    DWORD ignored = 0;
    VirtualProtect(call, 5, oldProtection, &ignored);
    return true;
}

struct PointerPatch {
    uintptr_t address;
    uintptr_t target;
    uintptr_t replacement;
    const char *name;
};

bool patchPointer(const PointerPatch &entry) {
    auto *pointer = reinterpret_cast<uintptr_t *>(entry.address);
    if (*pointer != entry.target) {
        patch::log::err(
                "camera profile: unexpected pointer %08X at %08X (%s)",
                *pointer, entry.address, entry.name);
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(pointer, sizeof(*pointer), PAGE_READWRITE, &oldProtection)) {
        patch::log::err("camera profile: vtable VirtualProtect failed: %08X",
                        GetLastError());
        return false;
    }
    *pointer = entry.replacement;
    DWORD ignored = 0;
    VirtualProtect(pointer, sizeof(*pointer), oldProtection, &ignored);
    return true;
}

}


bool dk2::installCameraPhaseProfiler() {
    const CallPatch patches[]{
            {0x00575C0B, 0x00574820,
             reinterpret_cast<uintptr_t>(&profileGenerate), "generate"},
            {0x00575C17, 0x0058A6F0,
             reinterpret_cast<uintptr_t>(&profileCameraState), "state"},
            {0x00575C4C, 0x0058A570,
             reinterpret_cast<uintptr_t>(&profileZArrays), "zarrays"},
            {0x00575C75, 0x0058A4F0,
             reinterpret_cast<uintptr_t>(&profileProjection), "projection"},
            {0x00575C8E, 0x005737E0,
             reinterpret_cast<uintptr_t>(&profileBuildVisibleA), "visibleA"},
            {0x00575C95, 0x005735A0,
             reinterpret_cast<uintptr_t>(&profileBuildVisibleB), "visibleB"},
            {0x00575CCC, 0x005725D0,
             reinterpret_cast<uintptr_t>(&profileCreateMeshes), "meshes"},
            {0x00575D05, 0x00576230,
             reinterpret_cast<uintptr_t>(&profileFinishLists), "finish"},
            {0x00575D0A, 0x00572CF0,
             reinterpret_cast<uintptr_t>(&profileSceneEmission), "scene emission"},
    };
    for (const CallPatch &patch : patches) {
        if (!patchCall(patch)) return false;
    }
    const PointerPatch primitivePatches[]{
            {0x0066FD24, 0x00580EC0,
             reinterpret_cast<uintptr_t>(
                     &profilePrimitive<0x00580EC0, DynamicMesh>), "dynamic mesh"},
            {0x0066FD6C, 0x00584900,
             reinterpret_cast<uintptr_t>(
                     &profilePrimitive<0x00584900, AnimatedMesh>), "animated mesh"},
            {0x0066FD94, 0x00586190,
             reinterpret_cast<uintptr_t>(
                     &profilePrimitive<0x00586190, StaticMesh>), "static mesh"},
            {0x0066FDBC, 0x00587060,
             reinterpret_cast<uintptr_t>(
                     &profilePrimitive<0x00587060, StaticHeightField>),
             "static height field"},
    };
    for (const PointerPatch &patch : primitivePatches) {
        if (!patchPointer(patch)) return false;
    }
    patch::log::dbg("camera profile: installed %u phase probes",
                    static_cast<unsigned>(sizeof(patches) / sizeof(patches[0])));
    return true;
}
