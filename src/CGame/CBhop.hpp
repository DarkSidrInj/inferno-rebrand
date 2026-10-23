#pragma once
// Bunnyhop: hold the game's jump button, turn the camera, and the player hops on every landing.
//
// What the cheat can see is the screen, not the game's input. So:
//   - the jump button is told once ("Set jump button": the next finger down is taken as its place);
//   - "jump held" = a finger of yours is on that place (multi-touch, read from the touch screen);
//   - a landing is read from the player's height over time (falling, then standing);
//   - on a landing while the jump is held, the jump is done again: an upward speed is written into the
//     vertical part of the movement vector the strafe module found (no touches of our own are made).
// While the jump is held the strafe module turns the movement vector with the camera (no extra setting).

#include "CGameContext.hpp"
#include "CStrafe.hpp"
#include "../CMath/CVector3.hpp"
#include "../CConfigs/CConfig.hpp"
#include "Android_touch/Touch.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <thread>

namespace bhop {
    struct Pose { long long t; float y; };

    inline std::atomic<bool> g_calibrating{false};
    inline long long g_calibration_start = 0;
    inline bool g_saw_empty = false;

    inline bool g_held = false;
    inline int g_jumps = 0;
    inline std::deque<Pose> g_poses;
    inline long long g_last_air = 0, g_last_jump = 0, g_log = 0;
    inline float g_prev_vy = 0.f, g_peak_vy = 5.5f;   // the jump speed seen from your own jumps
    inline int g_pid = -1;

    inline bool jump_button_set() { return cfg::bhop::jump_x >= 0.f && cfg::bhop::jump_y >= 0.f; }

    inline void start_calibration() {
        g_calibrating = true;
        g_calibration_start = diag::now_ms();
        g_saw_empty = false;
    }

    // One call per frame, whatever else is on: waits for the finger that shows where the jump button is.
    inline void calibration_step() {
        if (!g_calibrating) return;
        const long long now = diag::now_ms();
        if (now - g_calibration_start > 20000) { g_calibrating = false; diag::log("BHOP", "jump button: nothing tapped, cancelled"); return; }
        touch::Finger f[8];
        const int n = touch::fingers(f, 8);
        if (n == 0) { g_saw_empty = true; return; }
        if (!g_saw_empty) return;   // the finger that pressed the menu button is still down
        cfg::bhop::jump_x = f[0].x;
        cfg::bhop::jump_y = f[0].y;
        g_calibrating = false;
        diag::log("BHOP", "jump button set at (%.0f, %.0f)", cfg::bhop::jump_x, cfg::bhop::jump_y);
    }

    inline bool finger_on_button() {
        if (!jump_button_set()) return false;
        touch::Finger f[10];
        const int n = touch::fingers(f, 10);
        const float r2 = cfg::bhop::radius * cfg::bhop::radius;
        for (int i = 0; i < n; ++i) {
            const float dx = f[i].x - cfg::bhop::jump_x, dy = f[i].y - cfg::bhop::jump_y;
            if (dx * dx + dy * dy <= r2) return true;
        }
        return false;
    }

    inline void release() {
        g_held = false;
        strafe::g_request = strafe::g_turn = false;
        g_poses.clear();
    }

    inline int g_missed = 0;   // landings with the jump held where the vertical speed could not be written

    inline void press_jump(long long now) {
        g_last_jump = now;
        if (strafe::kick(g_peak_vy)) {
            ++g_jumps;
            if (cfg::bhop::log) diag::log("BHOP", "jump %d: vertical speed %.1f written", g_jumps, g_peak_vy);
        } else {
            ++g_missed;
        }
    }

    // One call per frame with the local player and its position.
    inline void tick(uint64_t local, const Vector3& position) {
        if (g_pid != proc::pid) { g_pid = proc::pid; g_poses.clear(); g_jumps = 0; }
        const long long now = diag::now_ms();
        if (!cfg::bhop::enabled || !local || proc::pid <= 0) { release(); return; }

        g_held = finger_on_button();
        strafe::g_request = true;                              // the vectors are measured as soon as you move
        strafe::g_turn = g_held && cfg::bhop::strafe;          // and they turn with the view while the jump is held

        // height over ~40 ms
        if (g_poses.empty() || now - g_poses.back().t >= 6) g_poses.push_back({now, position.y});
        while (g_poses.size() > 2 && now - g_poses[1].t > 40) g_poses.pop_front();
        if (g_poses.size() < 2) return;
        const float dt = float(now - g_poses.front().t) / 1000.f;
        if (dt < 0.02f) return;
        const float vy = (position.y - g_poses.front().y) / dt;
        if (!std::isfinite(vy) || std::fabs(vy) > 60.f) { g_poses.clear(); return; }   // teleport, respawn

        if (std::fabs(vy) > 0.9f) g_last_air = now;
        if (vy > 2.f && vy < 12.f) g_peak_vy = g_peak_vy * 0.7f + vy * 0.3f;     // learn the jump speed from your own jumps
        const bool landed = g_prev_vy < -1.f && std::fabs(vy) < 0.5f;             // falling, then not
        const bool standing = std::fabs(vy) < 0.35f && now - g_last_air >= 25;
        g_prev_vy = vy;

        if (g_held && (landed || standing) && now - g_last_jump >= 220) press_jump(now);

        if (diag::every(g_log, 3000) && cfg::bhop::log)
            diag::log("BHOP", "held %d, vy %.1f, jumps %d (not written %d), jump speed %.1f, vertical vector %s", g_held ? 1 : 0, vy, g_jumps, g_missed,
                      g_peak_vy, strafe::g_vert >= 0 ? "found" : "not measured yet");
    }
}
