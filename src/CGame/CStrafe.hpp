#pragma once
// Auto strafe on the local player's movement state.
//
// The offsets collection gives the navigation part (nip, found by its class name among the player's parts),
// its movement state (nip+0x48 -> tof) and three Vector3 in it (tof+0x10 / +0x1C / +0x28) whose meaning is
// not established. So the meaning is measured instead of assumed: while the player moves, each of the three
// is compared with the velocity seen from the player's position over ~100 ms. The one that points the same
// way as the real movement, by a steady factor, is taken as the velocity (and the factor as its scale).
//
// Once it is known: in the air, the horizontal part of that vector follows the view. When the view yaw
// turns, the vector turns with it (at most `turn` degrees per step), optionally with a little extra speed
// (`gain`, percent). That is what strafing the mouse does through air acceleration, done on the vector.
//
// Whether the game keeps what is written is measured too: shortly after a write the vector is read back, and
// the numbers (writes, kept) are logged. Nothing is done on the ground, below `min_speed`, or before the
// velocity vector is known. The view yaw comes from the camera's view matrix (forward = -row 3 of the
// rotation, the matrix is column-major, see world_to_screen).

#include "CGameContext.hpp"
#include "CClass.hpp"
#include "CCamera.hpp"
#include "CPlayer.hpp"
#include "../CMath/CVector3.hpp"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace strafe {
    inline constexpr uint64_t kNavState = 0x48;                       // nip.lgsc -> tof
    inline constexpr uint64_t kVector[3] = {0x10, 0x1C, 0x28};        // tof.<avwy>, <acfr>, <ziok>

    // How well one vector follows the real movement.
    struct Candidate {
        int n = 0;
        double cos_sum = 0, ratio_mean = 0, ratio_m2 = 0;
        void add(double cosine, double ratio) {
            ++n;
            cos_sum += cosine;
            const double d = ratio - ratio_mean;
            ratio_mean += d / n;
            ratio_m2 += d * (ratio - ratio_mean);
        }
        double cosine() const { return n ? cos_sum / n : 0; }
        double variation() const { return n > 1 && ratio_mean > 1e-6 ? std::sqrt(ratio_m2 / (n - 1)) / ratio_mean : 1e9; }
    };

    // Correlation of two series (the player's vertical speed and one vector's y).
    struct Correlation {
        int n = 0;
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        void add(double x, double y) { ++n; sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; }
        double covariance() const { return n ? sxy / n - (sx / n) * (sy / n) : 0; }
        double variance_x() const { return n ? sxx / n - (sx / n) * (sx / n) : 0; }
        double variance_y() const { return n ? syy / n - (sy / n) * (sy / n) : 0; }
        double r() const {
            const double d = std::sqrt(variance_x() * variance_y());
            return d > 1e-9 ? covariance() / d : 0.0;
        }
        double slope() const { return variance_x() > 1e-9 ? covariance() / variance_x() : 0.0; }
    };

    struct Pose { long long t; Vector3 position; float yaw; };

    inline int g_pid = -1;
    inline uint64_t g_nip = 0, g_tof = 0;
    inline long long g_search = 0, g_log = 0, g_write_at = 0;
    inline std::deque<Pose> g_poses;
    inline Candidate g_candidates[3];
    inline int g_locked = -1;
    // Set by the bunnyhop: measure the vectors (g_request) and turn the velocity with the view (g_turn), even
    // with the Auto strafe switch off.
    inline bool g_request = false, g_turn = false;
    // The vertical speed: which vector's y follows the player's real vertical movement, and by what factor.
    inline Correlation g_vertical[3];
    inline int g_vert = -1;
    inline double g_vert_scale = 1.0;
    inline double g_scale = 1.0;
    inline float g_written_yaw = 0.f;
    inline bool g_have_yaw = false;
    inline int g_writes = 0, g_kept = 0;
    inline float g_last_written[3]{};
    inline long long g_check_at = 0;
    inline int g_check_vector = -1;

    inline float wrap_degrees(float d) {
        while (d > 180.f) d -= 360.f;
        while (d < -180.f) d += 360.f;
        return d;
    }

    // The view's yaw in degrees (Unity: 0 along +Z, growing toward +X), false when the matrix looks vertical.
    inline bool view_yaw(const matrix& view, float& yaw) {
        const float* a = reinterpret_cast<const float*>(&view);
        const float fx = -a[2], fz = -a[10];
        if (!std::isfinite(fx) || !std::isfinite(fz) || fx * fx + fz * fz < 1e-6f) return false;
        yaw = std::atan2(fx, fz) * 57.29578f;
        return true;
    }

    inline void forget() {
        g_nip = g_tof = 0;
        g_poses.clear();
        for (auto& c : g_candidates) c = Candidate{};
        g_locked = -1;
        for (auto& c : g_vertical) c = Correlation{};
        g_vert = -1;
        g_have_yaw = false;
        g_writes = g_kept = 0;
        g_check_vector = -1;
    }

    // The local player's navigation state: nip among its parts, tof behind it.
    inline bool find_state(uint64_t local) {
        if (g_tof && klass::is_named(g_tof, "tof")) return true;
        g_tof = 0;
        const long long now = diag::now_ms();
        if (now - g_search < 1000) return false;
        g_search = now;
        std::vector<uint64_t> parts;
        game_camera::collect_parts(local, parts);
        for (uint64_t part : parts) {
            if (!klass::is_named(part, "nip")) continue;
            const uint64_t tof = rpm<uint64_t>(part + kNavState);
            if (!plausible(tof) || !klass::is_named(tof, "tof")) continue;
            if (g_nip != part) diag::log("STRAFE", "navigation part nip 0x%llx, movement state tof 0x%llx", (unsigned long long)part, (unsigned long long)tof);
            g_nip = part;
            g_tof = tof;
            return true;
        }
        static long long miss = 0;
        if (diag::every(miss, 5000)) diag::log("STRAFE", "navigation part (nip) not found among %zu parts of the local player", parts.size());
        return false;
    }

    inline bool read_vector(int i, float out[3]) {
        return g_tof && mem_read(g_tof + kVector[i], out, 12) && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }

    // An upward speed (m/s) into the vector whose y was found to follow the vertical movement. False when there
    // is none (not measured yet): the bunnyhop then cannot jump this way.
    inline bool kick(float speed) {
        if (g_vert < 0 || !g_tof) return false;
        float v[3];
        if (!read_vector(g_vert, v)) return false;
        v[1] = float(speed * g_vert_scale);
        return mem_write(g_tof + kVector[g_vert] + 4, &v[1], 4);
    }

    // One call per frame with the local player, its position and the camera's view matrix.
    inline void tick(uint64_t local, const Vector3& position, const matrix* view) {
        if (g_pid != proc::pid) { g_pid = proc::pid; forget(); }
        if (!(cfg::strafe::enabled || g_request) || !local || !view || proc::pid <= 0) {
            if (g_tof) forget();
            return;
        }
        if (!find_state(local)) return;

        const long long now = diag::now_ms();
        float yaw = 0.f;
        const bool yaw_ok = view_yaw(*view, yaw);
        if (!g_poses.empty() && now - g_poses.back().t > 300) g_poses.clear();   // a pause, a respawn
        if (g_poses.empty() || now - g_poses.back().t >= 8) g_poses.push_back({now, position, yaw});
        while (g_poses.size() > 2 && now - g_poses[1].t > 100) g_poses.pop_front();
        if (g_poses.size() < 2) return;

        // Velocity over the window (about 100 ms).
        const Pose& old = g_poses.front();
        const float dt = float(now - old.t) / 1000.f;
        if (dt < 0.05f) return;
        const Vector3 velocity((position.x - old.position.x) / dt, (position.y - old.position.y) / dt, (position.z - old.position.z) / dt);
        const float speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        if (!std::isfinite(speed) || speed > 60.f) { g_poses.clear(); return; }   // teleport
        const bool airborne = std::fabs(velocity.y) > 0.6f;

        float v[3][3]{};
        bool readable[3];
        for (int i = 0; i < 3; ++i) readable[i] = read_vector(i, v[i]);

        // Which y follows the vertical movement (for the bunnyhop's jump): while really rising or falling.
        float vy_short = velocity.y;   // over the last ~40 ms, less lag than the 100 ms window
        for (const Pose& pose : g_poses) {
            if (now - pose.t > 45) continue;
            if (now - pose.t >= 20) vy_short = (position.y - pose.position.y) / (float(now - pose.t) / 1000.f);
            break;
        }
        if (g_vert < 0 && std::fabs(vy_short) > 1.0f) {
            for (int i = 0; i < 3; ++i)
                if (readable[i]) g_vertical[i].add(vy_short, v[i][1]);
            // The speed seen from positions is a ~50 ms average, so the match is loose; the others do not match at all.
            int best = -1;
            for (int i = 0; i < 3; ++i) {
                const Correlation& c = g_vertical[i];
                if (c.n < 15 || c.r() < 0.75 || c.slope() < 0.05 || c.slope() > 20.0) continue;
                if (best < 0 || c.r() > g_vertical[best].r()) best = i;
            }
            if (best >= 0) {
                g_vert = best;
                g_vert_scale = g_vertical[best].slope();
                diag::log("STRAFE", "vertical speed is tof+0x%llx y: factor %.3f (correlation %.3f)", (unsigned long long)kVector[best], g_vert_scale, g_vertical[best].r());
            }
        }

        // Did the game keep our last write? (read back a little after it)
        if (g_check_vector >= 0 && now - g_check_at >= 12) {
            if (readable[g_check_vector]) {
                const float* w = g_last_written;
                const float* c = v[g_check_vector];
                // kept when it is still near what we wrote, rather than what the game computes next
                if (std::fabs(c[0] - w[0]) < 0.02f * std::max(1.f, std::fabs(w[0])) && std::fabs(c[2] - w[2]) < 0.02f * std::max(1.f, std::fabs(w[2]))) ++g_kept;
            }
            g_check_vector = -1;
        }

        if (g_locked < 0) {
            if (speed < 3.f) return;
            for (int i = 0; i < 3; ++i) {
                if (!readable[i]) continue;
                const double mag = std::sqrt(double(v[i][0]) * v[i][0] + double(v[i][2]) * v[i][2]);
                if (mag < 1e-4) continue;
                g_candidates[i].add((v[i][0] * velocity.x + v[i][2] * velocity.z) / (mag * speed), mag / speed);
            }
            for (int i = 0; i < 3; ++i) {
                const Candidate& c = g_candidates[i];
                if (c.n < 40 || c.cosine() < 0.9 || c.variation() > 0.35) continue;
                // the best one: the highest cosine among those that qualify
                int best = i;
                for (int j = 0; j < 3; ++j) if (g_candidates[j].n >= 40 && g_candidates[j].cosine() > g_candidates[best].cosine() && g_candidates[j].variation() <= 0.35) best = j;
                g_locked = best;
                g_scale = g_candidates[best].ratio_mean;
                diag::log("STRAFE", "velocity is tof+0x%llx: direction match %.3f, factor %.3f (variation %.2f)", (unsigned long long)kVector[best],
                          g_candidates[best].cosine(), g_scale, g_candidates[best].variation());
                break;
            }
            if (g_locked < 0 && cfg::strafe::log && diag::every(g_log, 3000))
                diag::log("STRAFE", "measuring: samples %d/%d/%d, direction match %.2f/%.2f/%.2f, speed %.1f m/s", g_candidates[0].n, g_candidates[1].n,
                          g_candidates[2].n, g_candidates[0].cosine(), g_candidates[1].cosine(), g_candidates[2].cosine(), speed);
            return;
        }

        if (!(cfg::strafe::enabled || g_turn)) return;   // measuring only
        if (!yaw_ok || !readable[g_locked]) return;
        if (!g_have_yaw) { g_written_yaw = yaw; g_have_yaw = true; return; }
        if (!airborne || speed < cfg::strafe::min_speed) { g_written_yaw = yaw; return; }   // on the ground: only follow the view

        // The view turned since the last step: the velocity turns with it. (One write at a time: the next one
        // waits until the read-back of this one is done, so "kept" means what it says.)
        if (g_check_vector >= 0) return;
        const float dyaw = wrap_degrees(yaw - g_written_yaw);
        if (std::fabs(dyaw) < 0.05f || now - g_write_at < 6) return;
        const float turn = std::clamp(dyaw, -cfg::strafe::turn, cfg::strafe::turn);
        g_written_yaw = yaw;
        const float rad = turn * 0.01745329f, c = std::cos(rad), s = std::sin(rad);
        const float boost = 1.f + std::max(cfg::strafe::gain, 0.f) * 0.01f;
        const float* now_v = v[g_locked];
        float w[3] = {(now_v[0] * c + now_v[2] * s) * boost, now_v[1], (now_v[2] * c - now_v[0] * s) * boost};
        if (!mem_write(g_tof + kVector[g_locked], w, sizeof(w))) return;
        std::memcpy(g_last_written, w, sizeof(w));
        g_check_vector = g_locked;
        g_check_at = now;
        g_write_at = now;
        ++g_writes;
        if (cfg::strafe::log && diag::every(g_log, 3000))
            diag::log("STRAFE", "air, %.1f m/s, view turned %.2f deg: %d writes, %d still there after 12 ms", speed, dyaw, g_writes, g_kept);
    }
}
