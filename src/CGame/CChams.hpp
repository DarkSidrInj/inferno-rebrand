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
// no shader can be compiled, but Unity ships one: the default UI material (UI/Default). It has
// ZWrite Off, sits in the Transparent queue (drawn after the opaque walls) and takes its depth
// test from unity_GUIZTestMode, which nothing sets on the shared asset, so it reads 0 =
// CompareFunction.Disabled: no depth test. On an enemy renderer it draws flat white over the
// walls. Its native pointer comes from tracing Canvas.GetDefaultCanvasMaterial_Injected (uGUI
// calls it for every default Graphic, so the wrapper's icall slot is normally filled).

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CIcalls.hpp"
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
    inline bool g_logged_material = false;
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
        auto goal = [](const a64::Machine& m) { return m.returned && m.x[0].known && plausible(m.x[0].value); };
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
        diag::log("WARN", "CHAMS default UI material not found (%s); through walls unavailable", why.c_str());
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

    inline int32_t desired_id(uint64_t bindings) {
        if (cfg::chams::through_walls && g_ui_material.load()) return g_ui_material.load();
        if (cfg::chams::material == 0 || !bindings) return 0;
        const uint64_t field = cfg::chams::material == 1 ? v100::bindings_untouchable_material : v100::bindings_outline_material;
        const uint64_t native = native_of(rpm<uint64_t>(bindings + field));
        const int32_t id = native ? rpm<int32_t>(native + v100::native_instance_id) : 0;
        if (!g_logged_material) {
            g_logged_material = true;
            diag::log("CHAMS", "material %d -> instance id %d%s", cfg::chams::material, id, id ? "" : " (missing, flat used)");
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

    // Once per frame with every player of the match.
    inline void tick(const std::vector<uint64_t>& players, uint64_t local, int local_team) {
        if (g_pid != proc::pid) {
            if (g_resolving.load()) return;
            g_pid = proc::pid;
            g_data_offset = -1;
            g_size_offset = -1;
            g_ui_material = 0;
            g_patched.clear();
            g_last_resolve = 0;
            g_logged_material = false;
        }
        if (!cfg::chams::enabled) { if (!g_patched.empty()) restore_all(); return; }
        if (g_resolving.load()) return;
        const long long now = diag::now_ms();

        if (g_data_offset.load() < 0) {
            if (now - g_last_resolve < 5000) return;
            uint64_t sample = 0;
            for (uint64_t p : players) if (p != local && (sample = renderer_of(p))) break;
            if (!sample) return;
            g_last_resolve = now;
            g_resolving = true;
            std::thread([sample] { resolve(sample); g_resolving = false; }).detach();
            return;
        }

        if (now - g_last_apply < 200) return;
        g_last_apply = now;
        const int data_off = g_data_offset.load(), size_off = g_size_offset.load();
        std::unordered_map<uint64_t, bool> seen;
        for (uint64_t p : players) {
            if (!p || p == local) continue;
            const int team = player::team(p);
            if (cfg::chams::team_check && team == local_team) continue;
            if (player::health(p) <= 0) continue;
            const uint64_t bindings = bindings_of(p);
            const uint64_t renderer = bindings ? native_of(rpm<uint64_t>(bindings + v100::bindings_character_renderer)) : 0;
            if (!renderer) continue;
            const uint64_t array = rpm<uint64_t>(renderer + uint64_t(data_off));
            const int count = size_off >= 0 ? rpm<int32_t>(renderer + uint64_t(size_off)) : 1;
            if (!plausible(array) || count < 1 || count > kMaxSlots) continue;
            const int32_t want = desired_id(bindings);
            int32_t ids[kMaxSlots]{};
            if (!mem_read(array, ids, size_t(count) * 4)) continue;

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
            seen[renderer] = true;
        }
        // Players who left the filter (teammates after a team swap, the dead) get their own
        // materials back; renderers that are gone are just forgotten.
        for (auto it = g_patched.begin(); it != g_patched.end();) {
            if (seen.count(it->first)) { ++it; continue; }
            release(it->first, it->second);
            it = g_patched.erase(it);
        }
    }

    inline void wait() { while (g_resolving.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
