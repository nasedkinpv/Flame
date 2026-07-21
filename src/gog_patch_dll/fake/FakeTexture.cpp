//
// Created by DiaLight on 20.01.2023.
//
#include <fake/FakeTexture.h>
#include <gog_debug.h>
#include <metal_bridge/MetalBridgeProducer.h>

using namespace gog;

namespace {
volatile LONG nextBridgeTextureId = 0;
}

FakeTexture::FakeTexture(IDirect3DTexture2 *orig_tex, IDirectDrawSurface4 *orig_surf)
    : f8_orig_tex(orig_tex), fC_orig_surf(orig_surf),
      f10_bridge_id(static_cast<DWORD>(InterlockedIncrement(&nextBridgeTextureId))) {
    if (fC_orig_surf) fC_orig_surf->AddRef();
}

HRESULT FakeTexture::QueryInterface(const IID &riid, LPVOID *ppvObj) {
    gog_unused_function_called("FakeTexture::QueryInterface");
    return DDERR_GENERIC;
}

ULONG FakeTexture::Release(void) {
    if (--this->refs != 0)
        return this->refs;
    metal_bridge::textureReleased(this->f10_bridge_id, this->fC_orig_surf);
    if (this->f8_orig_tex) {
        this->f8_orig_tex->Release();
        this->f8_orig_tex = nullptr;
    }
    if (this->fC_orig_surf) {
        this->fC_orig_surf->Release();
        this->fC_orig_surf = nullptr;
    }
    operator delete(this);
    return 0;
}

HRESULT FakeTexture::GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE) {
    gog_unused_function_called("FakeTexture::GetHandle");
    return DDERR_GENERIC;
}

HRESULT FakeTexture::PaletteChanged(DWORD, DWORD) {
    gog_unused_function_called("FakeTexture::PaletteChanged");
    return DDERR_GENERIC;
}

HRESULT FakeTexture::Load(LPDIRECT3DTEXTURE2) {
    gog_unused_function_called("FakeTexture::Load");
    return DDERR_GENERIC;
}
