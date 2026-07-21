//
// Created by DiaLight on 5/17/2025.
//

#include "limit_tps.h"
#include <tools/flametal_config.h>
#include <patches/logging.h>
#include <Windows.h>


namespace {

    LARGE_INTEGER g_frequency = {};
    LONGLONG g_nextDeadline = 0;
    LONGLONG g_fraction = 0;
    int g_deadlineTps = 0;

    // present-gap probe: how much of the frame interval is the limiter's
    // wait vs. lateness past the deadline (frames that miss entirely).
    LONGLONG g_probeWait = 0;      // QPC ticks spent waiting this window
    LONGLONG g_probeLate = 0;      // QPC ticks past deadline on late frames
    int g_probeLateFrames = 0;
    int g_probeFrames = 0;

    void probeWindow(LONGLONG waitTicks, LONGLONG lateTicks) {
        g_probeWait += waitTicks;
        if (lateTicks > 0) { g_probeLate += lateTicks; ++g_probeLateFrames; }
        if (++g_probeFrames < 300) return;
        const double toUs = 1e6 / (double) g_frequency.QuadPart;
        patch::log::dbg("PERF limit_tps avg us: wait=%d late=%d late-frames=%d/300",
                        (int) (g_probeWait * toUs / g_probeFrames),
                        g_probeLateFrames ? (int) (g_probeLate * toUs / g_probeLateFrames) : 0,
                        g_probeLateFrames);
        g_probeWait = g_probeLate = 0;
        g_probeLateFrames = g_probeFrames = 0;
    }

    void resetDeadline() {
        g_nextDeadline = 0;
        g_fraction = 0;
        g_deadlineTps = 0;
    }

    void advanceDeadline(int tps) {
        g_nextDeadline += g_frequency.QuadPart / tps;
        g_fraction += g_frequency.QuadPart % tps;
        if (g_fraction >= tps) {
            g_nextDeadline += g_fraction / tps;
            g_fraction %= tps;
        }
    }

}


flametal_config::define_flame_option<int> o_limitTps(
    "flametal:limit-tps", flametal_config::OG_Config,
    "For displays with high frequency you can limit game loop time\n"
    "I don't know what fps value the dk2 developers were adjusting to\n"
    "I was comfortable with 60 (frames/ticks) per second\n"
    "use value 0 to disable limit",
    60
);

void patch::limit_tps::call() {
    int tps = *o_limitTps;
    if (tps <= 0) {
        resetDeadline();
        return;
    }
    if (!g_frequency.QuadPart && !QueryPerformanceFrequency(&g_frequency)) return;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (!g_nextDeadline || g_deadlineTps != tps) {
        g_nextDeadline = now.QuadPart;
        g_fraction = 0;
        g_deadlineTps = tps;
        advanceDeadline(tps);
    } else if (now.QuadPart >= g_nextDeadline) {
        probeWindow(0, now.QuadPart - g_nextDeadline);
        if (now.QuadPart - g_nextDeadline > g_frequency.QuadPart) {
            g_nextDeadline = now.QuadPart;
            g_fraction = 0;
        }
        do advanceDeadline(tps); while (g_nextDeadline <= now.QuadPart);
        return;  // A late frame must not be delayed to the following deadline.
    }

    const LONGLONG target = g_nextDeadline;
    const LONGLONG waitStart = now.QuadPart;
    for (;;) {
        QueryPerformanceCounter(&now);
        const LONGLONG remaining = target - now.QuadPart;
        if (remaining <= 0) break;
        const DWORD remainingMs = static_cast<DWORD>(
            remaining * 1000 / g_frequency.QuadPart);
        if (remainingMs > 1) SleepEx(remainingMs - 1, FALSE);
        else SwitchToThread();
    }
    probeWindow(now.QuadPart - waitStart, 0);
    advanceDeadline(tps);
}
