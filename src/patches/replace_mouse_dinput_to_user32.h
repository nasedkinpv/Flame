//
// Created by DiaLight on 20.07.2024.
//

#ifndef FLAMETAL_REPLACE_MOUSE_DINPUT_TO_USER32_H
#define FLAMETAL_REPLACE_MOUSE_DINPUT_TO_USER32_H

#include <Windows.h>

namespace patch::replace_mouse_dinput_to_user32 {

    extern bool enabled;
    void emulate_dinput_from_user32(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
    void handle_mouse_move(HWND hWnd, POINT pos);
    void inject_metal_pointer(float normalizedX, float normalizedY, LONG deltaX, LONG deltaY);
    void inject_metal_button(DWORD button, DWORD value);
    void inject_metal_key(DWORD dikScancode, bool pressed);
    void release_handled_dinput_actions();

}


#endif //FLAMETAL_REPLACE_MOUSE_DINPUT_TO_USER32_H
