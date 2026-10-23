#pragma once
// Anti-aim, ported from the legacy code (vintex offer(): rotation written straight into Unity's
// transform data, TransformEntry = position, rotation, scale).
//
// Only the character model turns. The legacy targets map to the aim controller's pivots
// (ios+0x50 / +0x58) in this build, and the camera follows those, so they are not touched. The
// model root (jgi.<gdnv>, jgi+0x20) carries the body mesh, bones and hitboxes and nothing the
// camera reads.
//
// The game may re-pose the model every frame: the rotation is then added on top of the game's
// each frame; when the game leaves it alone, on top of the last base, so it never compounds.
// Which of the two happens is logged ("game rewrites" / "holds").
// Legacy mode keeps the original behaviour: four degenerate quaternions (w = 0) per tick.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CPlayer.hpp"
#include "CSkeleton.hpp"
#include "../CConfigs/CConfig.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace antiaim {
    struct Target {
        uint64_t native{};
        int index_offset{-1};
        Vector4 base{};          // rotation the game wants, without ours
        Vector4 written{};
        bool wrote{};
        int held{}, rewritten{};
    };

    inline int g_pid = -1;
    inline Target g_model;
    inline long long g_bound_at = 0, g_log = 0, g_start = 0;
    inline unsigned g_jitter_flip = 0;

    inline bool same(const Vector4& a, const Vector4& b) {
        return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
               std::fabs(a.z - b.z) < 1e-5f && std::fabs(a.w - b.w) < 1e-5f;
    }

    inline Vector4 axis_angle(float x, float y, float z, float degrees) {
        const float h = degrees * 0.00872664626f;
        const float s = std::sin(h);
        return Vector4(x * s, y * s, z * s, std::cos(h));
    }

    inline Vector4 degenerate() {
        auto r = [] { return float((std::rand() % 200000) - 100000) * 0.01f; };
        return Vector4(r(), r(), r(), 0.f);
    }

    // Address of the target's local entry, or 0.
    inline uint64_t entry(const Target& t) {
        const auto& l = player::g_transform_layout;
        if (!t.native || l.pid != proc::pid || l.data_offset < 0 || t.index_offset < 0) return 0;
        const uint64_t data = rpm<uint64_t>(t.native + uint64_t(l.data_offset));
        const int index = rpm<int>(t.native + uint64_t(t.index_offset));
        const uint64_t entries = plausible(data) ? rpm<uint64_t>(data + uint64_t(l.entries_offset)) : 0;
        if (!plausible(entries) || index < 0 || index > skeleton::kMaxIndex) return 0;
        return entries + uint64_t(index) * sizeof(player::TransformEntry);
    }

    // The local model's root transform and the index offset that places it on the local player.
    inline bool bind(uint64_t local, const Vector3& feet) {
        const long long now = diag::now_ms();
        if (g_model.index_offset >= 0 && now - g_bound_at < 2000) return true;
        g_bound_at = now;
        const uint64_t model = skeleton::find_model(local);
        const uint64_t managed = plausible(model) ? rpm<uint64_t>(model + v100::model_transform) : 0;
        const uint64_t native = plausible(managed) ? rpm<uint64_t>(managed + v100::unity_cached_ptr) : 0;
        if (!plausible(native)) return false;
        if (native == g_model.native && g_model.index_offset >= 0) return true;
        g_model = Target{};
        g_model.native = native;
        std::vector<int> offsets;
        if (skeleton::g_index_pid == proc::pid && skeleton::g_index_offset >= 0) offsets.push_back(skeleton::g_index_offset);
        for (int off : {0x30, player::g_transform_layout.index_offset, 0x40})
            if (off >= 0 && std::find(offsets.begin(), offsets.end(), off) == offsets.end()) offsets.push_back(off);
        for (int off : offsets) {
            Vector3 pos{}; Vector4 rot{};
            if (!player::decode_transform_pose(native, pos, rot, off)) continue;
            const float d = std::sqrt((pos.x - feet.x) * (pos.x - feet.x) + (pos.y - feet.y) * (pos.y - feet.y) +
                                      (pos.z - feet.z) * (pos.z - feet.z));
            if (d > 3.f) continue;
            g_model.index_offset = off;
            diag::log("AA", "character model transform native=0x%llx index +0x%x (%.2f m from the feet)",
                      (unsigned long long)native, off, d);
            return true;
        }
        diag::log("AA", "character model transform 0x%llx: no index offset places it on the player", (unsigned long long)native);
        return false;
    }

    // The rotation added on top of the game's.
    inline Vector4 delta(long long now) {
        float yaw = 0.f;
        switch (cfg::antiaim::yaw_mode) {
            case 1: yaw = std::fmod(float(now - g_start) * 0.001f * cfg::antiaim::spin_speed, 360.f); break;
            case 2: yaw = (g_jitter_flip++ & 1) ? cfg::antiaim::jitter : -cfg::antiaim::jitter; break;
            case 3: yaw = 180.f; break;
            default: break;
        }
        float pitch = 0.f;
        if (cfg::antiaim::pitch_mode == 1) pitch = 89.f;        // Unity: +X tips forward
        else if (cfg::antiaim::pitch_mode == 2) pitch = -89.f;
        return player::quaternion_multiply(axis_angle(0.f, 1.f, 0.f, yaw), axis_angle(1.f, 0.f, 0.f, pitch));
    }

    // Puts back what the game had when it does not re-pose the model itself.
    inline void release() {
        const uint64_t at = entry(g_model);
        if (at && g_model.wrote && g_model.held > g_model.rewritten) wpm<Vector4>(at + 0x10, g_model.base);
        g_model.wrote = false;
        g_model.held = g_model.rewritten = 0;
    }

    inline void tick(uint64_t local, const Vector3& feet) {
        if (g_pid != proc::pid) { g_pid = proc::pid; g_model = Target{}; }
        if (!cfg::antiaim::enabled || !local) { release(); return; }
        const uint64_t before = g_model.native;
        if (!bind(local, feet)) return;
        if (g_model.native != before) g_model.wrote = false;  // respawn: new model
        const long long now = diag::now_ms();
        if (!g_start) g_start = now;

        Target& t = g_model;
        const uint64_t at = entry(t);
        if (!at) return;
        const Vector4 current = rpm<Vector4>(at + 0x10);
        if (t.wrote && same(current, t.written)) ++t.held;      // the game left our value: keep the old base
        else { t.base = current; if (t.wrote) ++t.rewritten; }  // the game posed it again this frame
        if (cfg::antiaim::yaw_mode == 0) {
            Vector4 q{};
            for (int k = 0; k < 4; ++k) { q = degenerate(); wpm<Vector4>(at + 0x10, q); }
            t.written = q;
        } else {
            Vector4 q = player::quaternion_multiply(t.base, delta(now));
            if (!player::normalize_quaternion(q)) return;
            wpm<Vector4>(at + 0x10, q);
            t.written = q;
        }
        t.wrote = true;

        if (diag::every(g_log, 3000))
            diag::log("AA", "model: %s (held %d, rewritten %d) base=(%.3f %.3f %.3f %.3f)",
                      t.held + t.rewritten == 0 ? "no frames yet" : t.held > t.rewritten ? "holds our rotation" : "game rewrites it",
                      t.held, t.rewritten, t.base.x, t.base.y, t.base.z, t.base.w);
    }
}
