//
// Created by DiaLight on 10/9/2025.
//

#ifndef FLAMETAL_SHOW_WIREFRAME_H
#define FLAMETAL_SHOW_WIREFRAME_H

#include <Windows.h>

typedef struct DIDEVICEOBJECTDATA DIDEVICEOBJECTDATA;

namespace patch::show_wireframe {

    void onKeyboard(DIDEVICEOBJECTDATA *data);
    void window_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

}


#endif // FLAMETAL_SHOW_WIREFRAME_H
