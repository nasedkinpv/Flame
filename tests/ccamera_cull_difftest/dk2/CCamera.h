#pragma once
namespace dk2 {
struct CCamera {
    int fD92 = 0;
    int endTime = 0;
    int minZoomLevel = 0;
    int maxZoomLevel = 0;
    int curZoomLevel = 0;
    void zoomRel_449CA0(int delta);
};
}
