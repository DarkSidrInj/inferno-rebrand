#pragma once
// Material chams, ported from the legacy chams (normal5 SDK) to this build.
//
// Legacy: Player+0x128 CharacterLodGroup -> +0x30 SkinnedMeshRenderer -> +0x10 native ->
// +0x140 material id array, and a 0 written into the first id forces the flat default
// material. Here the renderer comes from the model the skeleton already resolves
// (jgi -> CharacterBindings.<CharacterRenderer>, +0x38), and the material id array inside the
// native renderer is located on a live renderer instead of the hard-coded +0x140 of the old
// Unity: Renderer.GetMaterialCount_Injected is traced (CUtils/CArm64.hpp) to the size of
// m_Materials, and the data pointer is the field before it whose ids read back sane. The
// legacy offset is the last resort.
//
// Material choice: 0 = flat (id 0, the legacy behaviour), or one of the game's own materials
// from CharacterBindings (UntouchableMaterial, OutlineMaterial) by their instance id. Every
// slot of every patched renderer is remembered and put back when chams are switched off.
//
// Through walls: internal chams swap in a shader with ZTest Always / ZWrite Off. From outside
// no shader can be compiled, so an existing material that draws through walls is used:
//   1. the game's own CharacterBindings.OutlineMaterial: the teammate silhouette, made to be
//      seen through walls. Taken from any player whose bindings carry one, so enemies whose own
//      bindings leave it empty get it too.
//   2. Unity's default UI material (UI/Default) as the fallback. It has
// ZWrite Off, sits in the Transparent queue (drawn after the opaque walls) and takes its depth
// test from unity_GUIZTestMode, which nothing sets on the shared asset, so it reads 0 =
// CompareFunction.Disabled: no depth test. On an enemy renderer it draws flat white over the
// walls. Its native pointer comes from tracing Canvas.GetDefaultCanvasMaterial_Injected (uGUI
// calls it for every default Graphic, so the wrapper's icall slot is normally filled).

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
#include "CClass.hpp"
#include "CPlayer.hpp"
#include "CSkeleton.hpp"
#include "../CUtils/CArm64.hpp"
#include "../CConfigs/CConfig.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace chams {
    inline constexpr int kMaxSlots = 16;

    inline int g_pid = -1;
    inline std::atomic<bool> g_resolving{false};
    inline std::atomic<int> g_data_offset{-1};  // m_Materials data pointer inside the native renderer
    inline std::atomic<int> g_size_offset{-1};  // m_Materials size, -1 = unknown (one slot is patched)
    inline long long g_last_resolve = 0, g_last_apply = 0;
    inline std::atomic<int32_t> g_ui_material{0};  // instance id of Unity's default UI material, 0 = unknown
    struct Patched {
        uint64_t array{};                // material id array the originals were read from
        std::vector<int32_t> original;   // per slot
        int32_t written{};               // id we put there
    };
    inline std::unordered_map<uint64_t, Patched> g_patched;  // native renderer -> what we changed

    inline uint64_t bindings_of(uint64_t player) {
        const uint64_t model = skeleton::find_model(player);
        if (!plausible(model)) return 0;
        const uint64_t bindings = rpm<uint64_t>(model + v100::model_bindings);
        return plausible(bindings) ? bindings : 0;
    }

    inline uint64_t native_of(uint64_t managed) {
        if (!plausible(managed)) return 0;
        const uint64_t native = rpm<uint64_t>(managed + v100::unity_cached_ptr);
        return plausible(native) ? native : 0;
    }

    inline int32_t g_shared_ids[3]{};               // material id per mode found on any player (index 1, 2)
    inline int g_logged_source = -1;

    inline int32_t material_id(uint64_t bindings, uint64_t field) {
        if (!bindings) return 0;
        const uint64_t native = native_of(rpm<uint64_t>(bindings + field));
        return native ? rpm<int32_t>(native + v100::native_instance_id) : 0;
    }

    // The game's own materials, from whichever player carries them (teammates usually do).
    inline void collect_shared(const std::vector<uint64_t>& players) {
        for (int mode : {1, 2}) {
            if (g_shared_ids[mode]) continue;
            const uint64_t field = mode == 1 ? v100::bindings_untouchable_material : v100::bindings_outline_material;
            for (uint64_t p : players) {
                const int32_t id = material_id(bindings_of(p), field);
                if (!id) continue;
                g_shared_ids[mode] = id;
                diag::log("CHAMS", "%s material id %d (from player 0x%llx)", mode == 1 ? "untouchable" : "outline", id,
                          (unsigned long long)p);
                break;
            }
        }
    }

    inline uint64_t renderer_of(uint64_t player) {
        const uint64_t bindings = bindings_of(player);
        return bindings ? native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer)) : 0;
    }

    // Material ids are Unity instance ids: non-zero, well inside the int range.
    inline bool ids_sane(uint64_t array, int count) {
        if (!plausible(array) || count < 1 || count > kMaxSlots) return false;
        int32_t ids[kMaxSlots]{};
        if (!mem_read(array, ids, size_t(count) * 4)) return false;
        for (int i = 0; i < count; ++i)
            if (ids[i] == 0 || ids[i] < -0x20000000 || ids[i] > 0x20000000) return false;
        return true;
    }

    // Offset of the size field: the load Renderer::GetMaterialCount returns.
    inline int trace_count(uint64_t entry, uint64_t native, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        start.x[0] = {native, true, 0, 0};
        start.x[1] = {0, true, 0, 0};
        int offset = -1;
        auto goal = [&](const a64::Machine& m) {
            if (!m.returned || !m.x[0].known) return false;
            const uint64_t src = m.x[0].src;
            if (src < native || src >= native + 0x1000) return false;
            const uint64_t count = m.x[0].value & 0xFFFFFFFFULL;
            if (count < 1 || count > uint64_t(kMaxSlots)) return false;
            offset = int(src - native);
            return true;
        };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return offset;
        why = session.why;
        return -1;
    }

    inline uint64_t trace_pointer(uint64_t entry, std::string& why) {
        a64::Session session;
        a64::Machine start;
        start.pc = entry;
        start.x[0] = {0, true, 0, 0};
        // A native Unity object: a vtable pointer first and a non-zero instance id at +8.
        auto goal = [](const a64::Machine& m) {
            if (!m.returned || !m.x[0].known || !plausible(m.x[0].value)) return false;
            return plausible(rpm<uint64_t>(m.x[0].value)) && rpm<int32_t>(m.x[0].value + v100::native_instance_id) != 0;
        };
        a64::Machine found;
        if (a64::explore(session, start, goal, found)) return found.x[0].value;
        why = session.why;
        return 0;
    }

    inline void resolve_ui_material() {
        std::vector<std::pair<uint64_t, const char*>> entries;
        const uint64_t wrapper = proc::lib + v100::canvas_default_material_injected;
        if (!icalls::slot_empty(wrapper)) entries.push_back({wrapper, "wrapper"});
        for (const auto& c : icalls::candidates("UnityEngine.Canvas::GetDefaultCanvasMaterial_Injected")) entries.push_back({c.fn, c.how});
        std::string why = entries.empty() ? "no accessor" : "";
        for (const auto& e : entries) {
            const uint64_t material = trace_pointer(e.first, why);
            const int32_t id = material ? rpm<int32_t>(material + v100::native_instance_id) : 0;
            if (!id) continue;
            g_ui_material = id;
            diag::log("CHAMS", "through-walls material (Unity default UI) native=0x%llx id=%d via %s",
                      (unsigned long long)material, id, e.second);
            return;
        }
        diag::log("WARN", "CHAMS default UI material not found (%s, %zu accessors)", why.empty() ? "no object returned" : why.c_str(),
                  entries.size());
    }

    inline void resolve(uint64_t native) {
        resolve_ui_material();
        std::vector<std::pair<uint64_t, const char*>> entries;
        const uint64_t wrapper = proc::lib + v100::renderer_material_count_injected;
        if (!icalls::slot_empty(wrapper)) entries.push_back({wrapper, "wrapper"});
        for (const auto& c : icalls::candidates("UnityEngine.Renderer::GetMaterialCount_Injected")) entries.push_back({c.fn, c.how});

        std::string why;
        for (const auto& e : entries) {
            const int size_off = trace_count(e.first, native, why);
            if (size_off < 0) continue;
            const int count = rpm<int32_t>(native + uint64_t(size_off));
            for (int delta : {0x10, 0x18, 0x8, 0x20}) {
                const int data_off = size_off - delta;
                if (data_off < 0) continue;
                if (!ids_sane(rpm<uint64_t>(native + uint64_t(data_off)), count)) continue;
                g_size_offset = size_off;
                g_data_offset = data_off;
                diag::log("CHAMS", "m_Materials data +0x%x size +0x%x (%d slots, GetMaterialCount via %s)", data_off, size_off,
                          count, e.second);
                return;
            }
        }
        // No trace: look for {data, ..., size, capacity} with sane ids.
        for (int off = 0x80; off <= 0x300; off += 8) {
            const uint64_t data = rpm<uint64_t>(native + uint64_t(off));
            for (int delta : {0x10, 0x18}) {
                const uint64_t size = rpm<uint64_t>(native + uint64_t(off + delta));
                const uint64_t cap = rpm<uint64_t>(native + uint64_t(off + delta + 8)) & 0x7FFFFFFFFFFFFFFFULL;
                if (size < 1 || size > 8 || cap < size || cap > 64 || !ids_sane(data, int(size))) continue;
                g_size_offset = off + delta;
                g_data_offset = off;
                diag::log("CHAMS", "m_Materials data +0x%x size +0x%x (%llu slots, layout scan; trace: %s)", off, off + delta,
                          (unsigned long long)size, why.empty() ? "no accessor" : why.c_str());
                return;
            }
        }
        if (ids_sane(rpm<uint64_t>(native + v100::legacy_renderer_materials), 1)) {
            g_size_offset = -1;
            g_data_offset = int(v100::legacy_renderer_materials);
            diag::log("CHAMS", "m_Materials at the legacy +0x140 (first slot only)");
            return;
        }
        diag::log("WARN", "CHAMS material array not found in renderer 0x%llx (%s)", (unsigned long long)native, why.c_str());
    }

    inline int32_t desired_id(uint64_t bindings, bool through_walls = true) {
        int source = 0;
        int32_t id = 0;
        if (through_walls && cfg::chams::through_walls) {
            if ((id = g_shared_ids[2])) source = 2;                   // the game's x-ray (outline) material
            else if ((id = g_ui_material.load())) source = 3;         // Unity's default UI material
        }
        if (!id && cfg::chams::material != 0) {
            const int mode = cfg::chams::material;
            id = material_id(bindings, mode == 1 ? v100::bindings_untouchable_material : v100::bindings_outline_material);
            if (!id) id = g_shared_ids[mode];
            source = id ? mode : 0;
        }
        if (source != g_logged_source) {
            g_logged_source = source;
            static const char* names[] = {"flat (id 0)", "untouchable", "outline", "Unity default UI"};
            diag::log("CHAMS", "material: %s, id %d%s", names[source], id,
                      cfg::chams::through_walls && source < 2 ? " (no through-walls material found)" : "");
        }
        return id;
    }

    // Puts one renderer back if it still holds what we wrote.
    inline void release(uint64_t renderer, const Patched& p) {
        const int offset = g_data_offset.load();
        if (offset < 0 || rpm<uint64_t>(renderer + uint64_t(offset)) != p.array) return;  // re-created: nothing of ours left
        for (size_t i = 0; i < p.original.size(); ++i) {
            const uint64_t slot = p.array + i * 4;
            if (rpm<int32_t>(slot) == p.written) wpm<int32_t>(slot, p.original[i]);
        }
    }

    inline void restore_all() {
        for (const auto& entry : g_patched) release(entry.first, entry.second);
        if (!g_patched.empty()) diag::log("CHAMS", "restored %zu renderers", g_patched.size());
        g_patched.clear();
    }

    // ---- local weapon -----------------------------------------------------------------------------
    // The weapon's client content (class gby) holds its renderers, List<MeshRenderer> at +0x30 and
    // +0x40, and the owner's CharacterBindings at +0x48. Nothing on the player points back at it,
    // so it is found by its owner: memory is searched for the local player's bindings pointer at
    // +0x48 of an object whose Il2Cpp class is gby. Runs on the worker, again every 10 s for
    // weapon switches.
    inline constexpr uint64_t kWeaponBindings = 0x48;
    inline constexpr uint64_t kWeaponLists[] = {0x30, 0x40};
    inline std::vector<uint64_t> g_weapon_objects;     // gby instances of the local player
    inline uint64_t g_weapon_owner = 0;                // bindings they were found for
    inline long long g_last_weapon_scan = 0;
    inline std::atomic<bool> g_weapon_scanning{false};

    inline void weapon_renderers(uint64_t object, std::vector<uint64_t>& out) {
        for (uint64_t field : kWeaponLists) {
            const uint64_t list = rpm<uint64_t>(object + field);
            if (!plausible(list)) continue;
            const uint64_t items = rpm<uint64_t>(list + v100::list_items);
            const int count = rpm<int>(list + v100::list_count);
            if (!plausible(items) || count < 1 || count > 64) continue;
            for (int i = 0; i < count; ++i)
                if (const uint64_t native = native_of(rpm<uint64_t>(items + v100::array_data + uint64_t(i) * 8))) out.push_back(native);
        }
    }

    inline std::vector<uint64_t> scan_weapons(uint64_t bindings) {
        std::vector<uint64_t> found;
        if (g_scan_maps.empty()) refresh_scan_maps();
        const std::vector<remote_map> maps = g_scan_maps;
        std::vector<uint64_t> words;
        for (const auto& m : maps) {
            for (uint64_t at = m.begin; at < m.end;) {
                const size_t chunk = size_t(std::min<uint64_t>(m.end - at, 1024 * 1024)) & ~size_t(7);
                if (chunk < 8) break;
                words.assign(chunk / 8, 0);
                if (mem_read(at, words.data(), chunk)) {
                    for (size_t i = 0; i < words.size(); ++i) {
                        if (words[i] != bindings) continue;
                        const uint64_t object = at + i * 8 - kWeaponBindings;
                        if (object < m.begin || !klass::is_named(object, "gby")) continue;
                        std::vector<uint64_t> renderers;
                        weapon_renderers(object, renderers);
                        if (!renderers.empty()) found.push_back(object);
                    }
                }
                at += chunk;
            }
        }
        return found;
    }

    // ---- targets ------------------------------------------------------------------------------
    enum Target { Enemy, Teammate, LocalBody, LocalArms, LocalWeapon, kTargets };
    inline const char* const kTargetNames[kTargets] = {"enemy", "teammate", "local body", "local arms", "local weapon"};
    inline bool target_on(Target t) {
        switch (t) {
            case Enemy: return cfg::chams::enemies;
            case Teammate: return cfg::chams::teammates;
            case LocalBody: return cfg::chams::local_body;
            case LocalArms: return cfg::chams::local_arms;
            default: return cfg::chams::local_weapon;
        }
    }

    // Puts `want` into every slot of one renderer, remembering what was there.
    inline bool patch(uint64_t renderer, int32_t want) {
        const int data_off = g_data_offset.load(), size_off = g_size_offset.load();
        const uint64_t array = rpm<uint64_t>(renderer + uint64_t(data_off));
        const int count = size_off >= 0 ? rpm<int32_t>(renderer + uint64_t(size_off)) : 1;
        if (!plausible(array) || count < 1 || count > kMaxSlots) return false;
        int32_t ids[kMaxSlots]{};
        if (!mem_read(array, ids, size_t(count) * 4)) return false;
        auto it = g_patched.find(renderer);
        if (it != g_patched.end() && it->second.array != array) { g_patched.erase(it); it = g_patched.end(); }  // re-created
        if (it == g_patched.end()) {
            Patched fresh;
            fresh.array = array;
            fresh.original.assign(ids, ids + count);
            it = g_patched.emplace(renderer, fresh).first;
        }
        it->second.written = want;
        for (int i = 0; i < count && i < int(it->second.original.size()); ++i)
            if (ids[i] != want) wpm<int32_t>(array + uint64_t(i) * 4, want);
        return true;
    }

    inline int g_logged_targets[kTargets] = {-1, -1, -1, -1, -1};

    // Once per frame with every player of the match.
    inline void tick(const std::vector<uint64_t>& players, uint64_t local, int local_team) {
        if (g_pid != proc::pid) {
            if (g_resolving.load() || g_weapon_scanning.load()) return;
            g_pid = proc::pid;
            g_data_offset = -1;
            g_size_offset = -1;
            g_ui_material = 0;
            g_shared_ids[1] = g_shared_ids[2] = 0;
            g_logged_source = -1;
            g_patched.clear();
            g_last_resolve = 0;
            g_weapon_objects.clear();
            g_weapon_owner = 0;
            g_last_weapon_scan = 0;
            for (int& t : g_logged_targets) t = -1;
        }
        if (!cfg::chams::enabled) { if (!g_patched.empty()) restore_all(); return; }
        if (g_resolving.load()) return;
        const long long now = diag::now_ms();

        if (g_data_offset.load() < 0) {
            if (now - g_last_resolve < 5000) return;
            uint64_t sample = 0;
            for (uint64_t p : players) if ((sample = renderer_of(p))) break;
            if (!sample) return;
            g_last_resolve = now;
            g_resolving = true;
            std::thread([sample] { resolve(sample); g_resolving = false; }).detach();
            return;
        }

        // Local weapon objects: found on the worker, refreshed for weapon switches.
        const uint64_t local_bindings = local ? bindings_of(local) : 0;
        if (cfg::chams::local_weapon && local_bindings && !g_weapon_scanning.load() &&
            (local_bindings != g_weapon_owner || now - g_last_weapon_scan > 10000)) {
            g_last_weapon_scan = now;
            g_weapon_scanning = true;
            std::thread([local_bindings] {
                std::vector<uint64_t> found = scan_weapons(local_bindings);
                diag::log("CHAMS", "local weapon: %zu weapon content objects", found.size());
                g_weapon_objects = found;
                g_weapon_owner = local_bindings;
                g_weapon_scanning = false;
            }).detach();
        }

        if (now - g_last_apply < 200) return;
        g_last_apply = now;
        collect_shared(players);
        std::unordered_map<uint64_t, bool> seen;
        int patched[kTargets]{};
        auto apply = [&](Target t, uint64_t renderer, uint64_t bindings) {
            if (!renderer || !target_on(t)) return;
            const bool through_walls = t == Enemy || t == Teammate;
            if (patch(renderer, desired_id(bindings, through_walls))) { seen[renderer] = true; ++patched[t]; }
        };

        for (uint64_t p : players) {
            if (!p || player::health(p) <= 0) continue;
            const uint64_t bindings = bindings_of(p);
            if (!bindings) continue;
            const uint64_t body = native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer));
            if (p == local) {
                apply(LocalBody, body, bindings);
                apply(LocalArms, native_of(rpm<uint64_t>(bindings + v100::bindings_arms_renderer)), bindings);
                apply(LocalArms, native_of(rpm<uint64_t>(bindings + v100::bindings_gloves_renderer)), bindings);
                continue;
            }
            apply(player::team(p) == local_team ? Teammate : Enemy, body, bindings);
        }
        if (cfg::chams::local_weapon && !g_weapon_scanning.load()) {
            for (uint64_t object : g_weapon_objects) {
                if (rpm<uint64_t>(object + kWeaponBindings) != local_bindings) continue;  // freed or reused
                std::vector<uint64_t> renderers;
                weapon_renderers(object, renderers);
                for (uint64_t r : renderers) apply(LocalWeapon, r, local_bindings);
            }
        }
        for (int t = 0; t < kTargets; ++t) {
            if (patched[t] == g_logged_targets[t] || !target_on(Target(t))) continue;
            g_logged_targets[t] = patched[t];
            diag::log("CHAMS", "%s: %d renderers", kTargetNames[t], patched[t]);
        }
        // Renderers no longer targeted get their own materials back; ones that are gone are forgotten.
        for (auto it = g_patched.begin(); it != g_patched.end();) {
            if (seen.count(it->first)) { ++it; continue; }
            release(it->first, it->second);
            it = g_patched.erase(it);
        }
    }

    inline void wait() {
        while (g_resolving.load() || g_weapon_scanning.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
