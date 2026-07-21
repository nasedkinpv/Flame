//
// Created by DiaLight on 10/5/2025.
//

#include "remember_window_location_and_size.h"
#include "tools/flametal_config.h"


flametal_config::define_flame_option<bool> o_no_initial_size(
    "flametal:no-initial-size", flametal_config::OG_Config,
    "Disable initial automatic window resizing\n"
    "Used only in windowed mode\n",
    false
);
flametal_config::define_flame_option<bool> o_lock_window_size(
    "flametal:lock-window-size", flametal_config::OG_Config,
    "Keep the first native window size across screen mode changes\n"
    "Used only in windowed mode\n",
    false
);

namespace {
    POINT window_pos = {50, 50};
    POINT window_size = {0, 0};
    bool ignore_size = true;

    void initWindowSize(uint32_t w, uint32_t h) {
        if(window_size.x != 0 || window_size.y != 0) return;
        if(o_no_initial_size.get()) return;
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);

        int height;
        int width;
        if(screenHeight < screenWidth) {
            height = screenHeight * 5 / 6;
            width = height * 12 / 9;
        } else {
            width = screenWidth * 5 / 6;
            height = width * 9 / 12;
        }
        window_size = {width, height};
    }
}

bool patch::remember_window_location_and_size::window_proc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    switch(Msg) {
    case WM_DESTROY: {
        ignore_size = true;
        break;
    }
    case WM_MOVE: {
        RECT winRect;
        GetWindowRect(hWnd, &winRect);
        window_pos = {winRect.left, winRect.top};

        break;
    }
    case WM_SIZE: {
        bool has_size = window_size.x != 0 && window_size.y != 0;
        if(!ignore_size && (!o_lock_window_size.get() || !has_size)) {
            RECT winRect;
            GetWindowRect(hWnd, &winRect);
            window_size = {winRect.right - winRect.left, winRect.bottom - winRect.top};
        }
        break;
    }
    }
    return false;
}
void patch::remember_window_location_and_size::patchWinLoc(int &xPos, int &yPos) {
    xPos = window_pos.x;
    yPos = window_pos.y;
}
void patch::remember_window_location_and_size::resizeWindow(HWND hWnd, uint32_t w, uint32_t h) {
    initWindowSize(w, h);
    if(window_size.x != 0 && window_size.y != 0) {
        SetWindowPos(hWnd, NULL, 0, 0, window_size.x, window_size.y, SWP_NOMOVE | SWP_NOZORDER);
    }
    ignore_size = false;
}
