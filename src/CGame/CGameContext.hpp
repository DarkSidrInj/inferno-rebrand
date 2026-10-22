#pragma once

#include "../CUtils/CMemory.hpp"
#include "../CProtection/COxorany.hpp"
#include <stdint.h>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <cctype>

struct matrix {
    float m11, m12, m13, m14;
    float m21, m22, m23, m24;
    float m31, m32, m33, m34;
    float m41, m42, m43, m44;
};

namespace v100 {
    // Standoff 2 1.0.0 ARM64 profile, shared with LemmingRMT v16.
    inline constexpr uint64_t registry_typeinfo = 0xC203D50;
    inline constexpr uint64_t store_typeinfo = 0xC204210;
    inline constexpr uint64_t player_typeinfo = 0xC202058;
    // PlayerLoopHelper TypeInfo RVA supplied from the matching 1.0.0 dump.
    inline constexpr uint64_t player_loop_helper_typeinfo = 0xC1FCEC8;

    // External mirror of the internal GameMgr-based entity walk.
    // The static-fields pointer is probed at the supplied profile offset first;
    // 0xB8 is a compatibility candidate for common IL2CPP class layouts.
    inline constexpr uint64_t il2cpp_class_static_fields_profile = 0x40;
    inline constexpr uint64_t il2cpp_class_static_fields_compat = 0xB8;
    inline constexpr uint64_t helper_runners = 0x0;
    inline constexpr uint64_t runner_items = 0x10;
    inline constexpr uint64_t tickable_entries = 0x10;
    inline constexpr uint64_t game_mgr_world = 0x10;
    inline constexpr uint64_t world_registry = 0x40;
    inline constexpr uint64_t store_entities = 0x18;
    inline constexpr uint64_t wrapped_array_data = 0x8;
    inline constexpr uint64_t transforms_typeinfo = 0xC2031A8;
    inline constexpr uint64_t model_typeinfo = 0xC204D68;
    inline constexpr uint64_t stats_typeinfo = 0xC209D38;
    inline constexpr uint64_t weapon_part_typeinfo = 0xC2064A8;
    inline constexpr uint64_t camera_typeinfo = 0xC1F4198;
    inline constexpr uint64_t ios_typeinfo = 0xC2049C0;
    inline constexpr uint64_t player_string_literal = 0xC294858;
    inline constexpr uint64_t data_relro_begin = 0xB886F80;
    inline constexpr uint64_t data_end = 0xC46CF80;
    inline constexpr uint64_t text_begin = 0x535F940;
    inline constexpr uint64_t text_end = 0xB882E00;
    inline constexpr uint64_t native_camera_get_main = 0x54C1338;

    inline constexpr uint64_t registry_store = 0x30;
    inline constexpr uint64_t store_objects = 0x18;
    inline constexpr uint64_t list_items = 0x10;
    inline constexpr uint64_t list_count = 0x18;
    inline constexpr uint64_t array_length = 0x18;
    inline constexpr uint64_t array_data = 0x20;
    inline constexpr uint64_t player_local = 0x44;
    inline constexpr uint64_t player_parts = 0x28;
    inline constexpr uint64_t player_world = 0x18;
    inline constexpr uint64_t net_object_id = 0x10;
    inline constexpr uint64_t player_stats = 0x70;
    inline constexpr uint64_t player_team = 0x90;
    inline constexpr uint64_t stats_hp = 0x10;
    inline constexpr uint64_t stats_max_hp = 0x14;
    inline constexpr uint64_t parts_list = 0x10;
    inline constexpr uint64_t transform_a = 0x28;
    inline constexpr uint64_t transform_b = 0x30;
    inline constexpr uint64_t model_transform = 0x20;
    inline constexpr uint64_t unity_cached_ptr = 0x10;
    inline constexpr uint64_t camera_view = 0x70;
    inline constexpr uint64_t camera_projection = 0xB0;
}

struct remote_map { uint64_t begin{}, end{}; bool writable{}; };
inline std::vector<remote_map> g_scan_maps;
// Writable mappings used only to accept camera pointers. Refreshed on its own, so a
// camera created after the lobby (the match camera) is not rejected as out of range,
// and the registry scan cursors are never reset by it.
inline std::vector<remote_map> g_camera_maps;
inline long long g_camera_maps_stamp = 0;
inline size_t g_registry_map = 0, g_camera_map = 0;
inline uint64_t g_registry_cursor = 0, g_camera_cursor = 0;
inline uint64_t g_camera_next_address = 0;
inline uint64_t g_registry = 0, g_camera = 0;
inline uint64_t g_game_mgr = 0, g_player_loop_helper_class = 0, g_player_loop_runners = 0;
inline int g_scan_pid = -1;
inline uint64_t g_scan_lib = 0;
inline uint64_t g_registry_scanned = 0, g_camera_scanned = 0;
inline unsigned g_registry_cycles = 0, g_camera_cycles = 0;
struct player_cache_entry { uint32_t id{}; uint64_t object{}; long long seen_ms{}; };
inline std::vector<player_cache_entry> g_player_cache;
inline size_t g_player_scan_map = 0;
inline uint64_t g_player_scan_cursor = 0;
inline uint64_t g_player_scan_bytes = 0;
// synthetic = built from the ios transforms (fixed FOV, no shot animation); a
// native candidate is the game's own Camera with live view and projection.
struct camera_candidate { uint64_t address{}; matrix view{}; matrix projection{}; bool synthetic{false}; };
inline std::vector<camera_candidate> g_camera_candidates;
struct vtable_check { uint64_t address{}; bool valid{}; };
inline std::vector<vtable_check> g_vtable_checks;
inline bool g_get_main_dumped = false;

inline void reset() {
    g_registry = g_camera = 0;
    g_game_mgr = g_player_loop_helper_class = g_player_loop_runners = 0;
    g_scan_maps.clear();
    g_registry_map = g_camera_map = 0; g_registry_cursor = g_camera_cursor = 0;
    g_camera_next_address = 0;
    g_registry_scanned = g_camera_scanned = 0;
    g_registry_cycles = g_camera_cycles = 0;
    g_player_cache.clear();
    g_camera_candidates.clear();
    g_vtable_checks.clear();
    g_get_main_dumped = false;
    g_player_scan_map = 0; g_player_scan_cursor = 0; g_player_scan_bytes = 0;
}

inline void ensure_scan_context() {
    if (g_scan_pid != proc::pid || g_scan_lib != proc::lib) {
        reset();
        g_scan_pid = proc::pid;
        g_scan_lib = proc::lib;
        diag::log("INFO", "scan context reset pid=%d libunity=0x%llx", proc::pid,
                  (unsigned long long)proc::lib);
    }
}

inline bool plausible(uint64_t p) { return p >= 0x10000 && p < 0x0001000000000000ULL; }
inline bool exact_class(uint64_t object, uint64_t klass) {
    return object && klass && rpm<uint64_t>(object) == klass;
}

inline void refresh_scan_maps() {
    g_scan_maps.clear();
    if (proc::pid <= 0) return;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
    FILE* file = std::fopen(path, "r");
    if (!file) {
        diag::log("ERROR", "cannot open %s errno=%d", path, errno);
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || end <= begin) continue;
        const uint64_t size = end - begin;
        if (size < 0x1000 || size > 512ULL * 1024 * 1024) continue;
        // Managed/native objects live in private writable mappings. Avoid files and stacks.
        if (std::strchr(line, '/') || std::strstr(line, "[stack")) continue;
        g_scan_maps.push_back({begin, end, true});
    }
    std::fclose(file);
    g_registry_map = g_camera_map = 0;
    g_registry_cursor = g_camera_cursor = 0;
    g_player_scan_map = 0;
    g_player_scan_cursor = 0;
    uint64_t total = 0;
    for (const auto& map : g_scan_maps) total += map.end - map.begin;
    diag::log("INFO", "scan map snapshot: ranges=%zu eligible=%.1f MiB", g_scan_maps.size(),
              double(total) / 1048576.0);
}

inline bool validate_registry(uint64_t candidate, uint64_t registry_class, uint64_t store_class) {
    if (!exact_class(candidate, registry_class)) return false;
    uint64_t store = rpm<uint64_t>(candidate + v100::registry_store);
    if (!exact_class(store, store_class)) return false;
    uint64_t list = rpm<uint64_t>(store + v100::store_objects);
    int count = rpm<int>(list + v100::list_count);
    uint64_t array = rpm<uint64_t>(list + v100::list_items);
    uint64_t length = rpm<uint64_t>(array + v100::array_length);
    return list && count >= 0 && count <= 4096 && (count == 0 || array) && length >= unsigned(count) && length <= 8192;
}

inline bool scan_registry(size_t budget = 4 * 1024 * 1024) {
    ensure_scan_context();
    const uint64_t registry_class = rpm<uint64_t>(proc::lib + v100::registry_typeinfo);
    const uint64_t store_class = rpm<uint64_t>(proc::lib + v100::store_typeinfo);
    static long long type_log = 0;
    if (diag::every(type_log, 5000))
        diag::log(registry_class && store_class ? "INFO" : "ERROR",
                  "TypeInfo registry=0x%llx store=0x%llx player=0x%llx transforms=0x%llx",
                  (unsigned long long)registry_class, (unsigned long long)store_class,
                  (unsigned long long)rpm<uint64_t>(proc::lib + v100::player_typeinfo),
                  (unsigned long long)rpm<uint64_t>(proc::lib + v100::transforms_typeinfo));
    if (!registry_class || !store_class) return false;
    if (validate_registry(g_registry, registry_class, store_class)) return true;
    g_registry = 0;
    if (g_scan_maps.empty()) refresh_scan_maps();
    std::vector<uint64_t> words;
    size_t consumed = 0;
    while (consumed < budget && g_registry_map < g_scan_maps.size()) {
        const auto& map = g_scan_maps[g_registry_map];
        uint64_t cursor = g_registry_cursor ? g_registry_cursor : map.begin;
        size_t bytes = size_t(std::min<uint64_t>(budget - consumed, std::min<uint64_t>(map.end - cursor, 1024 * 1024)));
        bytes &= ~size_t(7);
        if (bytes < 16) { ++g_registry_map; g_registry_cursor = 0; continue; }
        words.resize(bytes / 8);
        if (mem_read(cursor, words.data(), bytes)) {
            for (size_t i = 0; i + 1 < words.size(); ++i) {
                if (words[i] == registry_class && validate_registry(cursor + i * 8, registry_class, store_class)) {
                    g_registry = cursor + i * 8;
                    diag::log("INFO", "NetObject registry found at 0x%llx after %.1f MiB",
                              (unsigned long long)g_registry, double(g_registry_scanned) / 1048576.0);
                    return true;
                }
            }
        }
        cursor += bytes; consumed += bytes; g_registry_scanned += bytes;
        if (cursor >= map.end) { ++g_registry_map; g_registry_cursor = 0; }
        else g_registry_cursor = cursor;
    }
    if (g_registry_map >= g_scan_maps.size()) {
        g_registry_map = 0; g_registry_cursor = 0; ++g_registry_cycles;
        diag::log("WARN", "registry scan cycle %u complete; no valid registry (%.1f MiB read)",
                  g_registry_cycles, double(g_registry_scanned) / 1048576.0);
        refresh_scan_maps();
    }
    return false;
}

// Reads the PlayerLoopHelper -> Runner -> TickableLoopItem -> GameMgr chain
// from the target process. This deliberately uses mem_read/rpm, never local
// pointer dereferences or IL2CPP API calls in the external overlay process.
namespace entitywalk {
struct Snapshot {
    uint64_t game_mgr{};
    uint64_t world{};
    uint64_t registry{};
    uint64_t store{};
    uint64_t entities_wrapper{};
    uint64_t entities_array{};
    int entity_count{};
};

inline bool array_count(uint64_t array, uint64_t max_count, int& out_count,
                        bool allow_empty = true) {
    out_count = 0;
    if (!plausible(array)) return false;
    uint64_t length = 0;
    if (!mem_read(array + v100::array_length, &length, sizeof(length))) return false;
    if (length > max_count || (!allow_empty && length == 0)) return false;
    out_count = static_cast<int>(length);
    return true;
}

inline uint64_t array_item(uint64_t array, int index) {
    if (!plausible(array) || index < 0) return 0;
    return rpm<uint64_t>(array + v100::array_data + uint64_t(index) * sizeof(uint64_t));
}

inline bool runner_array_shape(uint64_t runners) {
    int runner_count = 0;
    if (!array_count(runners, 64, runner_count, false)) return false;

    // Validate at least one Runner.Items container before trusting a candidate
    // static-fields offset. This prevents a random class field being mistaken
    // for the runners array.
    const int probe_count = std::min(runner_count, 8);
    for (int i = 0; i < probe_count; ++i) {
        const uint64_t runner = array_item(runners, i);
        if (!plausible(runner)) continue;
        const uint64_t items_wrapper = rpm<uint64_t>(runner + v100::runner_items);
        if (!plausible(items_wrapper)) continue;
        uint64_t items_array = rpm<uint64_t>(items_wrapper + v100::wrapped_array_data);
        int item_count = 0;
        if (!array_count(items_array, 1024, item_count, false)) {
            // Compatibility path for wrappers whose array field is at +0x10.
            items_array = rpm<uint64_t>(items_wrapper + v100::list_items);
            if (!array_count(items_array, 1024, item_count, false)) continue;
        }
        const int item_probe_count = std::min(item_count, 16);
        bool has_valid_item = false;
        for (int j = 0; j < item_probe_count; ++j) {
            if (plausible(array_item(items_array, j))) {
                has_valid_item = true;
                break;
            }
        }
        if (has_valid_item) return true;
    }
    return false;
}

inline uint64_t resolve_runners(uint64_t helper_class) {
    if (!plausible(helper_class)) return 0;

    const uint64_t static_offsets[] = {
        v100::il2cpp_class_static_fields_profile,
        v100::il2cpp_class_static_fields_compat
    };
    for (uint64_t static_offset : static_offsets) {
        const uint64_t static_fields = rpm<uint64_t>(helper_class + static_offset);
        if (!plausible(static_fields)) continue;
        const uint64_t runners = rpm<uint64_t>(static_fields + v100::helper_runners);
        if (!runner_array_shape(runners)) continue;

        static long long last_log = 0;
        if (diag::every(last_log, 4000)) {
            diag::log("INFO", "PlayerLoopHelper resolved class=0x%llx staticFieldsOffset=0x%llx statics=0x%llx runners=0x%llx",
                      (unsigned long long)helper_class,
                      (unsigned long long)static_offset,
                      (unsigned long long)static_fields,
                      (unsigned long long)runners);
        }
        return runners;
    }
    return 0;
}

inline bool manager_snapshot(uint64_t candidate, Snapshot& out) {
    if (!plausible(candidate)) return false;

    const uint64_t world = rpm<uint64_t>(candidate + v100::game_mgr_world);
    if (!plausible(world)) return false;
    const uint64_t registry = rpm<uint64_t>(world + v100::world_registry);
    const uint64_t registry_class = rpm<uint64_t>(proc::lib + v100::registry_typeinfo);
    if (!exact_class(registry, registry_class)) return false;

    const uint64_t store = rpm<uint64_t>(registry + v100::registry_store);
    const uint64_t store_class = rpm<uint64_t>(proc::lib + v100::store_typeinfo);
    if (!exact_class(store, store_class)) return false;

    const uint64_t entities_wrapper = rpm<uint64_t>(store + v100::store_entities);
    if (!plausible(entities_wrapper)) return false;

    uint64_t entities_array = rpm<uint64_t>(entities_wrapper + v100::wrapped_array_data);
    int entity_count = 0;
    if (!array_count(entities_array, 4096, entity_count)) {
        // Some builds expose the backing managed array directly at the usual
        // List<T>.items field; keep this as a verified fallback, not an assumption.
        entities_array = rpm<uint64_t>(entities_wrapper + v100::list_items);
        if (!array_count(entities_array, 4096, entity_count)) return false;
    }

    out.game_mgr = candidate;
    out.world = world;
    out.registry = registry;
    out.store = store;
    out.entities_wrapper = entities_wrapper;
    out.entities_array = entities_array;
    out.entity_count = entity_count;
    return true;
}

inline bool resolve(Snapshot& out) {
    out = {};
    if (proc::pid <= 0 || !plausible(proc::lib)) return false;

    // Fast path: validate the cached GameMgr against its complete live chain.
    if (g_game_mgr && manager_snapshot(g_game_mgr, out)) {
        g_registry = out.registry;
        return true;
    }
    g_game_mgr = 0;

    const uint64_t helper_class = rpm<uint64_t>(proc::lib + v100::player_loop_helper_typeinfo);
    if (!plausible(helper_class)) {
        static long long type_log = 0;
        if (diag::every(type_log, 5000))
            diag::log("WAIT", "PlayerLoopHelper TypeInfo unresolved rva=0x%llx base=0x%llx value=0x%llx",
                      (unsigned long long)v100::player_loop_helper_typeinfo,
                      (unsigned long long)proc::lib,
                      (unsigned long long)helper_class);
        return false;
    }
    g_player_loop_helper_class = helper_class;

    if (!runner_array_shape(g_player_loop_runners))
        g_player_loop_runners = resolve_runners(helper_class);
    int runner_count = 0;
    if (!array_count(g_player_loop_runners, 64, runner_count, false)) {
        static long long runner_log = 0;
        if (diag::every(runner_log, 5000))
            diag::log("WAIT", "PlayerLoopHelper runners not resolved helper=0x%llx",
                      (unsigned long long)helper_class);
        return false;
    }

    const int runners_to_check = std::min(runner_count, 64);
    for (int r = 0; r < runners_to_check; ++r) {
        const uint64_t runner = array_item(g_player_loop_runners, r);
        if (!plausible(runner)) continue;

        const uint64_t items_wrapper = rpm<uint64_t>(runner + v100::runner_items);
        if (!plausible(items_wrapper)) continue;
        uint64_t items_array = rpm<uint64_t>(items_wrapper + v100::wrapped_array_data);
        int item_count = 0;
        if (!array_count(items_array, 1024, item_count)) {
            items_array = rpm<uint64_t>(items_wrapper + v100::list_items);
            if (!array_count(items_array, 1024, item_count)) continue;
        }

        const int items_to_check = std::min(item_count, 1024);
        for (int i = 0; i < items_to_check; ++i) {
            const uint64_t item = array_item(items_array, i);
            if (!plausible(item)) continue;

            // Internal reference: TickableLoopItem.Entries is at +0x10.
            // We avoid depending on remote Il2CppClass::name layout: the
            // candidate is accepted only if its entry chain reaches objects
            // whose class pointers exactly match the known Registry and Store.
            const uint64_t entries_array = rpm<uint64_t>(item + v100::tickable_entries);
            int entry_count = 0;
            if (!array_count(entries_array, 1024, entry_count)) continue;

            const int entries_to_check = std::min(entry_count, 1024);
            for (int e = 0; e < entries_to_check; ++e) {
                const uint64_t tick = array_item(entries_array, e);
                Snapshot candidate_snapshot{};
                if (!manager_snapshot(tick, candidate_snapshot)) continue;

                g_game_mgr = tick;
                g_registry = candidate_snapshot.registry;
                out = candidate_snapshot;
                static long long found_log = 0;
                if (diag::every(found_log, 3000)) {
                    diag::log("INFO", "GameMgr chain found mgr=0x%llx world=0x%llx registry=0x%llx store=0x%llx entities=0x%llx count=%d runner=%d item=%d entry=%d",
                              (unsigned long long)out.game_mgr,
                              (unsigned long long)out.world,
                              (unsigned long long)out.registry,
                              (unsigned long long)out.store,
                              (unsigned long long)out.entities_array,
                              out.entity_count, r, i, e);
                }
                return true;
            }
        }
    }

    static long long miss_log = 0;
    if (diag::every(miss_log, 5000))
        diag::log("WAIT", "GameMgr chain not found runners=%d helper=0x%llx; legacy Registry scan remains available",
                  runner_count, (unsigned long long)helper_class);
    return false;
}
} // namespace entitywalk

inline void cache_player(uint64_t object, uint32_t id, long long now) {
    if (!id) id = uint32_t((object >> 4) ^ object);
    // A reconnect/respawn may expose the same object through more than one
    // transient network id. Keep one cache record per actual managed object.
    auto same_object = std::find_if(g_player_cache.begin(), g_player_cache.end(),
        [object](const player_cache_entry& entry) { return entry.object == object; });
    if (same_object != g_player_cache.end()) {
        same_object->id = id;
        same_object->seen_ms = now;
        return;
    }
    auto found = std::find_if(g_player_cache.begin(), g_player_cache.end(),
                              [id](const player_cache_entry& entry) { return entry.id == id; });
    if (found == g_player_cache.end()) g_player_cache.push_back({id, object, now});
    else { found->object = object; found->seen_ms = now; }
}

inline size_t scan_player_heap(uint64_t player_class, uint64_t active_world, long long now,
                               size_t budget = 4 * 1024 * 1024) {
    if (!player_class || !active_world) return 0;
    if (g_scan_maps.empty()) refresh_scan_maps();
    std::vector<uint64_t> words;
    size_t consumed = 0, found = 0;
    while (consumed < budget && g_player_scan_map < g_scan_maps.size()) {
        const auto& map = g_scan_maps[g_player_scan_map];
        uint64_t cursor = g_player_scan_cursor ? g_player_scan_cursor : map.begin;
        size_t bytes = size_t(std::min<uint64_t>(budget - consumed,
            std::min<uint64_t>(map.end - cursor, 1024 * 1024)));
        bytes &= ~size_t(7);
        if (bytes < 16) { ++g_player_scan_map; g_player_scan_cursor = 0; continue; }
        words.resize(bytes / 8);
        if (mem_read(cursor, words.data(), bytes)) {
            for (size_t i = 0; i + 1 < words.size(); ++i) {
                if (words[i] != player_class) continue;
                uint64_t object = cursor + i * 8;
                if (rpm<uint64_t>(object + v100::player_world) != active_world ||
                    !rpm<uint64_t>(object + v100::player_parts) ||
                    !rpm<uint64_t>(object + v100::player_stats)) continue;
                uint8_t team = rpm<uint8_t>(object + v100::player_team);
                uint8_t local = rpm<uint8_t>(object + v100::player_local);
                if ((team != 1 && team != 2) || local > 1) continue;
                uint32_t id = rpm<uint32_t>(object + v100::net_object_id);
                cache_player(object, id, now);
                ++found;
            }
        }
        cursor += bytes; consumed += bytes; g_player_scan_bytes += bytes;
        if (cursor >= map.end) { ++g_player_scan_map; g_player_scan_cursor = 0; }
        else g_player_scan_cursor = cursor;
    }
    if (g_player_scan_map >= g_scan_maps.size()) {
        g_player_scan_map = 0; g_player_scan_cursor = 0;
        diag::log("SCAN", "player heap cycle complete scanned=%.1f MiB cached=%zu",
                  double(g_player_scan_bytes) / 1048576.0, g_player_cache.size());
    } else if (found) {
        diag::log("SCAN", "player heap step found=%zu cached=%zu range=%zu/%zu",
                  found, g_player_cache.size(), g_player_scan_map, g_scan_maps.size());
    }
    return found;
}

inline std::vector<uint64_t> players() {
    std::vector<uint64_t> out;

    entitywalk::Snapshot manager_snapshot{};
    const bool manager_chain = entitywalk::resolve(manager_snapshot);
    uint64_t player_class = rpm<uint64_t>(proc::lib + v100::player_typeinfo);
    uint64_t array = 0;
    int count = 0;
    uint64_t active_world = 0;

    if (manager_chain) {
        // The authoritative array comes from PlayerLoopHelper -> GameMgr.
        g_registry = manager_snapshot.registry;
        array = manager_snapshot.entities_array;
        count = manager_snapshot.entity_count;
        active_world = manager_snapshot.world;
    } else {
        // Keep the existing registry scan as a fallback while the new chain is
        // being validated against live game memory on-device.
        if (!scan_registry()) return out;
        const uint64_t store = rpm<uint64_t>(g_registry + v100::registry_store);
        const uint64_t list = rpm<uint64_t>(store + v100::store_objects);
        count = rpm<int>(list + v100::list_count);
        array = rpm<uint64_t>(list + v100::list_items);
    }

    if (!player_class || count < 0 || count > 4096 || !plausible(array)) {
        diag::log("WARN", "entity source invalid source=%s playerClass=0x%llx count=%d array=0x%llx",
                  manager_chain ? "GameMgr" : "Registry",
                  (unsigned long long)player_class, count, (unsigned long long)array);
        if (manager_chain) g_game_mgr = 0;
        g_registry = 0;
        return out;
    }

    const long long now = diag::now_ms();
    size_t raw_exact = 0;
    std::vector<uint64_t> current_objects;
    for (int i = 0; i < count; ++i) {
        const uint64_t object = rpm<uint64_t>(array + v100::array_data + uint64_t(i) * sizeof(uint64_t));
        if (!exact_class(object, player_class)) continue;
        const uint64_t object_world = rpm<uint64_t>(object + v100::player_world);
        if (!object_world) continue;
        if (manager_chain && object_world != manager_snapshot.world) continue;
        ++raw_exact;
        current_objects.push_back(object);
        if (!active_world || rpm<uint8_t>(object + v100::player_local)) active_world = object_world;
        const uint32_t id = rpm<uint32_t>(object + v100::net_object_id);
        cache_player(object, id, now);
    }
    // The authoritative NetObject registry is complete enough for rendering.
    // Never walk multi-gigabyte anonymous heaps from the UI/render thread: on
    // slower devices that stalls both ESP and menu input for several seconds.
    g_player_cache.erase(std::remove_if(g_player_cache.begin(), g_player_cache.end(),
        [now, player_class, active_world](const player_cache_entry& entry) {
            return now - entry.seen_ms > 2500 || !exact_class(entry.object, player_class) ||
                   !rpm<uint64_t>(entry.object + v100::player_world) ||
                   (active_world && rpm<uint64_t>(entry.object + v100::player_world) != active_world);
        }), g_player_cache.end());
    auto append_unique = [&out](uint64_t object) {
        if (object && std::find(out.begin(), out.end(), object) == out.end()) out.push_back(object);
    };
    // Prefer the authoritative registry snapshot. Heap/cache entries only fill
    // holes, and a 5v5 match can never require more than ten live players.
    for (uint64_t object : current_objects) {
        if (out.size() >= 10) break;
        append_unique(object);
    }
    for (const auto& entry : g_player_cache) {
        if (out.size() >= 10) break;
        append_unique(entry.object);
    }
    static long long player_log = 0;
    if (diag::every(player_log, 5000))
        diag::log("INFO", "entity source=%s count=%d exactNow=%zu cachedPlayers=%zu registry=0x%llx manager=0x%llx",
                  manager_chain ? "GameMgr" : "Registry", count, raw_exact, out.size(),
                  (unsigned long long)g_registry, (unsigned long long)g_game_mgr);
    return out;
}

inline std::string read_managed_string(uint64_t string_object, uint64_t string_class) {
    if (string_object < 0x100000 || !plausible(string_object) || !string_class ||
        rpm<uint64_t>(string_object) != string_class) return {};
    const int32_t length = rpm<int32_t>(string_object + 0x10);
    if (length <= 0 || length > 40) return {};
    std::vector<uint16_t> chars(static_cast<size_t>(length), uint16_t{});
    if (!mem_read(string_object + 0x14, chars.data(), chars.size() * sizeof(uint16_t))) return {};
    std::string out;
    for (uint16_t ch : chars) {
        if (ch < 0x20 || ch == 0x7f) return {};
        if (ch < 0x80) out.push_back(char(ch));
        else if (ch < 0x800) {
            out.push_back(char(0xC0 | (ch >> 6)));
            out.push_back(char(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (ch >> 12)));
            out.push_back(char(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(char(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

inline std::vector<std::string> player_name_candidates(uint64_t player) {
    std::vector<std::string> names;
    const uint64_t known_string = rpm<uint64_t>(proc::lib + v100::player_string_literal);
    const uint64_t string_class = plausible(known_string) ? rpm<uint64_t>(known_string) : 0;
    const uint64_t container = rpm<uint64_t>(player + 0x20);
    if (!string_class || !plausible(container)) return names;
    for (uint64_t field = 0x10; field <= 0x38; field += 8) {
        const uint64_t array = rpm<uint64_t>(container + field);
        const uint64_t count = rpm<uint64_t>(array + v100::array_length);
        if (!plausible(array) || count > 256) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t component = rpm<uint64_t>(array + v100::array_data + i * 8);
            if (!plausible(component)) continue;
            uint64_t fields[30]{};
            if (!mem_read(component + 0x10, fields, sizeof(fields))) continue;
            for (uint64_t value : fields) {
                std::string candidate = read_managed_string(value, string_class);
                if (candidate.empty() || candidate == "Player" || candidate.size() > 96) continue;
                if (std::find(names.begin(), names.end(), candidate) == names.end()) names.push_back(candidate);
                if (names.size() >= 6) return names;
            }
        }
    }
    return names;
}

inline bool noisy_player_name(const std::string& value) {
    if (value.empty() || value == "Player" || value.front() == '_' ||
        value.find('/') != std::string::npos || value.find('<') != std::string::npos ||
        value.find('>') != std::string::npos || value.find("http") != std::string::npos ||
        value.find(" SDF") != std::string::npos || value.find('.') != std::string::npos ||
        value.find('"') != std::string::npos) return true;
    bool hex = value.size() >= 16;
    bool short_lower = value.size() <= 4;
    bool repeated = value.size() >= 8;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(ch)) hex = false;
        if (!(ch >= 'a' && ch <= 'z')) short_lower = false;
        if (i && value[i] != value[0]) repeated = false;
    }
    static const char* ui_words[] = {"Normal", "Highlighted", "Pressed", "Selected",
        "Crashlytics", "Diffuse"};
    for (const char* word : ui_words) if (value == word) return true;
    return hex || short_lower || repeated;
}

inline std::string player_name(uint64_t player) {
    const std::vector<std::string> candidates = player_name_candidates(player);
    for (const std::string& candidate : candidates)
        if (!noisy_player_name(candidate)) return candidate;
    return {};
}

inline bool finite_matrix(const matrix& m) {
    const float* p = reinterpret_cast<const float*>(&m);
    float sum = 0.f;
    for (int i = 0; i < 16; ++i) { if (!std::isfinite(p[i]) || std::fabs(p[i]) > 1000000.f) return false; sum += std::fabs(p[i]); }
    return sum > .01f;
}

inline bool valid_native_vtable(uint64_t vtable) {
    if (vtable < proc::lib + v100::data_relro_begin || vtable >= proc::lib + v100::data_end) return false;
    auto cached = std::find_if(g_vtable_checks.begin(), g_vtable_checks.end(),
        [vtable](const vtable_check& entry) { return entry.address == vtable; });
    if (cached != g_vtable_checks.end()) return cached->valid;
    const uint64_t active_method = rpm<uint64_t>(vtable + 0xD0);
    const bool valid = active_method >= proc::lib + v100::text_begin &&
                       active_method < proc::lib + v100::text_end;
    g_vtable_checks.push_back({vtable, valid});
    return valid;
}

inline bool validate_camera(uint64_t camera, matrix& view, matrix& projection, bool strict_vtable = false) {
    uint64_t vtable = rpm<uint64_t>(camera);
    uint64_t game_object = rpm<uint64_t>(camera + 0x20);
    if (vtable < proc::lib + v100::data_relro_begin || vtable >= proc::lib + v100::data_end ||
        !plausible(game_object) || (strict_vtable && !valid_native_vtable(vtable))) return false;
    if (!mem_read(camera + v100::camera_view, &view, sizeof(view)) || !mem_read(camera + v100::camera_projection, &projection, sizeof(projection))) return false;
    const float* v = reinterpret_cast<const float*>(&view);
    const float* p = reinterpret_cast<const float*>(&projection);
    return finite_matrix(view) && finite_matrix(projection) && std::fabs(v[3]) < .001f && std::fabs(v[7]) < .001f &&
           std::fabs(v[11]) < .001f && std::fabs(v[15] - 1.f) < .01f && std::fabs(p[0]) > .001f && std::fabs(p[5]) > .001f;
}

inline int64_t sign_extend(uint64_t value, unsigned bits) {
    const uint64_t sign = 1ULL << (bits - 1);
    return int64_t((value ^ sign) - sign);
}

inline void remember_camera(uint64_t address, const matrix& view, const matrix& projection);

inline void refresh_camera_maps() {
    g_camera_maps.clear();
    g_camera_maps_stamp = diag::now_ms();
    if (proc::pid <= 0) return;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", proc::pid);
    FILE* file = std::fopen(path, "r");
    if (!file) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, perms) != 3) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || end <= begin) continue;
        if (end - begin < 0x1000 || std::strstr(line, "[stack")) continue;
        g_camera_maps.push_back({begin, end, true});
    }
    std::fclose(file);
}

inline bool in_camera_maps(uint64_t address) {
    return std::any_of(g_camera_maps.begin(), g_camera_maps.end(),
        [address](const remote_map& map) { return address >= map.begin && address < map.end; });
}

inline bool writable_scan_address(uint64_t address) {
    if (in_camera_maps(address)) return true;
    // Miss: the mapping may be newer than the snapshot. Re-read at most every 500 ms.
    if (diag::now_ms() - g_camera_maps_stamp < 500) return false;
    refresh_camera_maps();
    return in_camera_maps(address);
}

inline void probe_camera_pointer(uint64_t pointer) {
    if (!plausible(pointer) || !writable_scan_address(pointer)) return;
    matrix view{}, projection{};
    if (validate_camera(pointer, view, projection)) {
        remember_camera(pointer, view, projection);
        return;
    }
    // Camera::GetMain may reference a manager, vector, handle, or singleton
    // rather than the Camera directly. Walk only small, bounded pointer tables.
    uint64_t children[65]{};
    if (!mem_read(pointer, children, sizeof(children))) return;
    for (size_t index = 0; index < 65; ++index) {
        const uint64_t child = children[index];
        if (validate_camera(child, view, projection)) {
            remember_camera(child, view, projection);
            continue;
        }
        if (!plausible(child)) continue;
        uint64_t grandchildren[17]{};
        if (!mem_read(child, grandchildren, sizeof(grandchildren))) continue;
        for (uint64_t grandchild : grandchildren) {
            if (validate_camera(grandchild, view, projection))
                remember_camera(grandchild, view, projection);
        }
    }
}

inline void discover_camera_get_main() {
    uint64_t start = proc::lib + v100::native_camera_get_main;
    // Resolve small ELF/Unity tail-call veneers before decoding global loads.
    for (int hop = 0; hop < 4; ++hop) {
        const uint32_t first = rpm<uint32_t>(start);
        if ((first & 0x7C000000U) != 0x14000000U) break;
        const int64_t displacement = sign_extend(first & 0x03FFFFFFU, 26) * 4;
        const uint64_t target = start + uint64_t(displacement);
        if (target < proc::lib + v100::text_begin || target >= proc::lib + v100::text_end) break;
        start = target;
    }
    uint32_t code[64]{};
    if (!mem_read(start, code, sizeof(code))) return;
    uint64_t registers[32]{};
    bool known[32]{};
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t insn = code[i];
        const uint64_t pc = start + i * 4;
        if ((insn & 0x9F000000U) == 0x90000000U) { // ADRP
            const unsigned rd = insn & 31U;
            const uint64_t imm21 = (uint64_t((insn >> 5) & 0x7FFFFU) << 2) |
                                   uint64_t((insn >> 29) & 3U);
            registers[rd] = (pc & ~0xFFFULL) + uint64_t(sign_extend(imm21, 21) * 4096LL);
            known[rd] = true;
        } else if ((insn & 0xFF000000U) == 0x91000000U) { // ADD Xd, Xn, #imm
            const unsigned rd = insn & 31U, rn = (insn >> 5) & 31U;
            if (known[rn]) {
                uint64_t imm = (insn >> 10) & 0xFFFU;
                if ((insn >> 22) & 1U) imm <<= 12;
                registers[rd] = registers[rn] + imm;
                known[rd] = true;
            }
        } else if ((insn & 0xFFC00000U) == 0xF9400000U) { // LDR Xt, [Xn,#imm]
            const unsigned rt = insn & 31U, rn = (insn >> 5) & 31U;
            if (known[rn]) {
                const uint64_t slot = registers[rn] + uint64_t((insn >> 10) & 0xFFFU) * 8;
                const uint64_t value = rpm<uint64_t>(slot);
                registers[rt] = value;
                known[rt] = plausible(value);
                probe_camera_pointer(value);
                if (!g_get_main_dumped)
                    diag::log("CAMERA", "GetMain global slot=0x%llx value=0x%llx insn=%08x",
                              (unsigned long long)slot, (unsigned long long)value, insn);
            }
        }
    }
    if (!g_get_main_dumped) {
        diag::log("CAMERA", "GetMain RVA=0x%llx words=%08x %08x %08x %08x %08x %08x %08x %08x",
                  (unsigned long long)v100::native_camera_get_main,
                  code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
        g_get_main_dumped = true;
    }
}

inline uint64_t find_player_component(uint64_t player, uint64_t component_class) {
    if (!player || !component_class) return 0;
    const uint64_t container = rpm<uint64_t>(player + 0x20);
    if (!plausible(container)) return 0;
    for (uint64_t field = 0x10; field <= 0x38; field += 8) {
        const uint64_t array = rpm<uint64_t>(container + field);
        const uint64_t count = rpm<uint64_t>(array + v100::array_length);
        if (!plausible(array) || count > 256) continue;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t component = rpm<uint64_t>(array + v100::array_data + i * 8);
            if (exact_class(component, component_class)) return component;
        }
    }
    return 0;
}

inline void discover_camera_from_local_player(uint64_t local_player) {
    const uint64_t camera_class = rpm<uint64_t>(proc::lib + v100::camera_typeinfo);
    const uint64_t ios_class = rpm<uint64_t>(proc::lib + v100::ios_typeinfo);
    const uint64_t ios = find_player_component(local_player, ios_class);
    uint64_t managed_transform_a = ios ? rpm<uint64_t>(ios + 0x50) : 0;
    uint64_t managed_transform_b = ios ? rpm<uint64_t>(ios + 0x58) : 0;
    uint64_t native_transform_a = managed_transform_a ? rpm<uint64_t>(managed_transform_a + v100::unity_cached_ptr) : 0;
    uint64_t native_transform_b = managed_transform_b ? rpm<uint64_t>(managed_transform_b + v100::unity_cached_ptr) : 0;
    uint64_t game_object_a = native_transform_a ? rpm<uint64_t>(native_transform_a + 0x20) : 0;
    uint64_t game_object_b = native_transform_b ? rpm<uint64_t>(native_transform_b + 0x20) : 0;
    probe_camera_pointer(game_object_a);
    if (game_object_b != game_object_a) probe_camera_pointer(game_object_b);
    // Some Unity native layouts keep the component array one pointer away from
    // the GameObject. The bounded graph walker handles both representations.
    if (native_transform_a) probe_camera_pointer(native_transform_a);
    if (native_transform_b && native_transform_b != native_transform_a) probe_camera_pointer(native_transform_b);
    static long long targeted_log = 0;
    if (diag::every(targeted_log, 5000))
        diag::log("CAMERA", "targeted TypeInfo camera=0x%llx ios=0x%llx controller=0x%llx transforms=(0x%llx,0x%llx) gameObjects=(0x%llx,0x%llx) candidates=%zu",
                  (unsigned long long)camera_class, (unsigned long long)ios_class,
                  (unsigned long long)ios, (unsigned long long)native_transform_a,
                  (unsigned long long)native_transform_b, (unsigned long long)game_object_a,
                  (unsigned long long)game_object_b, g_camera_candidates.size());
}

inline void remember_camera(uint64_t address, const matrix& view, const matrix& projection) {
    auto found = std::find_if(g_camera_candidates.begin(), g_camera_candidates.end(),
        [address](const camera_candidate& candidate) { return candidate.address == address; });
    if (found == g_camera_candidates.end()) {
        g_camera_candidates.push_back({address, view, projection});
        diag::log("CAMERA", "candidate found address=0x%llx total=%zu",
                  (unsigned long long)address, g_camera_candidates.size());
    } else {
        found->view = view;
        found->projection = projection;
    }
}

inline std::vector<camera_candidate> camera_candidates(uint64_t local_player) {
    ensure_scan_context();
    if (g_camera_maps.empty()) refresh_camera_maps();
    discover_camera_from_local_player(local_player);
    if (g_camera_candidates.empty()) discover_camera_get_main();
    g_camera_candidates.erase(std::remove_if(g_camera_candidates.begin(), g_camera_candidates.end(),
        [](camera_candidate& candidate) {
            return !validate_camera(candidate.address, candidate.view, candidate.projection);
        }), g_camera_candidates.end());
    return g_camera_candidates;
}
