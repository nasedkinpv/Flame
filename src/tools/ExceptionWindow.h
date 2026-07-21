//
// Created by DiaLight on 26.09.2024.
//

#ifndef FLAMETAL_EXCEPTIONWINDOW_H
#define FLAMETAL_EXCEPTIONWINDOW_H

#include <Windows.h>
#include <queue>
#include "bug_hunter.h"


class ExceptionWindow {
    std::vector<StackFrame> frames;
    bool _throwAgain = false;
public:
    explicit ExceptionWindow(std::deque<StackFrame> &frames) : frames(frames.begin(), frames.end()) {}

    BOOL show(HINSTANCE hInstance);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    static BOOL init(HINSTANCE hInstance);
    static bool registered;
};


#endif //FLAMETAL_EXCEPTIONWINDOW_H
