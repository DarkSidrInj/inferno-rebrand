#pragma once

#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <unistd.h>

namespace diag {
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline bool every(long long& last, long long interval_ms) {
    const long long now = now_ms();
    if (now - last < interval_ms) return false;
    last = now;
    return true;
}

inline void log(const char* level, const char* format, ...) {
    // A disconnected/slow adb shell must never block the overlay thread.
    // STDOUT is configured O_NONBLOCK by main(); drop records when full.
    char line[1536];
    int used = std::snprintf(line, sizeof(line), "[bycmd][%s] ", level);
    if (used < 0) return;
    if (used >= static_cast<int>(sizeof(line))) used = sizeof(line) - 1;
    va_list args;
    va_start(args, format);
    const int added = std::vsnprintf(line + used, sizeof(line) - size_t(used), format, args);
    va_end(args);
    if (added > 0) used += std::min<int>(added, int(sizeof(line) - size_t(used) - 1));
    if (used < static_cast<int>(sizeof(line)) - 1) line[used++] = '\n';
    (void)::write(STDOUT_FILENO, line, size_t(used));
}
}
