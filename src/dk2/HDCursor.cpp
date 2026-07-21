//
// Created by Flametal contributor.
//
// HD cursor art substitution + the loadArtToSurfaceEx (005540B0) translation
// that hosts it.
//
// The Hand of Evil cursor is not a runtime 3D render: it is film-strip PNG
// art from Data/Sprite.WAD (Point.png = 41 frames of 82x53 stacked
// vertically, etc). CDefaultPlayerInterface::sub_40C710 (0040C710) loads
// each strip through loadArtToSurfaceEx at its NATIVE size, then
// stretch-Blts it once (static_MyDdSurfaceEx_Blt with NULL rects) into a
// per-cursor surface scaled by dwWidth/640 x dwHeight/480 - at 1600x1200
// that is a 2.5x nearest upscale, which is where the staircase contour
// comes from.
//
// Substitution point: loadArtToSurfaceEx itself. When the request comes
// from MyResources_instance.spriteFileMan, the name matches one of the 15
// known cursor strips and flametal:HDCursorDir points at a directory with
// per-frame art (cursor_<Stem>_f<NN>.png, each exactly 4x the original
// frame), the strip surface is built from those frames at 4x size instead.
// sub_40C710 then stretch-Blts 4x -> 2.5x (a downscale), so the composed
// cursorSurf gets genuinely sharper without touching the cursor pipeline,
// hotspots, tooltips or the strip's frame indexing (all of which work in
// scaled-target coordinates derived from the g_interfaceCursorArr table,
// not from the loaded art's size).
//
// Any failure - option empty, a frame file missing or mis-sized, decode or
// surface error - falls back to the original WAD strip for that cursor.
//
// Translation notes for loadArtToSurfaceEx (verified against the 005540B0
// disassembly): MyFile @esp+0x10 (ctor 005B6E40, dtor 005B6E70), MySurface
// @esp+0x18 (ctor 005B5250); MyFile_openImage(00554000) resolves the name
// through the file manager (this is what lets flametal:alt-resources
// loose-file directories shadow WAD entries); readHeader fills the
// MySurface header incl. size @+0x1C, createOffScreenSurface(005B5700,
// caps 0x800) sizes the destination, resolveDesc(005B5BE0) locks it,
// MySurface_copyFromSurf(005B81A0) repoints the same MySurface at the
// locked pixels, readBody(005B6C70) decodes into it, unlock(005B5C90).
// Failure paths log ("Unable to load...", _sprintf 00634DB0 into 0x740EC0
// in the original - patch::log here) and release the surface (005B57C0)
// when it was already created. *status: 0 on success, -1 on failure; the
// function returns the status pointer.

#include "dk2/MyDdSurfaceEx.h"
#include "dk2/MySurface.h"
#include "dk2/MySurfDesc.h"
#include "dk2/resources/MyResources.h"
#include "dk2/resources/file/MyFile.h"
#include "dk2/resources/graphic/TbGraphicFileLoader.h"
#include "dk2_functions.h"
#include "dk2_globals.h"
#include "patches/logging.h"
#include "tools/flametal_config.h"

#include <lodepng.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

flametal_config::define_flame_option<std::string> o_hdCursorDir(
    "flametal:HDCursorDir", flametal_config::OG_Config,
    "Directory with per-frame HD cursor art (cursor_<Name>_f<NN>.png, each\n"
    "exactly 4x the original frame). Empty (default) keeps the Sprite.WAD\n"
    "originals.",
    ""
);

namespace {

constexpr uint32_t kHdScale = 4;

struct CursorArt {
    const char *file;   // name as requested from spriteFileMan
    uint32_t frames;
    uint32_t frameW, frameH;  // original per-frame size inside the strip
};

// The 15 cursor strips of Sprite.WAD. Frame counts/sizes measured from the
// shipped art; they also match g_interfaceCursorArr and normalizeCursorSize.
const CursorArt kCursorArt[] = {
    {"Point.png", 41, 82, 53},
    {"SmallPoint.png", 1, 42, 20},
    {"Idle.png", 1, 83, 39},
    {"Slap.png", 15, 126, 99},
    {"PickAxeHold.png", 1, 84, 86},
    {"PickAxeTag.png", 1, 88, 65},
    {"PickAxeHoldTagging.png", 1, 88, 65},
    {"SpellCast.png", 12, 87, 104},
    {"SpellPossess.png", 6, 64, 64},
    {"SpellPossessNoGo.png", 1, 64, 64},
    {"HoldGold.png", 1, 63, 43},
    {"HoldThing.png", 1, 74, 70},
    {"DropGold.png", 16, 87, 95},
    {"DropThing.png", 14, 88, 103},
    {"SpellHold.png", 1, 63, 100},
};

// Packs one 8-bit channel value into an arbitrary surface bitmask,
// truncating to the mask's width (inverse of TextureDump's extractChannel8).
uint32_t packChannel(uint32_t value8, uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = 0, m = mask;
    while (!(m & 1)) { m >>= 1; ++shift; }
    uint32_t bits = 0;
    while (m & 1) { m >>= 1; ++bits; }
    const uint32_t v = bits >= 8 ? value8 << (bits - 8) : value8 >> (8 - bits);
    return (v << shift) & mask;
}

// Builds surfEx as the 4x strip from per-frame pack PNGs. True only when
// the surface was fully built and unlocked with *status = 0.
bool tryLoadHdStrip(int *status, dk2::MyDdSurfaceEx *surfEx, const char *name) {
    const std::string dir = o_hdCursorDir.get();
    if (dir.empty() || !name || !*name) return false;
    const CursorArt *art = nullptr;
    for (const CursorArt &entry : kCursorArt) {
        if (_stricmp(entry.file, name) == 0) { art = &entry; break; }
    }
    if (!art) return false;

    std::string stem(art->file);
    stem.resize(stem.size() - 4);  // drop ".png"
    const uint32_t frameW = art->frameW * kHdScale;
    const uint32_t frameH = art->frameH * kHdScale;

    // All frames first: a single missing/mis-sized frame rejects the whole
    // cursor (no mixed-resolution strips).
    struct Frame {
        unsigned char *pixels = nullptr;
        ~Frame() { if (pixels) free(pixels); }
    };
    std::vector<Frame> frames(art->frames);
    for (uint32_t i = 0; i < art->frames; ++i) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s\\cursor_%s_f%02u.png",
                      dir.c_str(), stem.c_str(), i);
        unsigned w = 0, h = 0;
        if (lodepng_decode32_file(&frames[i].pixels, &w, &h, path) != 0 ||
            w != frameW || h != frameH) {
            patch::log::dbg("[HDCursor] %s: frame %u missing or not %ux%u, "
                            "using WAD original", name, i, frameW, frameH);
            return false;
        }
    }

    int st = -1;
    if (*dk2::MyDdSurface_createOffScreenSurface(
            &st, frameW, frameH * art->frames, 0x800, &surfEx->dd_surf) < 0) {
        return false;
    }
    if (*dk2::MyDdSurfaceEx_resolveDesc(&st, surfEx, nullptr) < 0) {
        dk2::MyDdSurface_release(&st, &surfEx->dd_surf);
        return false;
    }
    char surfBuf[sizeof(dk2::MySurface)];
    dk2::MySurface &surf = *(dk2::MySurface *) surfBuf;
    surf.constructor_empty();
    dk2::MySurface_copyFromSurf(&surf, surfEx);

    const uint32_t bytesPerPixel = surf.desc.dwRGBBitCount / 8;
    if (!surf.lpSurface || (bytesPerPixel != 2 && bytesPerPixel != 4)) {
        dk2::MyDdSurfaceEx_unlock(surfEx);
        dk2::MyDdSurface_release(&st, &surfEx->dd_surf);
        return false;
    }

    // The cursor pipeline keys transparency on green (sub_40C710 writes
    // 0xFF00FF00 for A8R8G8B8); compose the same value from the masks so
    // 16bpp modes stay correct.
    const uint32_t keyPixel = surf.desc.dwGBitMask | surf.desc.dwRGBAlphaBitMask;
    auto *base = static_cast<uint8_t *>(surf.lpSurface);
    for (uint32_t i = 0; i < art->frames; ++i) {
        const unsigned char *src = frames[i].pixels;
        for (uint32_t y = 0; y < frameH; ++y) {
            uint8_t *row = base +
                    (static_cast<size_t>(i) * frameH + y) * surf.lPitch;
            for (uint32_t x = 0; x < frameW; ++x, src += 4) {
                uint32_t pixel;
                if (src[3] < 128) {
                    pixel = keyPixel;
                } else {
                    pixel = packChannel(src[0], surf.desc.dwRBitMask) |
                            packChannel(src[1], surf.desc.dwGBitMask) |
                            packChannel(src[2], surf.desc.dwBBitMask) |
                            surf.desc.dwRGBAlphaBitMask;
                    if (pixel == keyPixel) {
                        // an actually-green opaque pixel must not turn
                        // transparent; nudge it off the key
                        pixel ^= packChannel(1, surf.desc.dwGBitMask);
                    }
                }
                if (bytesPerPixel == 4) {
                    reinterpret_cast<uint32_t *>(row)[x] = pixel;
                } else {
                    reinterpret_cast<uint16_t *>(row)[x] =
                            static_cast<uint16_t>(pixel);
                }
            }
        }
    }
    dk2::MyDdSurfaceEx_unlock(surfEx);
    *status = 0;
    static bool logged = false;
    if (!logged) {
        patch::log::dbg("[HDCursor] serving 4x cursor art from %s", dir.c_str());
        logged = true;
    }
    return true;
}

}  // namespace

int *__cdecl dk2::loadArtToSurfaceEx(int *status, MyDdSurfaceEx *surfEx,
                                     MyDir_CFileManager *fileMan,
                                     const char *name, uint16_t flags) {
    if (fileMan == &MyResources_instance.spriteFileMan &&
        tryLoadHdStrip(status, surfEx, name)) {
        return status;
    }

    char fileBuf[sizeof(MyFile)];
    MyFile &file = *(MyFile *) fileBuf;
    file.constructor_empty();
    char surfBuf[sizeof(MySurface)];
    MySurface &surf = *(MySurface *) surfBuf;
    surf.constructor_empty();

    bool ok = false;
    int st = -1;
    if (TbGraphicFileLoader *loader = MyFile_openImage(&file, fileMan, name, flags)) {
        if (*loader->readHeader(&st, &file, &surf) >= 0) {
            int createSt = -1;
            if (*MyDdSurface_createOffScreenSurface(
                    &createSt, surf.size.w, surf.size.h, 0x800,
                    &surfEx->dd_surf) < 0) {
                patch::log::dbg("Unable to create surface for art '%s'", name);
            } else if (*MyDdSurfaceEx_resolveDesc(&createSt, surfEx, nullptr) < 0) {
                patch::log::dbg("Unable to lock surface for art '%s'", name);
                MyDdSurface_release(&createSt, &surfEx->dd_surf);
            } else {
                MySurface_copyFromSurf(&surf, surfEx);
                if (*loader->readBody(&st, &surf, &file, nullptr) >= 0) {
                    MyDdSurfaceEx_unlock(surfEx);
                    ok = true;
                } else {
                    MyDdSurfaceEx_unlock(surfEx);
                    MyDdSurface_release(&createSt, &surfEx->dd_surf);
                }
            }
        }
    }
    *status = ok ? 0 : -1;
    file.destructor();
    return status;
}
