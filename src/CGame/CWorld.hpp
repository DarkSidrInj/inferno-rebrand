#pragma once
// Grenades and dropped weapons, read from the same entity list the players come from.
// skeed classes: cim (grenade), qak (collision grenade), njg (dropped weapon).

#include "CGameContext.hpp"
#include "CPlayer.hpp"
#include "COffsets.hpp"
#include "CClass.hpp"
#include <cmath>
#include <vector>

namespace world {
    struct Entity { uint64_t object{}; klass::Kind kind{}; };
    struct Item { uint64_t object{}; Vector3 position{}; int weapon{}; bool grenade{}; };

    inline std::vector<Entity> g_entities;
    inline long long g_entities_stamp = 0;

    // GameMgr chain first, registry as fallback; the same sources players() uses.
    inline bool entity_array(uint64_t& array, int& count) {
        entitywalk::Snapshot snapshot{};
        if (entitywalk::resolve(snapshot)) {
            array = snapshot.entities_array;
            count = snapshot.entity_count;
            return plausible(array) && count > 0 && count <= 1024;
        }
        if (!g_registry) return false;
        const uint64_t store = rpm<uint64_t>(g_registry + v100::registry_store);
        const uint64_t list = rpm<uint64_t>(store + v100::store_objects);
        count = rpm<int>(list + v100::list_count);
        array = rpm<uint64_t>(list + v100::list_items);
        return plausible(array) && count > 0 && count <= 1024;
    }

    // Classified entities, refreshed at 10 Hz. Players and unknown classes are skipped.
    inline const std::vector<Entity>& entities() {
        const long long now = diag::now_ms();
        if (now - g_entities_stamp < 100) return g_entities;
        g_entities_stamp = now;
        g_entities.clear();

        uint64_t array = 0;
        int count = 0;
        if (!entity_array(array, count)) return g_entities;
        uint64_t items[1024]{};
        if (!mem_read(array + v100::array_data, items, size_t(count) * 8)) return g_entities;
        for (int i = 0; i < count; ++i) {
            if (!plausible(items[i])) continue;
            const klass::Kind kind = klass::kind_of(rpm<uint64_t>(items[i]));
            if (kind == klass::Kind::Other || kind == klass::Kind::Player) continue;
            g_entities.push_back({items[i], kind});
        }
        return g_entities;
    }

    inline bool usable(const Vector3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::fabs(v.x) < 10000.f && std::fabs(v.y) < 10000.f && std::fabs(v.z) < 10000.f &&
               (std::fabs(v.x) > .001f || std::fabs(v.y) > .001f || std::fabs(v.z) > .001f);
    }

    // Uses the Transform layout player::position already settled on. It never starts the
    // layout probe itself, so a wrong object can not overwrite the shared layout.
    inline bool decode_known(uint64_t native, Vector3& out) {
        const auto& layout = player::g_transform_layout;
        if (layout.pid != proc::pid || layout.data_offset < 0) return false;
        Vector3 value{};
        if (!player::decode_transform(native, layout.data_offset, layout.index_offset,
                                      layout.entries_offset, layout.parents_offset, value)) return false;
        if (!usable(value)) return false;
        out = value;
        return true;
    }

    inline bool managed_transform_position(uint64_t managed, Vector3& out) {
        if (!plausible(managed)) return false;
        const uint64_t native = rpm<uint64_t>(managed + v100::unity_cached_ptr);
        return plausible(native) && decode_known(native, out);
    }

    // The part object that carries the Transform keeps it in one of a few slots.
    inline bool holder_position(uint64_t holder, uint64_t& slot_cache, Vector3& out) {
        if (!plausible(holder)) return false;
        if (slot_cache && managed_transform_position(rpm<uint64_t>(holder + slot_cache), out)) return true;
        static const uint64_t slots[] = {0x18, 0x20, 0x10, 0x28, 0x30};
        for (uint64_t slot : slots) {
            if (!managed_transform_position(rpm<uint64_t>(holder + slot), out)) continue;
            slot_cache = slot;
            return true;
        }
        return false;
    }

    inline std::vector<Item> grenades() {
        std::vector<Item> out;
        static uint64_t part_slot = 0;
        for (const Entity& entity : entities()) {
            Item item{};
            item.object = entity.object;
            item.grenade = true;
            if (entity.kind == klass::Kind::Grenade) {
                const uint64_t part = rpm<uint64_t>(entity.object + v100::grenade_transform_part);
                if (!holder_position(part, part_slot, item.position)) continue;
                const uint64_t info = rpm<uint64_t>(entity.object + v100::grenade_info);
                item.weapon = plausible(info) ? int(rpm<uint8_t>(info + v100::grenade_info_weapon)) : 0;
            } else if (entity.kind == klass::Kind::GrenadeCollision) {
                const uint64_t holder = rpm<uint64_t>(entity.object + v100::grenade_col_holder);
                if (!plausible(holder) ||
                    !managed_transform_position(rpm<uint64_t>(holder + v100::grenade_col_transform), item.position)) continue;
                const uint64_t info = rpm<uint64_t>(entity.object + v100::grenade_col_info);
                item.weapon = plausible(info) ? int(rpm<uint8_t>(info + v100::grenade_col_weapon)) : 0;
            } else continue;
            out.push_back(item);
        }
        return out;
    }

    inline std::vector<Item> drops() {
        std::vector<Item> out;
        for (const Entity& entity : entities()) {
            if (entity.kind != klass::Kind::WeaponDrop) continue;
            const uint64_t visibility = rpm<uint64_t>(entity.object + v100::drop_visibility);
            if (!plausible(visibility)) continue;
            float center[3]{}, extents[3]{};
            if (!mem_read(visibility + v100::bounds_center, center, sizeof(center)) ||
                !mem_read(visibility + v100::bounds_extents, extents, sizeof(extents))) continue;
            Item item{};
            item.object = entity.object;
            item.position = Vector3(center[0], center[1], center[2]);
            if (!usable(item.position)) continue;
            const uint64_t data = rpm<uint64_t>(entity.object + v100::drop_data);
            item.weapon = plausible(data) ? int(rpm<uint8_t>(data + v100::drop_data_weapon)) : 0;
            out.push_back(item);
        }
        return out;
    }

    // Raw WeaponId values confirmed by skeed (grenade.cpp). Other ids print as numbers.
    inline const char* weapon_label(int id, char* scratch, size_t cap) {
        switch (id) {
            case 91: return "HE";
            case 92: return "Smoke";
            case 93: return "Flash";
            case 94: return "Molotov";
            case 95: return "Thermite";
            default: std::snprintf(scratch, cap, "Weapon %d", id); return scratch;
        }
    }
}
