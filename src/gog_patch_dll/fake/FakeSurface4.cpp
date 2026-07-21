//
// Created by DiaLight on 20.01.2023.
//
#include <fake/FakeSurface4.h>
#include <fake/FakeSurface.h>
#include <gog_globals.h>
#include <fake/FakeGammaControl.h>
#include <fake/FakeTexture.h>
#include <gog_fake.h>
#include <gog_debug.h>
#include <metal_bridge/MetalBridgeProducer.h>
#include <algorithm>
#include <mutex>
#include <new>

using namespace gog;

namespace {
bool isBump16(const DDSURFACEDESC2 &desc) {
    return desc.ddpfPixelFormat.dwRGBBitCount == 16 &&
           (desc.ddpfPixelFormat.dwFlags & DDPF_BUMPDUDV) != 0;
}

DWORD bytesPerPixel(const DDSURFACEDESC2 &desc) {
    return desc.ddpfPixelFormat.dwRGBBitCount / 8;
}

std::recursive_mutex &cpuSurfaceMutex() {
    // ponytail: one lock keeps cursor-worker Blts and render-thread captures
    // coherent. Split per surface only if profiling shows contention.
    static std::recursive_mutex mutex;
    return mutex;
}

bool validRect(const RECT &rect, const DDSURFACEDESC2 &desc) {
    return rect.left >= 0 && rect.top >= 0 && rect.right >= rect.left &&
           rect.bottom >= rect.top && rect.right <= static_cast<LONG>(desc.dwWidth) &&
           rect.bottom <= static_cast<LONG>(desc.dwHeight);
}
}


FakeSurface4::FakeSurface4(LPDIRECTDRAWSURFACE4 orig_surf, bool isModSurf) {
    this->f90_ownedPixels = nullptr;
    this->f94_ownedPitch = 0;
    this->f98_ownedSize = 0;
    this->f8_orig_surf = orig_surf;
    orig_surf->AddRef();
    this->fC_isModSurf = isModSurf;
    this->f10_lockCounter = 0;
    memset(&this->f14_desc, 0, sizeof(this->f14_desc));
    this->f14_desc.dwSize = sizeof(DDSURFACEDESC2);
    static_assert(sizeof(DDSURFACEDESC2) == 124);
    HRESULT hr = this->f8_orig_surf->GetSurfaceDesc(&this->f14_desc);
    if (FAILED(hr)) gog_assert_failed("FakeSurface4::FakeSurface4:200");
}

FakeSurface4::FakeSurface4(LPDDSURFACEDESC2 pDesc, bool displaySurface, bool isModSurf) {
    this->f8_orig_surf = nullptr;
    this->fC_isModSurf = isModSurf;
    this->f90_ownedPixels = nullptr;
    this->f94_ownedPitch = 0;
    this->f98_ownedSize = 0;
    this->f10_lockCounter = 0;
    static_assert(sizeof(DDSURFACEDESC2) == 0x7C);
    DDSURFACEDESC2 desc;
    memcpy(&desc, pDesc, sizeof(desc));
    DWORD dwFlags = desc.dwFlags;
    if (!displaySurface) {
        if ((desc.dwFlags & 1) != 0 && (desc.ddsCaps.dwCaps & 0x200) != 0 && (desc.dwFlags & 4) == 0)
            gog_assert_failed("FakeSurface4::FakeSurface4:207");
        if ((dwFlags & 0x1000) == 0) gog_assert_failed("FakeSurface4::FakeSurface4:210");
        if (desc.ddpfPixelFormat.dwRGBBitCount != 32 && !isBump16(desc))
            gog_assert_failed("FakeSurface4::FakeSurface4:211");
    }
    memcpy(&this->f14_desc, &desc, sizeof(this->f14_desc));
    if (!displaySurface) {
        if ((this->f14_desc.ddsCaps.dwCaps & 0x30000000) != 0) gog_assert_failed("FakeSurface4::FakeSurface4:216");
        if ((this->f14_desc.ddsCaps.dwCaps & 0x1000) == 0) gog_assert_failed("FakeSurface4::FakeSurface4:217");
        if ((this->f14_desc.dwFlags & 0x40) != 0) gog_assert_failed("FakeSurface4::FakeSurface4:218");
    }
    if (!this->f14_desc.dwWidth) gog_assert_failed("FakeSurface4::FakeSurface4:219");
    if (!this->f14_desc.dwHeight) gog_assert_failed("FakeSurface4::FakeSurface4:220");
    const bool useCpuBacking =
        (metal_bridge::isEnabled() && isBump16(this->f14_desc)) ||
        (metal_bridge::headlessDirectDrawEnabled() &&
         this->f14_desc.ddpfPixelFormat.dwRGBBitCount == 32);
    if (useCpuBacking) {
        // The Metal bridge consumes these pixels directly; WineD3D never uses
        // the surface. Keep the driver-like pitch and slack required by DK2's
        // 1999-era fill routines.
        //
        // Pitch/size deliberately mimic a real driver surface rather than a
        // tight CPU array. A dword-tight pitch with zero slack after the last
        // row put the next heap block header directly behind e.g. a 2x2 bump
        // surface's 8-byte buffer; DK2's 1999-era fill code (written against
        // drivers whose surfaces always carried padded pitches and trailing
        // slack) then smashed that header, and the process died later inside
        // RtlFreeHeap walking the corrupted block (page fault at ntdll
        // +0x4F3F2 reading a garbage-derived subheap base). Generous pitch
        // alignment plus one full spare row of slack absorbs those writes the
        // same way real surface memory always did.
        const DWORD pixelBytes = bytesPerPixel(this->f14_desc);
        const unsigned long long pitch =
            (static_cast<unsigned long long>(this->f14_desc.dwWidth) * pixelBytes + 15u) & ~15ull;
        // Size as if this were a 4-byte/pixel page, not our real 2-byte one.
        // DK2 fills 128x128 cache pages with family-shared code whose pitch
        // comes from the 32bpp page family (512), so a 16bpp bump page gets
        // written with double our pitch - 64KB into what a tight allocation
        // would make a 33KB buffer. On 1999 hardware that overflow landed in
        // adjacent video memory and was harmless; on the Wine heap it smashed
        // block headers and killed the process ~500 allocations later (page
        // fault at ntdll+0x4F3F2 on a corrupt free-list walk). Rows are still
        // read back at the correct 2-byte pitch; the extra space just absorbs
        // the overshoot exactly like real surface memory did.
        const unsigned long long allocationPitch = isBump16(this->f14_desc)
            ? (static_cast<unsigned long long>(this->f14_desc.dwWidth) * 4u + 15u) & ~15ull
            : pitch;
        const unsigned long long size =
            allocationPitch * (this->f14_desc.dwHeight + 1ull) + 64u;
        if (!pitch || size > 0xFFFFFFFFull) {
            gog_assert_failed("FakeSurface4::FakeSurface4:224");
            return;
        }
        this->f90_ownedPixels = static_cast<BYTE *>(operator new(
            static_cast<size_t>(size), std::nothrow));
        if (!this->f90_ownedPixels) {
            gog_assert_failed("FakeSurface4::FakeSurface4:225");
            return;
        }
        this->f94_ownedPitch = static_cast<DWORD>(pitch);
        this->f98_ownedSize = static_cast<DWORD>(size);
        memset(this->f90_ownedPixels, 0, this->f98_ownedSize);
        this->f14_desc.dwFlags |= DDSD_PITCH | DDSD_LPSURFACE;
        this->f14_desc.lPitch = static_cast<LONG>(this->f94_ownedPitch);
        this->f14_desc.lpSurface = this->f90_ownedPixels;
        if (isBump16(this->f14_desc))
            gog_debugf("Metal bridge: CPU bump surface %ux%u pitch=%u flags=0x%x",
                       this->f14_desc.dwWidth, this->f14_desc.dwHeight,
                       this->f94_ownedPitch, this->f14_desc.ddpfPixelFormat.dwFlags);
        return;
    }
    HRESULT hr = orig::pIDirectDraw4->CreateSurface(&this->f14_desc, &this->f8_orig_surf, NULL);
    if (FAILED(hr)) {
        gog_assert_failed_hr("FakeSurface4::FakeSurface4:227", hr);
        if (hr == DDERR_INVALIDCAPS) {
        }
        this->f8_orig_surf = nullptr;
        __debugbreak();
    }
}

HRESULT FakeSurface4::QueryInterface(REFIID riid, LPVOID FAR *ppvObj) {
    if (!ppvObj) return E_POINTER;
    *ppvObj = nullptr;
    if (IsEqualGUID(IID_IUnknown, riid) || IsEqualGUID(IID_IDirectDrawSurface4, riid)) {
        *ppvObj = this;
        AddRef();
        return DD_OK;
    }
    if (IsEqualGUID(IID_IDirectDrawGammaControl, riid)) {
        *ppvObj = FakeGammaControl::instance;
        return DD_OK;
    }
    if (IsEqualGUID(IID_IDirect3DTexture2, riid)) {
        if (this->f90_ownedPixels) {
            *ppvObj = new FakeTexture(nullptr, this);
            return DD_OK;
        }
        IDirect3DTexture2 *orig_tex = nullptr;
        HRESULT hr = this->f8_orig_surf->QueryInterface(IID_IDirect3DTexture2, (LPVOID *) &orig_tex);
        if (SUCCEEDED(hr)) {
            *ppvObj = new FakeTexture(orig_tex, this->f8_orig_surf);
        } else if (metal_bridge::isEnabled()) {
            // Metal only needs the lockable DirectDraw backing surface. Wine may
            // legitimately reject IDirect3DTexture2 for 16-bit bump formats.
            gog_debugf("Metal bridge: virtual texture fallback after QueryInterface failed (hr=0x%x, flags=0x%x, bits=%u)",
                       hr, this->f14_desc.ddpfPixelFormat.dwFlags,
                       this->f14_desc.ddpfPixelFormat.dwRGBBitCount);
            *ppvObj = new FakeTexture(nullptr, this->f8_orig_surf);
            return DD_OK;
        }
        return hr;
    }
    gog_unused_function_called("FakeSurface4::QueryInterface");
    return DDERR_GENERIC;
}

ULONG FakeSurface4::Release(void) {
    if (--this->refs != 0)
        return this->refs;
    std::lock_guard<std::recursive_mutex> lock(cpuSurfaceMutex());
    if (this->f8_orig_surf) {
        this->f8_orig_surf->Release();
        this->f8_orig_surf = nullptr;
    }
    if (this->f90_ownedPixels) {
        operator delete(this->f90_ownedPixels);
        this->f90_ownedPixels = nullptr;
    }
    operator delete(this);
    return 0;
}

HRESULT FakeSurface4::AddAttachedSurface(LPDIRECTDRAWSURFACE4) {
    gog_unused_function_called("FakeSurface4::AddAttachedSurface");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::AddOverlayDirtyRect(LPRECT) {
    gog_unused_function_called("FakeSurface4::AddOverlayDirtyRect");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::Blt(LPRECT dstRect, LPDIRECTDRAWSURFACE4 srcSurf_, LPRECT srcRect,
                          DWORD flags, LPDDBLTFX effects) {
    if (this->f90_ownedPixels) {
        std::lock_guard<std::recursive_mutex> lock(cpuSurfaceMutex());
        auto *srcSurf = static_cast<FakeSurface4 *>(srcSurf_);
        const DWORD operation = flags & ~DDBLT_WAIT;
        const bool colorFill = (operation & DDBLT_COLORFILL) != 0;
        const bool sourceColorKey = (operation & DDBLT_KEYSRC) != 0 || (operation & 1u) != 0;
        if (operation & ~(DDBLT_COLORFILL | DDBLT_KEYSRC | 1u)) return DDERR_UNSUPPORTED;
        RECT src = srcRect ? *srcRect : RECT{0, 0,
            srcSurf ? static_cast<LONG>(srcSurf->f14_desc.dwWidth) : 0,
            srcSurf ? static_cast<LONG>(srcSurf->f14_desc.dwHeight) : 0};
        RECT dst = dstRect ? *dstRect : RECT{0, 0,
            static_cast<LONG>(this->f14_desc.dwWidth),
            static_cast<LONG>(this->f14_desc.dwHeight)};
        if (!validRect(dst, this->f14_desc)) return DDERR_INVALIDRECT;
        const DWORD destinationBytes = bytesPerPixel(this->f14_desc);
        if (colorFill) {
            if (srcSurf || !effects || !destinationBytes) return DDERR_INVALIDPARAMS;
            const LONG fillWidth = dst.right - dst.left;
            const DWORD fill = effects->dwFillColor;
            for (LONG y = dst.top; y < dst.bottom; ++y) {
                BYTE *row = this->f90_ownedPixels + y * this->f94_ownedPitch +
                            dst.left * destinationBytes;
                switch (destinationBytes) {
                case 4: {
                    auto *px = reinterpret_cast<uint32_t *>(row);
                    for (LONG x = 0; x < fillWidth; ++x) px[x] = fill;
                    break;
                }
                case 2: {
                    auto *px = reinterpret_cast<uint16_t *>(row);
                    const uint16_t f16 = static_cast<uint16_t>(fill);
                    for (LONG x = 0; x < fillWidth; ++x) px[x] = f16;
                    break;
                }
                case 1:
                    memset(row, static_cast<int>(fill & 0xFF), fillWidth);
                    break;
                default:
                    for (LONG x = 0; x < fillWidth; ++x) {
                        memcpy(row + x * destinationBytes, &fill, destinationBytes);
                    }
                    break;
                }
            }
            metal_bridge::textureDirty(this);
            return DD_OK;
        }
        if (!srcSurf || !srcSurf->f90_ownedPixels ||
            !validRect(src, srcSurf->f14_desc)) return DDERR_INVALIDRECT;
        const LONG sourceWidth = src.right - src.left;
        const LONG sourceHeight = src.bottom - src.top;
        const LONG destinationWidth = dst.right - dst.left;
        const LONG destinationHeight = dst.bottom - dst.top;
        const DWORD sourceBytes = bytesPerPixel(srcSurf->f14_desc);
        if (sourceBytes != destinationBytes || !sourceBytes) return DDERR_UNSUPPORTED;
        if (sourceColorKey && (srcSurf->f14_desc.dwFlags & DDSD_CKSRCBLT) == 0)
            return DDERR_NOCOLORKEY;
        if (!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight)
            return DD_OK;
        const bool scaled = sourceWidth != destinationWidth || sourceHeight != destinationHeight;
        if (scaled) {
            if (this == srcSurf) return DDERR_UNSUPPORTED;
            for (LONG y = 0; y < destinationHeight; ++y) {
                const LONG sourceY = src.top +
                    static_cast<LONG>(static_cast<long long>(y) * sourceHeight / destinationHeight);
                BYTE *destination = this->f90_ownedPixels +
                    (dst.top + y) * this->f94_ownedPitch + dst.left * destinationBytes;
                const BYTE *sourceRow = srcSurf->f90_ownedPixels +
                    sourceY * srcSurf->f94_ownedPitch;
                for (LONG x = 0; x < destinationWidth; ++x) {
                    const LONG sourceX = src.left +
                        static_cast<LONG>(static_cast<long long>(x) * sourceWidth / destinationWidth);
                    const BYTE *source = sourceRow + sourceX * sourceBytes;
                    DWORD pixel = 0;
                    memcpy(&pixel, source, sourceBytes);
                    if (!sourceColorKey ||
                        pixel < srcSurf->f14_desc.ddckCKSrcBlt.dwColorSpaceLowValue ||
                        pixel > srcSurf->f14_desc.ddckCKSrcBlt.dwColorSpaceHighValue)
                        memcpy(destination, source, sourceBytes);
                    destination += destinationBytes;
                }
            }
            metal_bridge::textureDirty(this);
            return DD_OK;
        }
        const LONG firstRow = this == srcSurf && dst.top > src.top ? sourceHeight - 1 : 0;
        const LONG rowStep = firstRow ? -1 : 1;
        for (LONG rowIndex = firstRow;
             rowIndex >= 0 && rowIndex < sourceHeight;
             rowIndex += rowStep) {
            BYTE *destination = this->f90_ownedPixels +
                (dst.top + rowIndex) * this->f94_ownedPitch + dst.left * destinationBytes;
            const BYTE *source = srcSurf->f90_ownedPixels +
                (src.top + rowIndex) * srcSurf->f94_ownedPitch + src.left * sourceBytes;
            if (!sourceColorKey) {
                memmove(destination, source, static_cast<size_t>(sourceWidth) * sourceBytes);
                continue;
            }
            for (LONG x = 0; x < sourceWidth; ++x) {
                DWORD pixel = 0;
                memcpy(&pixel, source, sourceBytes);
                if (pixel < srcSurf->f14_desc.ddckCKSrcBlt.dwColorSpaceLowValue ||
                    pixel > srcSurf->f14_desc.ddckCKSrcBlt.dwColorSpaceHighValue)
                    memcpy(destination, source, sourceBytes);
                source += sourceBytes;
                destination += destinationBytes;
            }
        }
        metal_bridge::textureDirty(this);
        return DD_OK;
    }
    auto *srcSurf = static_cast<FakeSurface4 *>(srcSurf_);
    return this->f8_orig_surf->Blt(dstRect, srcSurf ? srcSurf->f8_orig_surf : nullptr,
                                  srcRect, flags, effects);
}

HRESULT FakeSurface4::BltBatch(LPDDBLTBATCH, DWORD, DWORD) {
    gog_unused_function_called("FakeSurface4::BltBatch");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE4 srcSurf_, LPRECT srcRect, DWORD a6) {
    auto *srcSurf = (FakeSurface4 *) srcSurf_;
    if (!srcSurf) gog_assert_failed("FakeSurface4::BltFast:264");
    if (this->fC_isModSurf) gog_assert_failed("FakeSurface4::BltFast:265");
    if (this->f90_ownedPixels) {
        if (!srcSurf || !srcSurf->f90_ownedPixels ||
            (a6 & ~(DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY))) return DDERR_UNSUPPORTED;
        RECT src = srcRect ? *srcRect : RECT{0, 0,
            static_cast<LONG>(srcSurf->f14_desc.dwWidth),
            static_cast<LONG>(srcSurf->f14_desc.dwHeight)};
        const LONG width = src.right - src.left;
        const LONG height = src.bottom - src.top;
        if (width < 0 || height < 0 || src.left < 0 || src.top < 0 ||
            src.right > static_cast<LONG>(srcSurf->f14_desc.dwWidth) ||
            src.bottom > static_cast<LONG>(srcSurf->f14_desc.dwHeight) ||
            x + static_cast<DWORD>(width) > this->f14_desc.dwWidth ||
            y + static_cast<DWORD>(height) > this->f14_desc.dwHeight)
            return DDERR_INVALIDRECT;
        RECT destination = {static_cast<LONG>(x), static_cast<LONG>(y),
                            static_cast<LONG>(x) + width, static_cast<LONG>(y) + height};
        DWORD bltFlags = (a6 & DDBLTFAST_WAIT) ? DDBLT_WAIT : 0;
        if (a6 & DDBLTFAST_SRCCOLORKEY) bltFlags |= DDBLT_KEYSRC;
        return Blt(&destination, srcSurf_, &src, bltFlags, nullptr);
    }
    IDirectDrawSurface4 *srcSurf_1 = nullptr;
    if (srcSurf) srcSurf_1 = srcSurf->f8_orig_surf;
    const bool isOverlay = FakeSurface::instance_cpy &&
                           this->f8_orig_surf == FakeSurface::instance_cpy->orig();
    if (isOverlay && srcSurf_1)
        metal_bridge::overlayBltFast(this->f8_orig_surf, x, y,
                                     srcSurf_1, srcRect, a6);
    HRESULT hr;
    hr = this->f8_orig_surf->BltFast(x, y, srcSurf_1, srcRect, a6);
    if (FAILED(hr)) {
        gog_assert_failed_hr("FakeSurface4::BltFast:282", hr);
        return hr;
    }
    if (isOverlay) {
        RECT destination;
        if (srcRect) {
            destination = {(LONG)x, (LONG)y,
                           (LONG)x + srcRect->right - srcRect->left,
                           (LONG)y + srcRect->bottom - srcRect->top};
        } else if (srcSurf) {
            destination = {(LONG)x, (LONG)y,
                           (LONG)(x + srcSurf->f14_desc.dwWidth),
                           (LONG)(y + srcSurf->f14_desc.dwHeight)};
        } else {
            destination = {};
        }
        metal_bridge::overlayDrawn(destination.right > destination.left ? &destination : nullptr);
    }
    IDirectDrawSurface4 *bridgeSurface =
        metal_bridge::isEnabled() && isBump16(this->f14_desc) ? this : this->f8_orig_surf;
    metal_bridge::textureDirty(bridgeSurface);
    if (!this->fC_isModSurf) return hr;
    if (!orig::pIDirectDrawSurface4_coop) {
        gog_assert_failed("FakeSurface4::BltFast:285");
    }
    orig::pIDirectDrawSurface4_coop->Blt(&g_renderRect, this->f8_orig_surf, NULL, 0x1000000, NULL);
    return hr;
}

HRESULT FakeSurface4::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE4) {
    gog_unused_function_called("FakeSurface4::DeleteAttachedSurface");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK2) {
    gog_unused_function_called("FakeSurface4::EnumAttachedSurfaces");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK2) {
    gog_unused_function_called("FakeSurface4::EnumOverlayZOrders");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::Flip(LPDIRECTDRAWSURFACE4, DWORD) {
    gog_unused_function_called("FakeSurface4::Flip");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetAttachedSurface(LPDDSCAPS2, LPDIRECTDRAWSURFACE4 *) {
    gog_unused_function_called("FakeSurface4::GetAttachedSurface");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetBltStatus(DWORD flags) {
    return this->f90_ownedPixels ? DD_OK : this->f8_orig_surf->GetBltStatus(flags);
}

HRESULT FakeSurface4::GetCaps(LPDDSCAPS2 caps) {
    if (this->f90_ownedPixels && caps) {
        *caps = this->f14_desc.ddsCaps;
        return DD_OK;
    }
    return caps ? this->f8_orig_surf->GetCaps(caps) : DDERR_INVALIDPARAMS;
}

HRESULT FakeSurface4::GetClipper(LPDIRECTDRAWCLIPPER *) {
    gog_unused_function_called("FakeSurface4::GetClipper");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetColorKey(DWORD flags, LPDDCOLORKEY key) {
    if (!this->f90_ownedPixels)
        return this->f8_orig_surf->GetColorKey(flags, key);
    if (!key || flags != DDCKEY_SRCBLT) return DDERR_INVALIDPARAMS;
    if ((this->f14_desc.dwFlags & DDSD_CKSRCBLT) == 0) return DDERR_NOCOLORKEY;
    *key = this->f14_desc.ddckCKSrcBlt;
    return DD_OK;
}

HRESULT FakeSurface4::GetDC(HDC *dc) {
    return this->f90_ownedPixels ? DDERR_UNSUPPORTED : this->f8_orig_surf->GetDC(dc);
}

HRESULT FakeSurface4::GetFlipStatus(DWORD flags) {
    return this->f90_ownedPixels ? DD_OK : this->f8_orig_surf->GetFlipStatus(flags);
}

HRESULT FakeSurface4::GetOverlayPosition(LPLONG, LPLONG) {
    gog_unused_function_called("FakeSurface4::GetOverlayPosition");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetPalette(LPDIRECTDRAWPALETTE *) {
    gog_unused_function_called("FakeSurface4::GetPalette");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetPixelFormat(LPDDPIXELFORMAT format) {
    if (this->f90_ownedPixels && format) {
        *format = this->f14_desc.ddpfPixelFormat;
        return DD_OK;
    }
    return format ? this->f8_orig_surf->GetPixelFormat(format) : DDERR_INVALIDPARAMS;
}

HRESULT FakeSurface4::GetSurfaceDesc(LPDDSURFACEDESC2 desc) {
    if (this->f90_ownedPixels && desc) {
        *desc = this->f14_desc;
        return DD_OK;
    }
    return desc ? this->f8_orig_surf->GetSurfaceDesc(desc) : DDERR_INVALIDPARAMS;
}

HRESULT FakeSurface4::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) {
    gog_unused_function_called("FakeSurface4::Initialize");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::IsLost(void) {
    return this->f90_ownedPixels ? DD_OK : this->f8_orig_surf->IsLost();
}

HRESULT FakeSurface4::Lock(LPRECT pRect, LPDDSURFACEDESC2 surf, DWORD a4, HANDLE a5) {
    if (this->f90_ownedPixels) {
        cpuSurfaceMutex().lock();
        if (!surf) {
            cpuSurfaceMutex().unlock();
            return DDERR_INVALIDPARAMS;
        }
        if (this->f10_lockCounter) {
            cpuSurfaceMutex().unlock();
            return DDERR_SURFACEBUSY;
        }
        *surf = this->f14_desc;
        if (pRect) {
            if (pRect->left < 0 || pRect->top < 0 || pRect->right < pRect->left ||
                pRect->bottom < pRect->top ||
                pRect->right > static_cast<LONG>(this->f14_desc.dwWidth) ||
                pRect->bottom > static_cast<LONG>(this->f14_desc.dwHeight))
                {
                    cpuSurfaceMutex().unlock();
                    return DDERR_INVALIDRECT;
                }
            surf->dwWidth = pRect->right - pRect->left;
            surf->dwHeight = pRect->bottom - pRect->top;
            surf->lpSurface = this->f90_ownedPixels +
                pRect->top * this->f94_ownedPitch + pRect->left * bytesPerPixel(this->f14_desc);
        }
        ++this->f10_lockCounter;
        return DD_OK;
    }
    const bool isMetalBump = metal_bridge::isEnabled() && isBump16(this->f14_desc);
    const DDPIXELFORMAT logicalFormat = this->f14_desc.ddpfPixelFormat;
    HRESULT hr = this->f8_orig_surf->Lock(pRect, surf, a4, a5);
    if (FAILED(hr)) {
        gog_assert_failed_hr("FakeSurface4::Lock:309", hr);
        return hr;
    }
    this->f14_desc = *surf;
    if (isMetalBump) {
        this->f14_desc.ddpfPixelFormat = logicalFormat;
        surf->ddpfPixelFormat = logicalFormat;
    }
    ++this->f10_lockCounter;
    return hr;
}

HRESULT FakeSurface4::ReleaseDC(HDC dc) {
    return this->f90_ownedPixels ? DDERR_UNSUPPORTED : this->f8_orig_surf->ReleaseDC(dc);
}

HRESULT FakeSurface4::Restore(void) {
    return this->f90_ownedPixels ? DD_OK : this->f8_orig_surf->Restore();
}

HRESULT FakeSurface4::SetClipper(LPDIRECTDRAWCLIPPER clipper) {
    return this->f90_ownedPixels ? DD_OK : this->f8_orig_surf->SetClipper(clipper);
}

HRESULT FakeSurface4::SetColorKey(DWORD flags, LPDDCOLORKEY key) {
    if (!this->f90_ownedPixels)
        return this->f8_orig_surf->SetColorKey(flags, key);
    if (flags != DDCKEY_SRCBLT) return DDERR_INVALIDPARAMS;
    if (!key) {
        this->f14_desc.dwFlags &= ~DDSD_CKSRCBLT;
        return DD_OK;
    }
    this->f14_desc.ddckCKSrcBlt = *key;
    this->f14_desc.dwFlags |= DDSD_CKSRCBLT;
    return DD_OK;
}

HRESULT FakeSurface4::SetOverlayPosition(LONG, LONG) {
    gog_unused_function_called("FakeSurface4::SetOverlayPosition");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetPalette(LPDIRECTDRAWPALETTE palette) {
    return this->f90_ownedPixels ? DDERR_UNSUPPORTED : this->f8_orig_surf->SetPalette(palette);
}

HRESULT FakeSurface4::Unlock(LPRECT pRect) {
    if (this->f90_ownedPixels && !this->f10_lockCounter)
        return DDERR_NOTLOCKED;
    IDirectDrawSurface4 *bridgeSurface =
        this->f90_ownedPixels ? this : this->f8_orig_surf;
    metal_bridge::textureDirty(bridgeSurface, pRect ? nullptr : &this->f14_desc);
    if (this->f90_ownedPixels) {
        --this->f10_lockCounter;
        cpuSurfaceMutex().unlock();
        return DD_OK;
    }
    HRESULT hr = this->f8_orig_surf->Unlock(pRect);
    if (FAILED(hr)) {
        gog_assert_failed_hr("FakeSurface4::Unlock:322", hr);
        return hr;
    }
    --this->f10_lockCounter;
    if (this->fC_isModSurf) {
        if (!this->f10_lockCounter) Fake_Redraw();
    }
    return hr;
}

HRESULT FakeSurface4::UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE4, LPRECT, DWORD, LPDDOVERLAYFX) {
    gog_unused_function_called("FakeSurface4::UpdateOverlay");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::UpdateOverlayDisplay(DWORD) {
    gog_unused_function_called("FakeSurface4::UpdateOverlayDisplay");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE4) {
    gog_unused_function_called("FakeSurface4::UpdateOverlayZOrder");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetDDInterface(LPVOID *) {
    gog_unused_function_called("FakeSurface4::GetDDInterface");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::PageLock(DWORD) {
    gog_unused_function_called("FakeSurface4::PageLock");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::PageUnlock(DWORD) {
    gog_unused_function_called("FakeSurface4::PageUnlock");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) {
    gog_unused_function_called("FakeSurface4::SetSurfaceDesc");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) {
    gog_unused_function_called("FakeSurface4::SetPrivateData");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetPrivateData(REFGUID, LPVOID, LPDWORD) {
    gog_unused_function_called("FakeSurface4::GetPrivateData");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::FreePrivateData(REFGUID) {
    gog_unused_function_called("FakeSurface4::FreePrivateData");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetUniquenessValue(LPDWORD) {
    gog_unused_function_called("FakeSurface4::GetUniquenessValue");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::ChangeUniquenessValue(void) {
    gog_unused_function_called("FakeSurface4::ChangeUniquenessValue");
    return DDERR_GENERIC;
}
