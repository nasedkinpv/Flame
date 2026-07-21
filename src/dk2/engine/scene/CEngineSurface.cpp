#include "dk2_functions.h"
#include "dk2/CEngineSurface.h"
#include "dk2/CEngineSurfaceBase.h"
#include <metal_bridge/MetalBridgeProducer.h>


void dk2::CEngineSurface::destructor() {
    *(void **) this = CEngineSurface::vftable;
    gog::metal_bridge::surfaceReleased(this);
    MyHeap_free(this->pixels);
    *(void **) this = CEngineSurfaceBase::vftable;
}
