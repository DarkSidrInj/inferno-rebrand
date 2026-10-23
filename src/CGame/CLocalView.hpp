#pragma once
// What the local player looks like in third person. In first person the game keeps the own body
// as a shadow caster only (Renderer.shadowCastingMode = ShadowsOnly: it is in the scene, casts
// its shadow, is never drawn) and draws the first-person arms, gloves and weapon. Third person
// swaps that: body ShadowCastingMode.On, arms / gloves / first-person weapon ShadowsOnly.
//
// Every change is the game's own Renderer.set_shadowCastingMode, replayed (CUtils/CArm64.hpp),
// so whatever Unity updates behind the field (renderer scene flags, bitfields) is updated too.
// The mode a renderer had is read by dry-running the setter for each mode: the mode whose writes
// leave the renderer's bytes as they are is the current one. Switching off replays the setter
// with that original mode. Work runs on a worker thread; the frame only starts it.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
#include "CChams.hpp"
#include "CAnimCull.hpp"
#include "../CUtils/CArm64.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace localview {
    enum Mode { Off = 0, On = 1, TwoSided = 2, ShadowsOnly = 3 };

    struct Touched {
        uint64_t vtable{};                // to notice a renderer that was destroyed
        int original{-1};
        int wanted{-1};
        std::vector<a64::Store> expected; // renderer bytes our replay left, to notice the game changing them
    };

    inline int g_pid = -1;
    inline std::atomic<bool> g_busy{false};
    inline std::atomic<uint64_t> g_setter{0};
    inline bool g_resolved = false;
    inline long long g_last_pass = 0, g_last_weapon_scan = 0;
    inline std::mutex g_mutex;
    inline std::unordered_map<uint64_t, Touched> g_touched;  // native renderer -> state
    inline std::vector<uint64_t> g_weapon_objects;
    inline uint64_t g_weapon_owner = 0;
    inline bool g_active = false;

    inline bool inside(uint64_t r, uint64_t a) { return a >= r && a < r + 0x400; }

    // Runs set_shadowCastingMode(r, mode) without writing; `out` = the game stores it would make.
    inline bool run(uint64_t r, int mode, std::vector<a64::Store>& out, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = g_setter.load();
        start.x[0] = {r, true, 0, 0};
        start.x[1] = {uint64_t(uint32_t(mode)), true, 0, a64::kArg};
        start.x[2] = {0, true, 0, 0};
        auto goal = [](const a64::Machine& m) {
            if (!m.returned || m.blind_stores) return false;
            for (const auto& st : m.stores) if (!a64::local(st.address) && !st.known) return false;
            return true;
        };
        a64::Machine found;
        if (!a64::explore(session, start, goal, found)) { why = session.why; return false; }
        out.clear();
        for (const auto& st : found.stores) if (!a64::local(st.address)) out.push_back(st);
        return true;
    }

    inline bool unchanged(const std::vector<a64::Store>& stores, uint64_t r) {
        for (const auto& st : stores) {
            if (!inside(r, st.address)) continue;
            uint8_t now[16]{}, want[16]{};
            std::memcpy(want, &st.value, 8);
            std::memcpy(want + 8, &st.hi, 8);
            if (!mem_read(st.address, now, size_t(st.size)) || std::memcmp(now, want, size_t(st.size)) != 0) return false;
        }
        return true;
    }

    inline int current_mode(uint64_t r) {
        int found = -1, matches = 0;
        for (int mode : {On, ShadowsOnly, Off, TwoSided}) {
            std::vector<a64::Store> stores;
            std::string why;
            if (!run(r, mode, stores, why)) continue;
            if (unchanged(stores, r)) { found = mode; ++matches; }
        }
        return matches == 1 ? found : -1;
    }

    inline void resolve(uint64_t sample) {
        std::vector<std::pair<const char*, uint64_t>> tries;
        const uint64_t wrapper = proc::lib + v100::renderer_set_shadow_casting_injected;
        if (!icalls::slot_empty(wrapper)) tries.push_back({"setter wrapper", wrapper});
        for (const auto& c : icalls::candidates("UnityEngine.Renderer::set_shadowCastingMode_Injected")) tries.push_back({c.how, c.fn});
        for (const auto& t : tries) {
            g_setter = t.second;
            const int mode = current_mode(sample);
            if (mode >= 0) {
                diag::log("TP", "shadowCastingMode setter (%s): local body renderer mode %d%s", t.first, mode,
                          mode == ShadowsOnly ? " (shadows only: hidden by the game)" : "");
                g_resolved = true;
                return;
            }
        }
        g_setter = 0;
        diag::log("WARN", "TP shadowCastingMode setter not usable (%zu tried): body and arms stay as the game sets them", tries.size());
        g_resolved = true;
    }

    // Sets one renderer's mode through the setter, remembering the mode it had.
    inline void want(uint64_t r, int mode, std::unordered_map<uint64_t, bool>& seen) {
        if (!plausible(r)) return;
        seen[r] = true;
        std::lock_guard<std::mutex> lock(g_mutex);
        Touched& t = g_touched[r];
        const uint64_t vtable = rpm<uint64_t>(r);
        if (t.vtable != vtable) t = Touched{vtable};
        if (t.wanted == mode && unchanged(t.expected, r)) return;  // still as we left it
        if (t.original < 0 || t.wanted != mode) {
            const int now = current_mode(r);
            if (now < 0) return;
            if (t.original < 0) t.original = now;
            if (now == mode) { t.wanted = mode; t.expected.clear(); return; }
        }
        std::vector<a64::Store> stores;
        std::string why;
        if (!run(r, mode, stores, why)) { diag::log("WARN", "TP renderer 0x%llx mode %d: %s", (unsigned long long)r, mode, why.c_str()); return; }
        a64::commit(stores);
        t.wanted = mode;
        t.expected = stores;
    }

    inline void restore_all() {
        std::lock_guard<std::mutex> lock(g_mutex);
        int restored = 0;
        for (auto& e : g_touched) {
            const Touched& t = e.second;
            if (t.original < 0 || t.wanted == t.original || rpm<uint64_t>(e.first) != t.vtable) continue;
            std::vector<a64::Store> stores;
            std::string why;
            if (run(e.first, t.original, stores, why) && a64::commit(stores)) ++restored;
        }
        if (!g_touched.empty()) diag::log("TP", "first-person look restored on %d renderers", restored);
        g_touched.clear();
    }

    inline void pass(uint64_t bindings, bool scan_weapons) {
        if (!g_resolved) {
            const uint64_t body = chams::native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer));
            if (body) resolve(body);
        }
        if (!g_setter.load()) return;
        if (scan_weapons) {
            g_weapon_objects = chams::scan_weapons(bindings);
            g_weapon_owner = bindings;
            diag::log("TP", "first-person weapon: %zu weapon content objects", g_weapon_objects.size());
        }
        std::unordered_map<uint64_t, bool> seen;
        want(chams::native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer)), On, seen);
        want(chams::native_of(rpm<uint64_t>(bindings + v100::bindings_arms_renderer)), ShadowsOnly, seen);
        want(chams::native_of(rpm<uint64_t>(bindings + v100::bindings_gloves_renderer)), ShadowsOnly, seen);
        for (uint64_t object : g_weapon_objects) {
            if (rpm<uint64_t>(object + chams::kWeaponBindings) != bindings) continue;  // freed or reused
            std::vector<uint64_t> renderers;
            chams::weapon_renderers(object, renderers);
            for (uint64_t r : renderers) want(r, ShadowsOnly, seen);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_touched.begin(); it != g_touched.end();)
            it = seen.count(it->first) ? std::next(it) : g_touched.erase(it);  // gone: the game made new ones
    }

    // Once per frame. `active` = third person is drawing this frame.
    inline void tick(uint64_t local, bool active) {
        if (g_pid != proc::pid) {
            if (g_busy.load()) return;
            g_pid = proc::pid;
            g_setter = 0;
            g_resolved = false;
            g_touched.clear();
            g_weapon_objects.clear();
            g_weapon_owner = 0;
            g_active = false;
        }
        if (g_busy.load()) return;
        if (!active || !local || !cfg::thirdperson::show_body) {
            if (g_active) {
                g_active = false;
                g_busy = true;
                std::thread([] { restore_all(); g_busy = false; }).detach();
            }
            return;
        }
        const long long now = diag::now_ms();
        if (now - g_last_pass < 300) return;
        g_last_pass = now;
        const uint64_t bindings = chams::bindings_of(local);
        if (!bindings) return;
        const bool scan = bindings != g_weapon_owner || now - g_last_weapon_scan > 10000;
        if (scan) g_last_weapon_scan = now;
        g_active = true;
        g_busy = true;
        std::thread([bindings, scan] { pass(bindings, scan); g_busy = false; }).detach();
    }

    inline void wait() { while (g_busy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
