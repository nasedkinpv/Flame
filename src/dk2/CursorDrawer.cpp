#include <dk2/CursorDrawer.h>
#include <dk2_functions.h>
#include <metal_bridge/MetalBridgeProducer.h>

#include <cstring>

namespace dk2 {

bool CursorDrawer::drawCursorToScreenSurf() {
    if (!pScreen[0]) {
        gog::metal_bridge::hideCursor();
        return true;
    }

    if (gog::metal_bridge::isEnabled() && cursorSize.x > 0 && cursorSize.y > 0) {
        IDirectDrawSurface *surface = cursorSurf[0].ddSurfEx.dd_surf.dd_surface;
        DWORD colorKey = 0;
        std::memcpy(&colorKey, &cursorSurf[0].ddSurfEx.dd_surf.fld10_00,
                    sizeof(colorKey));
        const LONG hotspotX = -mouseToCursorOffs.x;
        const LONG hotspotY = -mouseToCursorOffs.y;
        if (gog::metal_bridge::cursor(surface,
                                      static_cast<DWORD>(cursorSize.x),
                                      static_cast<DWORD>(cursorSize.y),
                                      colorKey,
                                      mousePos.x, mousePos.y,
                                      hotspotX, hotspotY)) {
            return true;
        }
    }

    int status = 0;
    static_MyDdSurfaceEx_BltWait(&status, pScreen[0],
                                 cursorAabbScreenCut[0].minX,
                                 cursorAabbScreenCut[0].minY,
                                 &cursorSurf[0].ddSurfEx,
                                 &cursorSizeAabb[0], 1);
    return status >= 0;
}

}  // namespace dk2
