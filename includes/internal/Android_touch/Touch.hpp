#pragma once
#include <cstdint>

namespace touch {
    bool init(int32_t _screen_w, int32_t _screen_h, uint8_t _orientation);
    void update(int32_t _screen_w, int32_t _screen_h, uint8_t _orientation);
    void updateOrientation(uint8_t _orientation);
    void shutdown();

    // Every finger on the screen right now, in display pixels (multi-touch).
    struct Finger { float x, y; };
    int fingers(Finger *out, int max);
}
