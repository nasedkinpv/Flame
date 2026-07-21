//
// Created by DiaLight on 21.01.2023.
//
#include <ddraw.h>
#include <fake/FakeDirectDraw1.h>
#include <fake/FakeDirectDraw2.h>
#include <fake/FakeDirectDraw4.h>
#include <fake/FakeD3D3.h>
#include <gog_globals.h>
#include <gog_debug.h>
#include <metal_bridge/MetalBridgeProducer.h>
#include <mutex>

using namespace gog;

namespace {

using DirectDrawCreateProc = HRESULT (WINAPI *)(GUID FAR *, LPDIRECTDRAW FAR *, IUnknown FAR *);
using DirectDrawEnumerateAProc = HRESULT (WINAPI *)(LPDDENUMCALLBACKA, LPVOID);

std::once_flag g_realDirectDrawOnce;
DirectDrawCreateProc g_realDirectDrawCreate = nullptr;
DirectDrawEnumerateAProc g_realDirectDrawEnumerateA = nullptr;

void loadRealDirectDraw() {
    HMODULE ddraw = LoadLibraryA("DDRAW.dll");
    if (!ddraw) return;
    g_realDirectDrawCreate = reinterpret_cast<DirectDrawCreateProc>(
        GetProcAddress(ddraw, "DirectDrawCreate"));
    g_realDirectDrawEnumerateA = reinterpret_cast<DirectDrawEnumerateAProc>(
        GetProcAddress(ddraw, "DirectDrawEnumerateA"));
}

}  // namespace

HRESULT WINAPI real_DirectDrawCreate(GUID FAR *lpGUID, LPDIRECTDRAW FAR *lplpDD,
                                     IUnknown FAR *pUnkOuter) {
    std::call_once(g_realDirectDrawOnce, loadRealDirectDraw);
    if (!g_realDirectDrawCreate) return DDERR_GENERIC;
    return g_realDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}

HRESULT WINAPI real_DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext) {
    std::call_once(g_realDirectDrawOnce, loadRealDirectDraw);
    if (!g_realDirectDrawEnumerateA) return DDERR_GENERIC;
    return g_realDirectDrawEnumerateA(lpCallback, lpContext);
}

//#pragma comment(linker, "/EXPORT:DirectDrawCreate@12=DirectDrawCreate")
extern "C" HRESULT WINAPI fake_DirectDrawCreate(GUID FAR *lpGUID, LPDIRECTDRAW FAR *lplpDD, IUnknown FAR *pUnkOuter) {
    gog_debug("Creating DX device");
    if (lpGUID != nullptr) gog_assert_failed("FakeDirectDrawCreate:1517");
    if (lplpDD == nullptr) gog_assert_failed("FakeDirectDrawCreate:1518");
    if (!FakeDirectDraw1::instance) {
        if (!metal_bridge::headlessDirectDrawEnabled()) {
            LPDIRECTDRAW lpDD;
            HRESULT hr = real_DirectDrawCreate(NULL, &lpDD, NULL);
            if (FAILED(hr))
                return hr;
            hr = lpDD->QueryInterface(IID_IDirectDraw4, (LPVOID *) &orig::pIDirectDraw4);
            lpDD->Release();
            if (FAILED(hr))
                return hr;
        } else {
            gog_debug("Metal bridge: using CPU-backed headless DirectDraw");
        }
        FakeDirectDraw1::instance = new FakeDirectDraw1();
        FakeDirectDraw2::instance = new FakeDirectDraw2();
        FakeDirectDraw4::instance = new FakeDirectDraw4();
        FakeD3D3::instance = new FakeD3D3();
    }
    *lplpDD = FakeDirectDraw1::instance;
    return 0;
}

//#pragma comment(linker, "/EXPORT:DirectDrawEnumerateA@8=DirectDrawEnumerateA")
extern "C" HRESULT WINAPI fake_DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext) {
    lpCallback(NULL, (LPSTR) "GOG HW Patch", (LPSTR) "GOG HW Patch", lpContext);
    return 0;
}

#pragma comment(linker, "/EXPORT:Flametal_DirectDrawCreate@12=Flametal_DirectDrawCreate")
extern "C" HRESULT WINAPI Flametal_DirectDrawCreate(GUID FAR *lpGUID,
                                                     LPDIRECTDRAW FAR *lplpDD,
                                                     IUnknown FAR *pUnkOuter) {
    return fake_DirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}

#pragma comment(linker, "/EXPORT:Flametal_DirectDrawEnumerateA@8=Flametal_DirectDrawEnumerateA")
extern "C" HRESULT WINAPI Flametal_DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback,
                                                         LPVOID lpContext) {
    return fake_DirectDrawEnumerateA(lpCallback, lpContext);
}
