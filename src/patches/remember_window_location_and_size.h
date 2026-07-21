//
// Created by DiaLight on 10/5/2025.
//

#ifndef FLAMETAL_REMEMBER_WINDOW_LOCATION_AND_SIZE_H
#define FLAMETAL_REMEMBER_WINDOW_LOCATION_AND_SIZE_H

#include <Windows.h>
#include <cstdint>


namespace patch::remember_window_location_and_size {
    bool window_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
    void patchWinLoc(int &xPos, int &yPos);
    void resizeWindow(HWND hWnd, uint32_t w, uint32_t h);
}


#endif // FLAMETAL_REMEMBER_WINDOW_LOCATION_AND_SIZE_H
