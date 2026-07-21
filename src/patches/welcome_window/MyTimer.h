//
// Created by DiaLight on 11/29/2025.
//

#ifndef FLAMETAL_MYTIMER_H
#define FLAMETAL_MYTIMER_H

#include <Windows.h>

struct MyTimer {

    LARGE_INTEGER s_frequency{};
    BOOL s_use_qpc = FALSE;
    HANDLE hTimer;
    MyTimer();
    ~MyTimer() {
        CloseHandle(hTimer);
    }

    [[nodiscard]] time_t now_ms() const;
    void sleep(time_t ms) const;

};


#endif // FLAMETAL_MYTIMER_H
