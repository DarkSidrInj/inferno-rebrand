#pragma once
// The game's own camera, found the way skeed does it: the local player's part set holds
// the GameCamera controller (xmt : nrl). nrl.hycu is the managed UnityEngine.Camera, its
// m_CachedPtr is the native Camera, and the native Camera carries the live matrices.
// Those matrices already contain the shot animation, shake and the scoped FOV, so no
// angle, eye height or FOV is assumed anywhere.
//
// skeed reaches xmt through wmz+0x40. In the ARM64 dump wmz+0x40 is a
// Dictionary<Type, chx>, so the holder is searched structurally instead: any part
// object whose +0x20 is a managed Camera with a valid native Camera behind it.

#include "CGameContext.hpp"
#include "COffsets.hpp"
#include "CClass.hpp"
#include <vector>

namespace game_camera {
    struct Reading {
        uint64_t holder{};
        uint64_t native{};
        matrix view{};
        matrix projection{};
    };

    inline uint64_t g_holder = 0;
    inline long long g_last_search = 0;
    inline int g_pid = -1;

    inline bool read_holder(uint64_t holder, Reading& out) {
        if (!plausible(holder)) return false;
        const uint64_t managed = rpm<uint64_t>(holder + v100::gamecam_camera);
        if (!plausible(managed)) return false;
        const uint64_t native = rpm<uint64_t>(managed + v100::unity_cached_ptr);
        if (!plausible(native)) return false;
        if (!validate_camera(native, out.view, out.projection, false)) return false;
        out.holder = holder;
        out.native = native;
        return true;
    }

    inline void collect_array(uint64_t array, std::vector<uint64_t>& out) {
        if (!plausible(array)) return;
        const uint64_t length = rpm<uint64_t>(array + v100::array_length);
        if (length == 0 || length > 128) return;
        uint64_t items[128]{};
        if (!mem_read(array + v100::array_data, items, size_t(length) * 8)) return;
        for (uint64_t i = 0; i < length; ++i)
            if (plausible(items[i])) out.push_back(items[i]);
    }

    // Every object a net object keeps for its parts.
    inline void collect_parts(uint64_t object, std::vector<uint64_t>& out) {
        const uint64_t wmz = rpm<uint64_t>(object + v100::net_systems);
        if (plausible(wmz)) {
            for (uint64_t field = v100::wmz_first_array; field <= v100::wmz_last_array; field += 8)
                collect_array(rpm<uint64_t>(wmz + field), out);

            const uint64_t dict = rpm<uint64_t>(wmz + v100::wmz_dictionary);
            const uint64_t entries = plausible(dict) ? rpm<uint64_t>(dict + v100::dict_entries) : 0;
            const int count = plausible(dict) ? rpm<int>(dict + v100::dict_count) : 0;
            if (plausible(entries) && count > 0 && count <= 64) {
                for (int i = 0; i < count; ++i) {
                    const uint64_t value = rpm<uint64_t>(entries + v100::array_data +
                        uint64_t(i) * v100::dict_entry_stride + v100::dict_entry_value);
                    if (plausible(value)) out.push_back(value);
                }
            }
            // skeed reads the holder straight out of the container.
            for (uint64_t field = 0x40; field <= 0x80; field += 8) {
                const uint64_t value = rpm<uint64_t>(wmz + field);
                if (plausible(value)) out.push_back(value);
            }
        }
        const uint64_t rct = rpm<uint64_t>(object + v100::player_parts);
        const uint64_t list = plausible(rct) ? rpm<uint64_t>(rct + v100::parts_list) : 0;
        if (plausible(list)) collect_array(rpm<uint64_t>(list + v100::list_items), out);
    }

    // Keeps the holder between frames. Re-reads the matrices every frame; searches again
    // (at most every 500 ms) only when the cached holder stops answering.
    inline bool acquire(const std::vector<uint64_t>& players, uint64_t local_player, Reading& out) {
        if (g_pid != proc::pid) { g_pid = proc::pid; g_holder = 0; g_last_search = 0; }
        if (g_holder && read_holder(g_holder, out)) return true;
        g_holder = 0;

        const long long now = diag::now_ms();
        if (now - g_last_search < 500) return false;
        g_last_search = now;

        std::vector<uint64_t> order;
        if (local_player) order.push_back(local_player);
        for (uint64_t p : players) if (p != local_player) order.push_back(p);

        size_t tested = 0;
        for (uint64_t player : order) {
            std::vector<uint64_t> parts;
            parts.reserve(128);
            collect_parts(player, parts);
            for (uint64_t part : parts) {
                ++tested;
                if (!read_holder(part, out)) continue;
                g_holder = part;
                char name[16] = "?";
                if (klass::g_name_offset >= 0) klass::name_at(rpm<uint64_t>(part), klass::g_name_offset, name, sizeof(name));
                diag::log("CAMERA", "game camera found holder=0x%llx class=%s native=0x%llx player=0x%llx local=%d",
                          (unsigned long long)part, name, (unsigned long long)out.native,
                          (unsigned long long)player, player == local_player ? 1 : 0);
                return true;
            }
        }
        static long long miss_log = 0;
        if (diag::every(miss_log, 5000))
            diag::log("WARN", "game camera holder not found: %zu players, %zu part objects tested", order.size(), tested);
        return false;
    }

    inline float fov_degrees(const matrix& projection) {
        const float p5 = reinterpret_cast<const float*>(&projection)[5];
        return p5 > 0.001f ? 2.f * std::atan(1.f / p5) * 57.29578f : 0.f;
    }
}
