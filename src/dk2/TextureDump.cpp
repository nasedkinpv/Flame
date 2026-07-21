//
// Created by Flametal contributor.
//
// Named texture dump (see TextureDump.h).
//
// Decode-path summary (investigated read-only, see report): DK2 loads WAD
// archives (EngineTextures.wad, Texture.Wad/dir, Sprite.Wad, FrontEnd.wad,
// ...) through MyDir_CFileManager, decompresses per-entry pixel blocks
// (TextureDecompressBlock_tqia_dec @0x659B90, tqia_decompressJpeg @0x5B26F0
// for JPEG-backed slots) into a raw dk2::MySurface, then that surface is
// named and cached by dk2::MyCESurfHandle inside the global
// MyStringHashMap_MyCESurfHandle string->handle table (name -> handle,
// see MyStringHashMap_MyCESurfHandle_entry.h). The narrowest point where a
// decoded MySurface and its resource name are both available *before* the
// pixels get composited into a shared atlas page is
// dk2::MyCESurfHandle::paint(MySurface *surf, char computeCrc) (original
// @0x590C30, re-implemented in src/dk2/engine/scene/MyCESurfHandle.cpp):
// `surf` is the just-decoded/just-updated source surface, and
// `MyStringHashMap_MyCESurfHandle_instance.entries.buf[this->mapIdx].name`
// is its resource name. paint() then calls MySurface_blend(...) to stamp
// `surf` onto `this->cesurf`'s shared page buffer, which is the point the
// previous host-side hash-named dump (macos/native/DK2Metal.mm) captured --
// after atlas compositing, hence its "collage" pages and content-hash names.
// Hooking here instead lets us dump one clean, named PNG per resource.
// Update: the "silhouette" 8bpp output traced back to the bytesPerPixel==1
// branch of convertToRgba8() below treating the byte as a raw grey level.
// DK2's 8-bit surfaces are palette-indexed against the single shared
// DirectDraw palette (dk2::g_paletteEntries, see dk2/engine/window_functions.cpp
// updatePalette()/dk2dd_updatePalette_devTexture and ddraw_functions.cpp's
// d_ge_dk2dd_init), not greyscale -- resolved below via that global.
#include "dk2/TextureDump.h"

#include "dk2/MySurface.h"
#include "dk2/MySurfDesc.h"
#include "dk2_globals.h"
#include "patches/logging.h"
#include "tools/flametal_config.h"

#include <lodepng.h>

#include <cstdint>
#include <cstdio>
#include <climits>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>


flametal_config::define_flame_option<std::string> o_textureDump(
    "flametal:TextureDump", flametal_config::OG_Config,
    "Dump every uniquely-named decoded texture as a PNG into this directory,\n"
    "named after its resource name/id. Empty (default) disables dumping.\n"
    "",
    ""
);

namespace patch::texture_dump {

namespace {

// Splits on both separators and creates each missing component; ERROR_ALREADY_EXISTS
// from CreateDirectoryA is expected and not a failure.
bool ensureDirRecursive(const std::string &path) {
    std::string cur;
    cur.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        cur += c;
        bool isSep = (c == '\\' || c == '/');
        bool lastChar = (i + 1 == path.size());
        if (!isSep && !lastChar) continue;
        if (cur.size() <= 3 && cur.find(':') != std::string::npos) continue;  // "C:\"
        if (!CreateDirectoryA(cur.c_str(), nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

// Only the '\' path separator (and NUL) is unusable on Windows for entries
// under our own dump directory; be conservative and replace the rest of the
// classic reserved set too, since resource names come straight from WAD
// entries and are not guaranteed to be filesystem-safe.
std::string sanitizeFileName(const char *name) {
    std::string out;
    if (!name) return "unnamed";
    for (const char *p = name; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            out += '_';
        } else {
            out += static_cast<char>(c);
        }
    }
    if (out.empty()) out = "unnamed";
    return out;
}

// Extracts an 8-bit channel from a packed pixel value given a bitmask,
// scaling up narrow channels (e.g. 5-bit) to the full 0-255 range.
uint8_t extractChannel8(uint32_t pixel, uint32_t mask) {
    if (mask == 0) return 0;
    uint32_t v = pixel & mask;
    uint32_t shift = 0;
    uint32_t m = mask;
    while (m && !(m & 1)) { m >>= 1; ++shift; }
    v >>= shift;
    uint32_t bits = 0;
    while (m & 1) { m >>= 1; ++bits; }
    if (bits == 0) return 0;
    if (bits >= 8) return static_cast<uint8_t>(v >> (bits - 8));
    // replicate to fill 8 bits (5-bit 0..31 -> 0..255 without banding)
    uint32_t maxv = (1u << bits) - 1u;
    return static_cast<uint8_t>((v * 255u + maxv / 2u) / maxv);
}

uint32_t loadPackedPixel(const uint8_t *src, uint32_t bytesPerPixel) {
    switch (bytesPerPixel) {
    case 1: return src[0];
    case 2: return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8);
    case 3:
        return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
               (static_cast<uint32_t>(src[2]) << 16);
    case 4:
        return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
               (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
    default: return 0;
    }
}

// Converts an arbitrary MySurfDesc-described surface into tightly-packed
// RGBA8. Returns false (leaving `out` untouched) on anything that doesn't
// look like sane pixel data, so callers can skip with a log instead of
// risking an out-of-bounds read on a malformed/unsupported surface.
bool convertToRgba8(const dk2::MySurface *surf, std::vector<uint8_t> &out) {
    if (!surf || !surf->lpSurface) return false;
    int32_t w = surf->size.w;
    int32_t h = surf->size.h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;
    uint32_t bpp = surf->desc.dwRGBBitCount;
    uint32_t bytesPerPixel = bpp / 8;
    if (bytesPerPixel < 1 || bytesPerPixel > 4) return false;
    int32_t minRowBytes = static_cast<int64_t>(w) * bytesPerPixel > INT32_MAX
            ? -1 : static_cast<int32_t>(w * bytesPerPixel);
    if (minRowBytes <= 0 || surf->lPitch < minRowBytes) return false;

    const bool hasAlpha = surf->desc.dwRGBAlphaBitMask != 0;
    out.resize(static_cast<size_t>(w) * h * 4);
    const auto *rowBase = static_cast<const uint8_t *>(surf->lpSurface);
    for (int32_t y = 0; y < h; ++y, rowBase += surf->lPitch) {
        const uint8_t *px = rowBase;
        uint8_t *dst = &out[static_cast<size_t>(y) * w * 4];
        for (int32_t x = 0; x < w; ++x, px += bytesPerPixel, dst += 4) {
            uint32_t packed = loadPackedPixel(px, bytesPerPixel);
            // 8bpp surfaces are palette-indexed against the single shared
            // DirectDraw palette (dk2::g_paletteEntries), not grey levels --
            // resolving through it is what turns these from silhouettes into
            // real colors.
            if (bytesPerPixel == 1) {
                const tagPALETTEENTRY &pe = dk2::g_paletteEntries[packed & 0xFF];
                dst[0] = pe.peRed;
                dst[1] = pe.peGreen;
                dst[2] = pe.peBlue;
                dst[3] = 0xFF;
                continue;
            }
            dst[0] = extractChannel8(packed, surf->desc.dwRBitMask);
            dst[1] = extractChannel8(packed, surf->desc.dwGBitMask);
            dst[2] = extractChannel8(packed, surf->desc.dwBBitMask);
            dst[3] = hasAlpha ? extractChannel8(packed, surf->desc.dwRGBAlphaBitMask) : 0xFF;
        }
    }
    return true;
}

std::unordered_set<std::string> &dumpedNames() {
    static std::unordered_set<std::string> names;
    return names;
}

// See setCompositeSourceName() in the header for why this exists: compressed
// (world/terrain/creature/room) surfaces have no name of their own.
const char *&compositeSourceNameSlot() {
    static const char *name = nullptr;
    return name;
}

// Resolved once; empty string means the feature stays off for the process.
const std::string &resolvedDir() {
    static std::string dir;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        std::string configured = o_textureDump.get();
        if (!configured.empty() && ensureDirRecursive(configured)) {
            dir = configured;
        } else if (!configured.empty()) {
            patch::log::err("[TextureDump] failed to create dump dir \"%s\", disabling\n",
                    configured.c_str());
        }
    }
    return dir;
}

}  // namespace

void onDecodedSurface(const char *name, const dk2::MySurface *surf) {
    const std::string &dir = resolvedDir();
    if (dir.empty()) return;  // feature off: single string check, no further cost
    if (!name || !*name) return;

    std::string key(name);
    auto &names = dumpedNames();
    if (names.find(key) != names.end()) return;  // already dumped this name

    std::vector<uint8_t> rgba;
    if (!convertToRgba8(surf, rgba)) {
        patch::log::dbg("[TextureDump] skip \"%s\": unsupported/anomalous surface\n", name);
        names.insert(std::move(key));  // don't retry every frame
        return;
    }

    std::string file = dir + "\\" + sanitizeFileName(name) + ".png";
    unsigned err = lodepng_encode32_file(
            file.c_str(), rgba.data(),
            static_cast<unsigned>(surf->size.w), static_cast<unsigned>(surf->size.h));
    if (err) {
        patch::log::err("[TextureDump] failed to write \"%s\": %s\n", file.c_str(),
                lodepng_error_text(err));
    }
    names.insert(std::move(key));
}

void setCompositeSourceName(const char *name) {
    // Cheap even when the feature is off (a pointer store); keeping this
    // unconditional avoids a second option check at every call site and
    // matches how trivial the cost is.
    compositeSourceNameSlot() = name;
}

void onCompositedSurfaceDecoded(const dk2::MySurface *surf) {
    const std::string &dir = resolvedDir();
    if (dir.empty()) return;  // feature off: single string check, no further cost
    onDecodedSurface(compositeSourceNameSlot(), surf);
}

}  // namespace patch::texture_dump
