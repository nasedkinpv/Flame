#include "patches/logging.h"

#include <cstdint>
#include <cstring>
#include <windows.h>


namespace dk2 {
bool installFtolSse3();
}

// The CRT __ftol at 0x634F30 (221 call sites) truncates via the classic
// fnstcw/fldcw round-mode dance. fldcw serializes the FPU and is brutally
// slow under Rosetta 2. SSE3's fisttp does the same truncating pop-store
// without touching the control word, and Rosetta always provides SSE3.
bool dk2::installFtolSse3() {
    auto *target = reinterpret_cast<uint8_t *>(0x00634F30);
    static const uint8_t original[] = {0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4,
                                       0x9B, 0xD9, 0x7D, 0xFE};
    if (std::memcmp(target, original, sizeof(original)) != 0) {
        patch::log::err("ftol patch: unexpected bytes at 00634F30");
        return false;
    }
    // sub esp,8; fisttp qword [esp]; mov eax,[esp]; mov edx,[esp+4];
    // add esp,8; ret
    static const uint8_t replacement[] = {
            0x83, 0xEC, 0x08,
            0xDD, 0x0C, 0x24,
            0x8B, 0x04, 0x24,
            0x8B, 0x54, 0x24, 0x04,
            0x83, 0xC4, 0x08,
            0xC3};
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, sizeof(replacement), PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        patch::log::err("ftol patch: VirtualProtect failed: %08X", GetLastError());
        return false;
    }
    std::memcpy(target, replacement, sizeof(replacement));
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(replacement), oldProtection, &ignored);
    patch::log::dbg("ftol patch: __ftol now fisttp (SSE3)");
    return true;
}
