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
#include <new>

using namespace gog;

namespace {
bool isBump16(const DDSURFACEDESC2 &desc) {
    return desc.ddpfPixelFormat.dwRGBBitCount == 16 &&
           (desc.ddpfPixelFormat.dwFlags & DDPF_BUMPDUDV) != 0;
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

FakeSurface4::FakeSurface4(LPDDSURFACEDESC2 pDesc) {
    this->f8_orig_surf = nullptr;
    this->f90_ownedPixels = nullptr;
    this->f94_ownedPitch = 0;
    this->f98_ownedSize = 0;
    this->f10_lockCounter = 0;
    static_assert(sizeof(DDSURFACEDESC2) == 0x7C);
    DDSURFACEDESC2 desc;
    memcpy(&desc, pDesc, sizeof(desc));
    DWORD dwFlags = desc.dwFlags;
    if ((desc.dwFlags & 1) != 0 && (desc.ddsCaps.dwCaps & 0x200) != 0 && (desc.dwFlags & 4) == 0) {
        gog_assert_failed("FakeSurface4::FakeSurface4:207");
    } else {
        this->fC_isModSurf = false;
        if ((dwFlags & 0x1000) == 0) gog_assert_failed("FakeSurface4::FakeSurface4:210");
        if (desc.ddpfPixelFormat.dwRGBBitCount != 32 && !isBump16(desc))
            gog_assert_failed("FakeSurface4::FakeSurface4:211");
    }
    memcpy(&this->f14_desc, &desc, sizeof(this->f14_desc));
    if ((this->f14_desc.ddsCaps.dwCaps & 0x30000000) != 0) gog_assert_failed("FakeSurface4::FakeSurface4:216");
    if ((this->f14_desc.ddsCaps.dwCaps & 0x1000) == 0) gog_assert_failed("FakeSurface4::FakeSurface4:217");
    if ((this->f14_desc.dwFlags & 0x40) != 0) gog_assert_failed("FakeSurface4::FakeSurface4:218");
    if (!this->f14_desc.dwWidth) gog_assert_failed("FakeSurface4::FakeSurface4:219");
    if (!this->f14_desc.dwHeight) gog_assert_failed("FakeSurface4::FakeSurface4:220");
    if (metal_bridge::isEnabled() && isBump16(this->f14_desc)) {
        // The Metal bridge consumes bump pixels directly; WineD3D never uses
        // this surface. Keeping the pixels in a plain CPU buffer avoids making
        // Wine manage a legacy bump/RGB surrogate and its COM lifetime.
        const unsigned long long pitch =
            (static_cast<unsigned long long>(this->f14_desc.dwWidth) * 2u + 3u) & ~3ull;
        const unsigned long long size = pitch * this->f14_desc.dwHeight;
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
    if (IsEqualGUID(IID_IDirectDrawGammaControl, riid)) {
        *ppvObj = FakeGammaControl::instance;
        return DD_OK;
    }
    if (IsEqualGUID(IID_IDirect3DTexture2, riid)) {
        if (metal_bridge::isEnabled() && isBump16(this->f14_desc)) {
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
                          DWORD flags, LPDDBLTFX) {
    if (this->f90_ownedPixels) {
        auto *srcSurf = static_cast<FakeSurface4 *>(srcSurf_);
        if (!srcSurf || !srcSurf->f90_ownedPixels || flags != DDBLT_WAIT)
            return DDERR_UNSUPPORTED;
        RECT src = srcRect ? *srcRect : RECT{0, 0,
            static_cast<LONG>(srcSurf->f14_desc.dwWidth),
            static_cast<LONG>(srcSurf->f14_desc.dwHeight)};
        RECT dst = dstRect ? *dstRect : RECT{0, 0,
            static_cast<LONG>(this->f14_desc.dwWidth),
            static_cast<LONG>(this->f14_desc.dwHeight)};
        const LONG width = src.right - src.left;
        const LONG height = src.bottom - src.top;
        if (width < 0 || height < 0 || dst.right - dst.left != width ||
            dst.bottom - dst.top != height || src.left < 0 || src.top < 0 ||
            dst.left < 0 || dst.top < 0 || src.right > static_cast<LONG>(srcSurf->f14_desc.dwWidth) ||
            src.bottom > static_cast<LONG>(srcSurf->f14_desc.dwHeight) ||
            dst.right > static_cast<LONG>(this->f14_desc.dwWidth) ||
            dst.bottom > static_cast<LONG>(this->f14_desc.dwHeight))
            return DDERR_INVALIDRECT;
        for (LONG y = 0; y < height; ++y) {
            memmove(this->f90_ownedPixels + (dst.top + y) * this->f94_ownedPitch + dst.left * 2,
                    srcSurf->f90_ownedPixels + (src.top + y) * srcSurf->f94_ownedPitch + src.left * 2,
                    static_cast<size_t>(width) * 2);
        }
        metal_bridge::textureDirty(this);
        return DD_OK;
    }
    gog_unused_function_called("FakeSurface4::Blt");
    return DDERR_GENERIC;
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
        if (!srcSurf || !srcSurf->f90_ownedPixels || (a6 & ~DDBLTFAST_WAIT))
            return DDERR_UNSUPPORTED;
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
        for (LONG row = 0; row < height; ++row) {
            memmove(this->f90_ownedPixels + (y + row) * this->f94_ownedPitch + x * 2,
                    srcSurf->f90_ownedPixels + (src.top + row) * srcSurf->f94_ownedPitch + src.left * 2,
                    static_cast<size_t>(width) * 2);
        }
        metal_bridge::textureDirty(this);
        return DD_OK;
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

HRESULT FakeSurface4::GetBltStatus(DWORD) {
    gog_unused_function_called("FakeSurface4::GetBltStatus");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetCaps(LPDDSCAPS2) {
    gog_unused_function_called("FakeSurface4::GetCaps");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetClipper(LPDIRECTDRAWCLIPPER *) {
    gog_unused_function_called("FakeSurface4::GetClipper");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetColorKey(DWORD, LPDDCOLORKEY) {
    gog_unused_function_called("FakeSurface4::GetColorKey");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetDC(HDC *) {
    gog_unused_function_called("FakeSurface4::GetDC");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetFlipStatus(DWORD) {
    gog_unused_function_called("FakeSurface4::GetFlipStatus");
    return DDERR_GENERIC;
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
    gog_unused_function_called("FakeSurface4::GetPixelFormat");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::GetSurfaceDesc(LPDDSURFACEDESC2 desc) {
    if (this->f90_ownedPixels && desc) {
        *desc = this->f14_desc;
        return DD_OK;
    }
    gog_unused_function_called("FakeSurface4::GetSurfaceDesc");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) {
    gog_unused_function_called("FakeSurface4::Initialize");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::IsLost(void) {
    return DD_OK;
}

HRESULT FakeSurface4::Lock(LPRECT pRect, LPDDSURFACEDESC2 surf, DWORD a4, HANDLE a5) {
    if (this->f90_ownedPixels) {
        if (!surf || this->f10_lockCounter) return DDERR_SURFACEBUSY;
        *surf = this->f14_desc;
        if (pRect) {
            if (pRect->left < 0 || pRect->top < 0 || pRect->right < pRect->left ||
                pRect->bottom < pRect->top ||
                pRect->right > static_cast<LONG>(this->f14_desc.dwWidth) ||
                pRect->bottom > static_cast<LONG>(this->f14_desc.dwHeight))
                return DDERR_INVALIDRECT;
            surf->dwWidth = pRect->right - pRect->left;
            surf->dwHeight = pRect->bottom - pRect->top;
            surf->lpSurface = this->f90_ownedPixels +
                pRect->top * this->f94_ownedPitch + pRect->left * 2;
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

HRESULT FakeSurface4::ReleaseDC(HDC) {
    gog_unused_function_called("FakeSurface4::ReleaseDC");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::Restore(void) {
    return DD_OK;
}

HRESULT FakeSurface4::SetClipper(LPDIRECTDRAWCLIPPER) {
    gog_unused_function_called("FakeSurface4::SetClipper");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetColorKey(DWORD, LPDDCOLORKEY) {
    gog_unused_function_called("FakeSurface4::SetColorKey");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetOverlayPosition(LONG, LONG) {
    gog_unused_function_called("FakeSurface4::SetOverlayPosition");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::SetPalette(LPDIRECTDRAWPALETTE) {
    gog_unused_function_called("FakeSurface4::SetPalette");
    return DDERR_GENERIC;
}

HRESULT FakeSurface4::Unlock(LPRECT pRect) {
    IDirectDrawSurface4 *bridgeSurface =
        metal_bridge::isEnabled() && isBump16(this->f14_desc) ? this : this->f8_orig_surf;
    metal_bridge::textureDirty(bridgeSurface, pRect ? nullptr : &this->f14_desc);
    if (this->f90_ownedPixels) {
        if (!this->f10_lockCounter) return DDERR_NOTLOCKED;
        --this->f10_lockCounter;
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
