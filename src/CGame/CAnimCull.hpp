#pragma once
// Occlusion bypass for the skeleton. Remote players' Animators use a culling mode that stops
// updating the bones while their renderer is culled (behind a wall, off screen), so the
// skeleton freezes exactly when it matters. This sets each enemy's native Animator to
// AnimatorCullingMode.AlwaysAnimate (0) and puts the original mode back when switched off.
//
// The m_CullingMode offset inside the native Animator is not guessed: Animator's accessor is
// traced (CUtils/CArm64.hpp) on a real enemy animator: the il2cpp wrapper from dump.cs when its
// icall slot is filled, otherwise the native icall found by name (CIcalls.hpp). The getter's
// load, or the setter's store of its argument, lands on the field. Runs on its own thread once
// per process; the frame only writes. A change is made by replaying the game's own setter
// (CUtils/CArm64.hpp), so whatever Unity does when the mode changes happens too; a plain
// write is the fallback.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
#include "CSkeleton.hpp"
#include "../CUtils/CArm64.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace animcull {
    inline int g_pid = -1;
    inline std::atomic<bool> g_resolving{false};
    inline std::atomic<int> g_offset{-1};      // m_CullingMode inside the native Animator, -1 = unknown
    inline std::atomic<uint64_t> g_setter{0};  // set_cullingMode(native, mode) code, for replays
    inline bool g_replay_logged = false;
    inline long long g_last_resolve = 0, g_last_apply = 0;
    inline std::unordered_map<uint64_t, int32_t> g_original;  // native animator -> mode it had
    inline std::unordered_map<uint64_t, std::pair<uint64_t, long long>> g_animator_of;  // player -> (native, when)

    inline uint64_t native_animator(uint64_t player) {
        const long long now = diag::now_ms();
        const auto it = g_animator_of.find(player);
        if (it != g_animator_of.end() && now - it->second.second < 2000) return it->second.first;
        uint64_t native = 0;
        const uint64_t model = skeleton::find_model(player);
        if (plausible(model)) {
            uint64_t managed = rpm<uint64_t>(model + v100::model_animator);
            if (!plausible(managed)) {
                const uint64_t bindings = rpm<uint64_t>(model + v100::model_bindings);
                if (plausible(bindings)) managed = rpm<uint64_t>(bindings + v100::bindings_animator);
            }
            if (plausible(managed)) native = rpm<uint64_t>(managed + v100::unity_cached_ptr);
            if (!plausible(native)) native = 0;
        }
        g_animator_of[player] = {native, now};
        return native;
    }

    // Field offset from tracing one accessor with x0 = `self`; setters get a tagged argument in x1.
    inline int trace(uint64_t entry, uint64_t self, bool setter, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        start.x[0] = {self, true, 0, 0};
        start.x[1] = setter ? a64::Reg{0, true, 0, a64::kArg} : a64::Reg{0, true, 0, 0};
        start.x[2] = {0, true, 0, 0};
        int offset = -1;
        auto inside = [&](uint64_t a) { return a >= self && a < self + 0x1000; };
        auto goal = [&](const a64::Machine& m) {
            if (setter) {
                for (const auto& st : m.stores)
                    if (st.tag == a64::kArg && inside(st.address)) { offset = int(st.address - self); return true; }
                return false;
            }
            if (!m.returned) return false;
            if (inside(m.x[0].src)) { offset = int(m.x[0].src - self); return true; }
            if (inside(m.last_load) && m.last_load_size == 4) { offset = int(m.last_load - self); return true; }
            return false;
        };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return offset;
        why = session.why;
        return -1;
    }

    inline bool sane_mode(uint64_t animator, int offset) {
        const int32_t v = rpm<int32_t>(animator + uint64_t(offset));
        return v >= 0 && v <= 2;
    }

    inline void resolve(uint64_t native, uint64_t managed, uint64_t other) {
        struct Try { const char* how; uint64_t entry; bool setter; uint64_t self; };
        std::vector<Try> tries;
        const uint64_t get_w = proc::lib + v100::animator_get_culling_injected, set_w = proc::lib + v100::animator_set_culling_injected;
        if (!icalls::slot_empty(get_w)) tries.push_back({"getter wrapper", get_w, false, native});
        if (!icalls::slot_empty(set_w)) tries.push_back({"setter wrapper", set_w, true, native});
        for (const auto& c : icalls::candidates("UnityEngine.Animator::get_cullingMode_Injected")) tries.push_back({c.how, c.fn, false, native});
        for (const auto& c : icalls::candidates("UnityEngine.Animator::set_cullingMode_Injected")) tries.push_back({c.how, c.fn, true, native});
        // Pre-Unity-6 bindings take the managed object and read m_CachedPtr themselves.
        for (const auto& c : icalls::candidates("UnityEngine.Animator::get_cullingMode")) tries.push_back({c.how, c.fn, false, managed});
        for (const auto& c : icalls::candidates("UnityEngine.Animator::set_cullingMode")) tries.push_back({c.how, c.fn, true, managed});

        std::string why;
        for (const Try& t : tries) {
            int offset = trace(t.entry, t.self, t.setter, why);
            if (offset >= 0 && t.self == managed) offset = -1;  // a managed-object trace lands in managed memory
            if (offset < 0 || !sane_mode(native, offset) || (other && !sane_mode(other, offset))) continue;
            g_offset = offset;
            diag::log("CULL", "Animator m_CullingMode at +0x%x (%s, mode now %d)", offset,
                      t.how, rpm<int32_t>(native + uint64_t(offset)));
            // The setter, replayed instead of a plain write, also runs what Unity does when the
            // mode changes (re-registering the animator for updates).
            for (const Try& s2 : tries) {
                if (!s2.setter || s2.self != native) continue;
                std::string w;
                if (trace(s2.entry, native, true, w) == offset) { g_setter = s2.entry; break; }
            }
            diag::log("CULL", "setter %s", g_setter.load() ? "found, changes are replayed" : "not found, changes are written directly");
            return;
        }
        diag::log("WARN", "CULL Animator culling offset not found (%zu accessors tried%s%s)", tries.size(),
                  why.empty() ? "" : ", last: ", why.c_str());
    }

    // Sets one animator's culling mode the way the game would: replays set_cullingMode, or
    // writes the field when that cannot be done.
    inline void set_mode(uint64_t animator, int32_t mode) {
        const int offset = g_offset.load();
        const uint64_t field = animator + uint64_t(offset);
        if (const uint64_t fn = g_setter.load()) {
            a64::Machine m;
            m.pc = fn;
            m.x[0] = {animator, true, 0, 0};
            m.x[1] = {uint64_t(uint32_t(mode)), true, 0, a64::kArg};
            m.x[2] = {0, true, 0, 0};
            std::vector<a64::Store> stores;
            std::string why;
            bool ok = a64::replay(m, field, stores, why);
            for (const auto& st : stores)
                if (st.address == field && st.size == 4 && int32_t(uint32_t(st.value)) != mode) ok = false;
            if (!g_replay_logged) {
                g_replay_logged = true;
                diag::log("CULL", "set_cullingMode replay: %s (%zu stores%s%s)", ok ? "ok" : "failed", stores.size(),
                          why.empty() ? "" : ", ", why.c_str());
            }
            if (ok && a64::commit(stores)) return;
        }
        wpm<int32_t>(field, mode);
    }

    inline void restore() {
        const int offset = g_offset.load();
        if (offset >= 0)
            for (const auto& entry : g_original)
                if (plausible(entry.first)) set_mode(entry.first, entry.second);
        if (!g_original.empty()) diag::log("CULL", "restored culling mode on %zu animators", g_original.size());
        g_original.clear();
    }

    // Once per frame with the enemies being drawn.
    inline void tick(const std::vector<uint64_t>& enemies, bool enabled) {
        if (g_pid != proc::pid) {
            if (g_resolving.load()) return;
            g_pid = proc::pid;
            g_offset = -1;
            g_setter = 0;
            g_replay_logged = false;
            g_original.clear();
            g_animator_of.clear();
            g_last_resolve = 0;
        }
        if (!enabled) { if (!g_original.empty()) restore(); return; }
        if (g_resolving.load()) return;
        const long long now = diag::now_ms();

        if (g_offset.load() < 0) {
            if (now - g_last_resolve < 10000) return;
            uint64_t native = 0, managed = 0, other = 0;
            for (uint64_t p : enemies) {
                const uint64_t n = native_animator(p);
                if (!n) continue;
                if (!native) {
                    native = n;
                    const uint64_t model = skeleton::find_model(p);
                    managed = plausible(model) ? rpm<uint64_t>(model + v100::model_animator) : 0;
                } else if (n != native) { other = n; break; }
            }
            if (!native) return;
            g_last_resolve = now;
            g_resolving = true;
            std::thread([native, managed, other] { resolve(native, managed, other); g_resolving = false; }).detach();
            return;
        }

        if (now - g_last_apply < 250) return;
        g_last_apply = now;
        const int offset = g_offset.load();
        for (uint64_t p : enemies) {
            const uint64_t animator = native_animator(p);
            if (!animator) continue;
            const int32_t mode = rpm<int32_t>(animator + uint64_t(offset));
            if (mode == 0 || mode < 0 || mode > 2) continue;  // already AlwaysAnimate, or not an animator
            g_original.emplace(animator, mode);
            set_mode(animator, 0);
        }
        if (g_animator_of.size() > 256) g_animator_of.clear();
    }

    inline void wait() { while (g_resolving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
