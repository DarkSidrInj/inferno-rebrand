#pragma once
// What the local player looks like in third person. Measured on device (TPDIAG):
//   - the first-person arms and weapon are on layer 8, and the main camera's culling mask leaves
//     layer 8 out (0xfffffeff): a second camera draws them from the first-person eye. Third person
//     never moves that camera, so the hands stay where first person had them. That camera is found
//     (same native Camera vtable as the main one, layer 8 in its mask) and loses layer 8 while third
//     person is on; its mask is put back after.
//   - the body is on layer 0 with shadow casting On, and even a chams material on it never shows:
//     the renderer itself is off. Renderer.enabled is read with the game's get_enabled and switched
//     on with its set_enabled, replayed (CUtils/CArm64.hpp) only along a path that stays inside
//     libunity and steps over no call, so nothing is half-applied. Switched off again after.
//   - the body gets the flat material (id 0), as other cheats draw it, unless chams draws it.
// Work runs on a worker thread; the frame only starts it.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
#include "CChams.hpp"
#include "CThirdPerson.hpp"
#include "CPlayer.hpp"
#include "../CUtils/CArm64.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace localview {
    inline constexpr uint32_t kFirstPersonLayer = 1u << 8;

    struct ViewCamera { uint64_t native{}; uint64_t vtable{}; uint32_t mask_before{}; };

    inline int g_pid = -1;
    inline std::atomic<bool> g_busy{false};
    inline long long g_last_pass = 0;
    inline bool g_active = false;
    inline uint64_t g_dumped_for = 0;

    // Second cameras that draw the first-person layer.
    inline uint64_t g_cameras_for = 0;             // main camera they were searched for
    inline std::vector<ViewCamera> g_view_cameras;

    // Body renderer switched on by us.
    inline uint64_t g_enabled_body = 0, g_enabled_vtable = 0;
    inline uint64_t g_checked_body = 0;
    inline uint64_t g_enabled_getter = 0, g_enabled_setter = 0;
    inline bool g_accessors_resolved = false;

    // Material swap, same bookkeeping as chams::Patched.
    inline std::unordered_map<uint64_t, chams::Patched> g_materials;
    inline bool g_logged_material = false;

    // ---- diagnostics ------------------------------------------------------------------------
    // Once per body: the bytes where the body renderer differs from the arms renderer, and both
    // GameObjects (native Component +0x20).
    inline void dump_look(uint64_t bindings) {
        if (g_dumped_for == bindings) return;
        g_dumped_for = bindings;
        const uint64_t body = chams::native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer));
        const uint64_t arms = chams::native_of(rpm<uint64_t>(bindings + v100::bindings_arms_renderer));
        diag::log("TPDIAG", "body renderer 0x%llx (vtable 0x%llx), arms renderer 0x%llx (vtable 0x%llx)",
                  (unsigned long long)body, (unsigned long long)rpm<uint64_t>(body), (unsigned long long)arms,
                  (unsigned long long)rpm<uint64_t>(arms));
        if (!body || !arms) return;
        uint8_t a[0x200]{}, b[0x200]{};
        if (mem_read(body, b, sizeof(b)) && mem_read(arms, a, sizeof(a))) {
            for (size_t off = 0; off < sizeof(a); off += 8) {
                uint64_t x = 0, y = 0;
                std::memcpy(&x, b + off, 8);
                std::memcpy(&y, a + off, 8);
                if (x == y || (plausible(x) && plausible(y))) continue;  // same, or two different pointers
                diag::log("TPDIAG", "renderer+0x%03zx body=%016llx arms=%016llx", off, (unsigned long long)x, (unsigned long long)y);
            }
        }
        player::dump_memory("body.gameobject", rpm<uint64_t>(body + 0x20), 0x80);
        player::dump_memory("arms.gameobject", rpm<uint64_t>(arms + 0x20), 0x80);
    }

    // ---- strict replays -----------------------------------------------------------------------
    // Calls are followed only inside the executable mapping that holds `fn` (libunity's code):
    // anything else (libc's malloc, locks) is stepped over and makes a strict replay fail.
    inline void confine(a64::Session& s, uint64_t fn) {
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
        if (FILE* f = std::fopen(path, "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), f)) {
                unsigned long long lo = 0, hi = 0;
                char perms[5]{};
                if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) == 3 && perms[2] == 'x' && fn >= lo && fn < hi) {
                    s.code_lo = lo;
                    s.code_hi = hi;
                    break;
                }
            }
            std::fclose(f);
        }
        s.max_total = 200000;
    }

    // Runs fn(self, arg) to its return; `out` = game stores. Strict: no call stepped over once the
    // code has touched `self` (the main-thread check before that may call what cannot be followed).
    // Where a failed replay spent its steps (once per function): enough to read the engine path offline.
    inline void report_replay(const char* name, uint64_t fn, const a64::Session& s) {
        static std::unordered_map<uint64_t, bool> reported;
        if (!reported.emplace(fn, true).second) return;
        auto dump = [](const char* what, uint64_t at, int count) {
            std::vector<uint32_t> words(size_t(count), 0);
            if (!mem_read(at, words.data(), words.size() * 4)) return;
            std::string text;
            char hex[10];
            for (uint32_t w : words) { std::snprintf(hex, sizeof(hex), "%08x ", w); text += hex; }
            diag::log("TPDIAG", "  %s 0x%llx (lib+0x%llx): %s", what, (unsigned long long)at, (unsigned long long)(at - proc::lib), text.c_str());
        };
        auto top = [](const std::unordered_map<uint64_t, uint32_t>& counts, size_t n) {
            std::vector<std::pair<uint64_t, uint32_t>> v(counts.begin(), counts.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (v.size() > n) v.resize(n);
            return v;
        };
        diag::log("TPDIAG", "%s 0x%llx replay profile: %zu distinct pcs, %zu fork sites, %zu callees, %ld steps (%s)", name,
                  (unsigned long long)fn, s.hot.size(), s.fork_sites.size(), s.call_targets.size(), s.total_steps, s.why);
        dump("entry", fn, 40);
        for (const auto& h : top(s.hot, 2)) { diag::log("TPDIAG", "  hot pc lib+0x%llx x%u", (unsigned long long)(h.first - proc::lib), h.second); dump("around", h.first - 32, 24); }
        for (const auto& f : top(s.fork_sites, 4)) diag::log("TPDIAG", "  fork pc lib+0x%llx x%u", (unsigned long long)(f.first - proc::lib), f.second);
        for (const auto& c : top(s.call_targets, 5)) { diag::log("TPDIAG", "  callee lib+0x%llx x%u", (unsigned long long)(c.first - proc::lib), c.second); dump("callee", c.first, 20); }
    }

    inline bool run(uint64_t fn, uint64_t self, uint64_t arg, bool strict, a64::Machine& found, std::string& why, bool heavy = false) {
        a64::Session session;
        confine(session, fn);
        if (heavy) {  // the enabling path walks scene lists: bigger budgets, and a profile if it still fails
            session.max_steps = 30000;
            session.max_total = 600000;
            session.max_site_forks = 64;
            session.max_depth = 12;
            session.profile = true;
        }
        a64::Machine start;
        start.pc = fn;
        start.x[0] = {self, true, 0, 0};
        start.x[1] = {arg, true, 0, a64::kArg};
        start.x[2] = {0, true, 0, 0};
        start.watch_lo = self;
        start.watch_hi = self + 0x1000;
        auto goal = [strict](const a64::Machine& m) {
            if (!m.returned || m.blind_stores || (strict && m.skipped_late)) return false;
            for (const auto& st : m.stores) if (!a64::local(st.address) && !st.known) return false;
            return true;
        };
        if (a64::explore(session, start, goal, found)) return true;
        why = session.why;
        if (heavy) report_replay("set_enabled", fn, session);
        return false;
    }

    inline int is_enabled(uint64_t r) {
        if (!g_enabled_getter) return -1;
        a64::Machine m;
        std::string why;
        if (!run(g_enabled_getter, r, 0, false, m, why) || !m.x[0].known) return -1;
        return int(m.x[0].value & 0xFF) ? 1 : 0;
    }

    // set_enabled(r, value), committed only when the strict replay goes through.
    inline bool set_enabled(uint64_t r, bool value, std::string& why) {
        if (!g_enabled_setter) { why = "no set_enabled"; return false; }
        a64::Machine m;
        if (!run(g_enabled_setter, r, value ? 1 : 0, true, m, why, true)) return false;
        std::vector<a64::Store> stores;
        for (const auto& st : m.stores) if (!a64::local(st.address)) stores.push_back(st);
        diag::log("TP", "set_enabled(%d) on 0x%llx: %zu stores", value ? 1 : 0, (unsigned long long)r, stores.size());
        return a64::commit(stores);
    }

    // Last resort when the setter cannot be replayed: the flag byte and bit behind get_enabled.
    // The byte is the first one-byte load inside the renderer; the bit is the one whose change flips
    // the getter, found by running the getter on a copy of the byte with each bit set / cleared.
    // Writing it does not register the renderer with the scene the way the setter does, so this
    // only helps where Unity keeps the renderer registered while disabled.
    inline bool find_flag(uint64_t r, uint64_t& addr, int& bit) {
        a64::Session session;
        confine(session, g_enabled_getter);
        a64::Machine start;
        start.pc = g_enabled_getter;
        start.x[0] = {r, true, 0, 0};
        start.x[1] = {0, true, 0, 0};
        start.watch_lo = r;
        start.watch_hi = r + 0x1000;
        a64::Machine found;
        if (!a64::explore(session, start, [](const a64::Machine& m) { return m.returned && m.watch_byte; }, found)) return false;
        addr = found.watch_byte;
        const uint8_t cur = rpm<uint8_t>(addr);
        const int before = is_enabled(r);
        for (int b = 0; b < 8; ++b) {
            for (int set = 0; set < 2; ++set) {
                a64::Session s2;
                confine(s2, g_enabled_getter);
                a64::Machine m = start;
                m.overlay[addr] = int16_t(set ? (cur | (1 << b)) : (cur & ~(1 << b)));
                a64::Machine out;
                if (!a64::explore(s2, m, [](const a64::Machine& x) { return x.returned; }, out) || !out.x[0].known) continue;
                const int now = int(out.x[0].value & 0xFF) ? 1 : 0;
                if (set && before == 0 && now == 1) { bit = b; return true; }
            }
        }
        return false;
    }

    inline uint64_t g_flag_addr = 0;
    inline int g_flag_bit = -1;

    inline void resolve_accessors(uint64_t body, uint64_t arms) {
        g_accessors_resolved = true;
        std::vector<uint64_t> getters, setters;
        const uint64_t gw = proc::lib + v100::renderer_get_enabled_injected, sw = proc::lib + v100::renderer_set_enabled_injected;
        if (!icalls::slot_empty(gw)) getters.push_back(gw);
        if (!icalls::slot_empty(sw)) setters.push_back(sw);
        for (const auto& c : icalls::candidates("UnityEngine.Renderer::get_enabled_Injected")) getters.push_back(c.fn);
        for (const auto& c : icalls::candidates("UnityEngine.Renderer::set_enabled_Injected")) setters.push_back(c.fn);
        for (uint64_t g : getters) {
            g_enabled_getter = g;
            if (is_enabled(arms) == 1) break;  // the arms are drawn: a working getter says 1
            g_enabled_getter = 0;
        }
        // The setter must be a no-op on the arms (already enabled): a strict replay of set_enabled(1)
        // that leaves every byte of the arms renderer as it is.
        for (uint64_t fn : setters) {
            a64::Machine m;
            std::string why;
            if (!run(fn, arms, 1, true, m, why)) { diag::log("TP", "set_enabled candidate 0x%llx: %s", (unsigned long long)fn, why.c_str()); continue; }
            g_enabled_setter = fn;
            break;
        }
        diag::log("TP", "Renderer.enabled: getter %s, setter %s; body enabled=%d arms enabled=%d",
                  g_enabled_getter ? "found" : "NOT found", g_enabled_setter ? "found" : "NOT found",
                  is_enabled(body), is_enabled(arms));
    }

    // ---- second camera ----------------------------------------------------------------------------
    inline void find_view_cameras(uint64_t main) {
        g_cameras_for = main;
        g_view_cameras.clear();
        const int mask_off = thirdperson::g_mask_offset.load();
        const uint64_t vtable = rpm<uint64_t>(main);
        if (mask_off < 0 || !plausible(vtable)) { diag::log("TP", "second camera search skipped (culling mask offset unknown)"); return; }
        if (g_scan_maps.empty()) refresh_scan_maps();
        const std::vector<remote_map> maps = g_scan_maps;
        std::vector<uint64_t> words;
        int cameras = 0;
        for (const auto& m : maps) {
            for (uint64_t at = m.begin; at < m.end && g_view_cameras.size() < 8;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.end - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 8) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); i += 2) {
                        if (words[i] != vtable) continue;
                        const uint64_t cam = at + i * 8;
                        matrix v{}, p{};
                        if (cam == main || !validate_camera(cam, v, p, false)) continue;
                        ++cameras;
                        const uint32_t mask = rpm<uint32_t>(cam + uint64_t(mask_off));
                        diag::log("TP", "camera 0x%llx culling mask 0x%08x%s", (unsigned long long)cam, mask,
                                  (mask & kFirstPersonLayer) ? " (draws the first-person layer)" : "");
                        if (mask & kFirstPersonLayer) g_view_cameras.push_back({cam, vtable, mask});
                    }
                }
                at += chunk;
            }
        }
        diag::log("TP", "second cameras: %d found, %zu draw layer 8", cameras, g_view_cameras.size());
    }

    // ---- camera masks -----------------------------------------------------------------------------
    // One place decides every camera mask from the game's own values:
    //   first-person camera = original, minus layer 8 while third person is on, plus the moved layers
    //   main camera         = original, minus the moved layers
    // Moved layers are the enemies' (chams layer pass): the main camera stops drawing them and the
    // first-person camera, which clears depth, draws them over the world.
    inline bool g_hide_first_person = false;
    inline uint32_t g_layer_move = 0;
    inline uint64_t g_main_for = 0;
    inline uint32_t g_main_before = 0;

    inline void apply_masks(uint64_t main) {
        const int off = thirdperson::g_mask_offset.load();
        if (off < 0) return;
        for (const auto& c : g_view_cameras) {
            if (rpm<uint64_t>(c.native) != c.vtable) continue;
            const uint32_t want = (c.mask_before & ~(g_hide_first_person ? kFirstPersonLayer : 0u)) | g_layer_move;
            if (rpm<uint32_t>(c.native + uint64_t(off)) != want) wpm<uint32_t>(c.native + uint64_t(off), want);
        }
        if (!main || rpm<uint64_t>(main) == 0) return;
        if (g_main_for != main) { g_main_for = main; g_main_before = rpm<uint32_t>(main + uint64_t(off)); }
        const uint32_t want = g_main_before & ~g_layer_move;
        if (rpm<uint32_t>(main + uint64_t(off)) != want) wpm<uint32_t>(main + uint64_t(off), want);
    }

    // Chams layer pass, once per frame. `enemy_layers` = bit per layer of the enemies' body renderers.
    inline bool g_layer_started = false;
    inline void layer_pass_tick(uint64_t main, uint32_t enemy_layers) {
        const uint32_t protect = (1u << 0) | kFirstPersonLayer;  // the world and the arms never move
        const bool want = cfg::chams::enabled && cfg::chams::layer_pass && main && enemy_layers && !(enemy_layers & protect);
        static long long log_stamp = 0;
        if (cfg::chams::enabled && cfg::chams::layer_pass && enemy_layers & protect && diag::every(log_stamp, 10000))
            diag::log("CHAMS", "layer pass skipped: enemies share layer mask 0x%08x with the world / first-person layer", enemy_layers);
        if (!want) {
            if (g_layer_move) { g_layer_move = 0; apply_masks(main ? main : g_main_for); diag::log("CHAMS", "layer pass off"); }
            return;
        }
        if (g_busy.load() || thirdperson::g_mask_resolving.load()) return;
        if (thirdperson::g_mask_offset.load() < 0) {
            thirdperson::g_mask_resolving = true;
            std::thread([main] { thirdperson::resolve_mask(main); thirdperson::g_mask_resolving = false; }).detach();
            return;
        }
        if (g_cameras_for != main) {
            g_busy = true;
            std::thread([main] { find_view_cameras(main); g_busy = false; }).detach();
            return;
        }
        if (g_view_cameras.empty()) return;
        if (g_layer_move != enemy_layers) {
            g_layer_move = enemy_layers;
            diag::log("CHAMS", "layer pass on: layers 0x%08x leave the main camera and go to the first-person camera", enemy_layers);
        }
        apply_masks(main);
    }

    // ---- materials ----------------------------------------------------------------------------
    inline bool patch_material(uint64_t r, int32_t want) {
        const int data_off = chams::g_data_offset.load(), size_off = chams::g_size_offset.load();
        if (!plausible(r) || data_off < 0) return false;
        const uint64_t array = rpm<uint64_t>(r + uint64_t(data_off));
        const int count = size_off >= 0 ? rpm<int32_t>(r + uint64_t(size_off)) : 1;
        if (!plausible(array) || count < 1 || count > chams::kMaxSlots) return false;
        int32_t ids[chams::kMaxSlots]{};
        if (!mem_read(array, ids, size_t(count) * 4)) return false;
        auto it = g_materials.find(r);
        if (it != g_materials.end() && it->second.array != array) { g_materials.erase(it); it = g_materials.end(); }
        if (it == g_materials.end()) {
            chams::Patched fresh;
            fresh.array = array;
            fresh.original.assign(ids, ids + count);
            it = g_materials.emplace(r, fresh).first;
        }
        it->second.written = want;
        for (int i = 0; i < count && i < int(it->second.original.size()); ++i)
            if (ids[i] != want) wpm<int32_t>(array + uint64_t(i) * 4, want);
        return true;
    }

    inline void restore_materials() {
        const int data_off = chams::g_data_offset.load();
        for (const auto& e : g_materials) {
            const chams::Patched& p = e.second;
            if (data_off < 0 || rpm<uint64_t>(e.first + uint64_t(data_off)) != p.array) continue;
            for (size_t i = 0; i < p.original.size(); ++i)
                if (rpm<int32_t>(p.array + i * 4) == p.written) wpm<int32_t>(p.array + i * 4, p.original[i]);
        }
        g_materials.clear();
        g_logged_material = false;
    }

    // ---- frame --------------------------------------------------------------------------------
    inline void restore_all() {
        restore_materials();
        g_hide_first_person = false;
        apply_masks(g_main_for);
        if (g_enabled_body && plausible(g_enabled_vtable) && rpm<uint64_t>(g_enabled_body) == g_enabled_vtable) {
            std::string why;
            if (g_flag_addr && g_flag_bit >= 0) {
                wpm<uint8_t>(g_flag_addr, uint8_t(rpm<uint8_t>(g_flag_addr) & ~(1 << g_flag_bit)));
                g_flag_addr = 0;
                g_flag_bit = -1;
            } else if (!set_enabled(g_enabled_body, false, why)) diag::log("WARN", "TP body renderer left enabled: %s", why.c_str());
        }
        g_enabled_body = 0;
        g_checked_body = 0;
        diag::log("TP", "first-person look restored");
    }

    inline void pass(uint64_t bindings, uint64_t main_camera) {
        dump_look(bindings);
        const uint64_t body = chams::native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer));
        const uint64_t arms = chams::native_of(rpm<uint64_t>(bindings + v100::bindings_arms_renderer));

        // First-person layer off the second camera(s).
        if (main_camera && g_cameras_for != main_camera) find_view_cameras(main_camera);
        g_hide_first_person = true;
        apply_masks(main_camera);

        // Body renderer on.
        if (body && arms && !g_accessors_resolved) resolve_accessors(body, arms);
        if (body && g_checked_body != body) {
            g_checked_body = body;
            const int on = is_enabled(body);
            if (on == 0) {
                std::string why;
                if (set_enabled(body, true, why)) { g_enabled_body = body; g_enabled_vtable = rpm<uint64_t>(body); }
                diag::log("TP", "body renderer was disabled: %s (now enabled=%d)", g_enabled_body ? "switched on" : why.c_str(),
                          is_enabled(body));
                if (!g_enabled_body && g_enabled_getter) {
                    uint64_t addr = 0;
                    int bit = -1;
                    if (find_flag(body, addr, bit)) {
                        g_flag_addr = addr;
                        g_flag_bit = bit;
                        wpm<uint8_t>(addr, uint8_t(rpm<uint8_t>(addr) | (1 << bit)));
                        g_enabled_body = body;
                        g_enabled_vtable = rpm<uint64_t>(body);
                        diag::log("TP", "body renderer flag written directly: byte 0x%llx (renderer+0x%llx) bit %d, now enabled=%d",
                                  (unsigned long long)addr, (unsigned long long)(addr - body), bit, is_enabled(body));
                    } else {
                        diag::log("TP", "body renderer enabled flag not found: the body stays hidden");
                    }
                }
            } else {
                diag::log("TP", "body renderer enabled=%d", on);
            }
        }

        // Body material.
        if (chams::g_data_offset.load() < 0 && body && !chams::g_resolving.exchange(true)) {
            chams::resolve(body);
            chams::g_resolving = false;
        }
        if (body && (!cfg::chams::enabled || !cfg::chams::local_body)) {
            const bool ok = patch_material(body, 0);
            if (!g_logged_material) { g_logged_material = true; diag::log("TP", "body material -> flat (id 0): %s", ok ? "ok" : "failed"); }
        } else if (!g_materials.empty()) {
            restore_materials();  // chams draws the body
        }
    }

    // Once per frame. `active` = third person is drawing this frame; `camera` = the main camera.
    inline void tick(uint64_t local, bool active, uint64_t camera = 0) {
        if (g_pid != proc::pid) {
            if (g_busy.load()) return;
            g_pid = proc::pid;
            g_active = false;
            g_dumped_for = 0;
            g_cameras_for = 0;
            g_view_cameras.clear();
            g_enabled_body = g_checked_body = 0;
            g_hide_first_person = false;
            g_layer_move = 0;
            g_main_for = 0;
            g_flag_addr = 0;
            g_flag_bit = -1;
            g_enabled_getter = g_enabled_setter = 0;
            g_accessors_resolved = false;
            g_materials.clear();
            g_logged_material = false;
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
        g_active = true;
        g_busy = true;
        std::thread([bindings, camera] { pass(bindings, camera); g_busy = false; }).detach();
    }

    inline void wait() { while (g_busy.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
