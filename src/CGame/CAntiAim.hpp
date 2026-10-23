#pragma once
// Anti-aim, ported from the legacy code (vintex offer(): aim controller -> movement transform and
// character transform, rotation written straight into Unity's transform data).
//
// In this build the aim controller is ios (dump.cs: rpi<nmj>, nmj = the networked pitch/yaw).
// Its two pivot transforms are ios.rkym (+0x50) and ios.qrhf (+0x58); they take the legacy
// "movement transform" / "character" roles. The rotation is written into the pivot's local
// entry of the transform data (TransformEntry: position, rotation, scale), as legacy did.
//
// The game re-poses the pivots from the aim state itself, so the write is repeated every frame
// on top of what the game wrote (relative), or on top of the last base when the game leaves the
// pivot alone. Which of the two happens is logged per pivot ("game rewrites" / "holds"), so a log
// from a match shows whether the pivot is driven per frame.
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
    struct Pivot {
        uint64_t field;          // ios field holding the managed Transform
        uint64_t native{};
        int index_offset{-1};
        Vector4 base{};          // rotation the game wants, without ours
        Vector4 written{};
        bool wrote{};
        int held{}, rewritten{};
    };

    inline int g_pid = -1;
    inline Pivot g_pivots[2] = {{v100::ios_pivot_a}, {v100::ios_pivot_b}};
    inline uint64_t g_ios = 0;
    inline long long g_ios_at = 0, g_log = 0, g_start = 0;
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

    // Address of the pivot's local entry, or 0.
    inline uint64_t entry(const Pivot& p) {
        const auto& l = player::g_transform_layout;
        if (!p.native || l.pid != proc::pid || l.data_offset < 0 || p.index_offset < 0) return 0;
        const uint64_t data = rpm<uint64_t>(p.native + uint64_t(l.data_offset));
        const int index = rpm<int>(p.native + uint64_t(p.index_offset));
        const uint64_t entries = plausible(data) ? rpm<uint64_t>(data + uint64_t(l.entries_offset)) : 0;
        if (!plausible(entries) || index < 0 || index > skeleton::kMaxIndex) return 0;
        return entries + uint64_t(index) * sizeof(player::TransformEntry);
    }

    // Finds the pivots of the local ios and the index offset that places them on the local player.
    inline bool bind(uint64_t local, const Vector3& feet) {
        const long long now = diag::now_ms();
        if (g_ios && now - g_ios_at < 2000) return true;
        g_ios_at = now;
        const uint64_t ios_class = rpm<uint64_t>(proc::lib + v100::ios_typeinfo);
        const uint64_t ios = find_player_component(local, ios_class);
        if (!ios) { g_ios = 0; return false; }
        for (Pivot& p : g_pivots) {
            const uint64_t managed = rpm<uint64_t>(ios + p.field);
            const uint64_t native = plausible(managed) ? rpm<uint64_t>(managed + v100::unity_cached_ptr) : 0;
            if (native == p.native && p.index_offset >= 0) continue;
            p = Pivot{p.field};
            if (!plausible(native)) continue;
            p.native = native;
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
                p.index_offset = off;
                diag::log("AA", "pivot ios+0x%llx native=0x%llx index +0x%x (%.2f m from the feet)",
                          (unsigned long long)p.field, (unsigned long long)native, off, d);
                break;
            }
            if (p.index_offset < 0) diag::log("AA", "pivot ios+0x%llx: no index offset places it on the player", (unsigned long long)p.field);
        }
        if (ios != g_ios) diag::log("AA", "local aim controller ios=0x%llx", (unsigned long long)ios);
        g_ios = ios;
        return true;
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
        if (cfg::antiaim::pitch_mode == 1) pitch = 89.f;        // Unity: +X looks down
        else if (cfg::antiaim::pitch_mode == 2) pitch = -89.f;
        return player::quaternion_multiply(axis_angle(0.f, 1.f, 0.f, yaw), axis_angle(1.f, 0.f, 0.f, pitch));
    }

    // Puts back what the game had where it does not re-pose the pivot itself.
    inline void release() {
        for (Pivot& p : g_pivots) {
            const uint64_t at = entry(p);
            if (at && p.wrote && p.held > p.rewritten) wpm<Vector4>(at + 0x10, p.base);
            p.wrote = false;
            p.held = p.rewritten = 0;
        }
    }

    inline bool targeted(int i) {
        return cfg::antiaim::target == 0 || cfg::antiaim::target == i + 1;
    }

    inline void tick(uint64_t local, const Vector3& feet) {
        if (g_pid != proc::pid) {
            g_pid = proc::pid;
            for (Pivot& p : g_pivots) p = Pivot{p.field};
            g_ios = 0;
        }
        if (!cfg::antiaim::enabled || !local) { release(); return; }
        if (!bind(local, feet)) return;
        const long long now = diag::now_ms();
        if (!g_start) g_start = now;

        for (int i = 0; i < 2; ++i) {
            Pivot& p = g_pivots[i];
            const uint64_t at = entry(p);
            if (!at) continue;
            if (!targeted(i)) { if (p.wrote && p.held > p.rewritten) wpm<Vector4>(at + 0x10, p.base); p.wrote = false; continue; }
            const Vector4 current = rpm<Vector4>(at + 0x10);
            if (p.wrote && same(current, p.written)) ++p.held;      // the game left our value: keep the old base
            else { p.base = current; if (p.wrote) ++p.rewritten; }  // the game posed it again this frame
            if (cfg::antiaim::yaw_mode == 0) {
                Vector4 q{};
                for (int k = 0; k < 4; ++k) { q = degenerate(); wpm<Vector4>(at + 0x10, q); }
                p.written = q;
            } else {
                Vector4 q = player::quaternion_multiply(p.base, delta(now));
                if (!player::normalize_quaternion(q)) continue;
                wpm<Vector4>(at + 0x10, q);
                p.written = q;
            }
            p.wrote = true;
        }

        if (diag::every(g_log, 3000)) {
            const uint64_t state = rpm<uint64_t>(g_ios + v100::ios_aim_state);
            const Vector4 view = plausible(state) ? rpm<Vector4>(state + 0x10) : Vector4{};
            const Vector4 yaw = plausible(state) ? rpm<Vector4>(state + 0x20) : Vector4{};
            for (int i = 0; i < 2; ++i) {
                const Pivot& p = g_pivots[i];
                diag::log("AA", "pivot ios+0x%llx: %s (held %d, rewritten %d) base=(%.3f %.3f %.3f %.3f)",
                          (unsigned long long)p.field, !entry(p) ? "not bound" : p.held + p.rewritten == 0 ? "no frames yet" :
                          p.held > p.rewritten ? "holds our rotation" : "game rewrites it",
                          p.held, p.rewritten, p.base.x, p.base.y, p.base.z, p.base.w);
            }
            diag::log("AA", "aim state view=(%.3f %.3f %.3f %.3f) yaw=(%.3f %.3f %.3f %.3f)",
                      view.x, view.y, view.z, view.w, yaw.x, yaw.y, yaw.z, yaw.w);
        }
    }
}
