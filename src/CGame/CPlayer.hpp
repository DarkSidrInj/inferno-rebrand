#pragma once

#include "CGameContext.hpp"
#include "../CMath/CVector3.hpp"
#include <cmath>

namespace player {
    struct TransformEntry { Vector4 position; Vector4 rotation; Vector4 scale; };
    struct TransformLayout {
        int data_offset{-1};
        int index_offset{-1};
        int entries_offset{-1};
        int parents_offset{-1};
        int pid{-1};
    };
    inline TransformLayout g_transform_layout;
    inline long long g_last_layout_probe = 0;

    inline Vector3 rotate(const Vector4& q, const Vector3& v) noexcept {
        Vector3 u(q.x, q.y, q.z);
        Vector3 uv = Vector3::Cross(u, v);
        Vector3 uuv = Vector3::Cross(u, uv);
        return v + uv * (2.f * q.w) + uuv * 2.f;
    }

    inline bool entry_valid(const TransformEntry& entry) noexcept {
        const float values[] = {entry.position.x, entry.position.y, entry.position.z,
            entry.rotation.x, entry.rotation.y, entry.rotation.z, entry.rotation.w,
            entry.scale.x, entry.scale.y, entry.scale.z};
        for (float value : values)
            if (!std::isfinite(value) || std::fabs(value) > 100000.f) return false;
        const float qn = entry.rotation.x * entry.rotation.x + entry.rotation.y * entry.rotation.y +
                         entry.rotation.z * entry.rotation.z + entry.rotation.w * entry.rotation.w;
        return qn > .05f && qn < 4.f && std::fabs(entry.scale.x) > .00001f &&
               std::fabs(entry.scale.y) > .00001f && std::fabs(entry.scale.z) > .00001f;
    }

    inline bool decode_transform(uint64_t native, int data_offset, int index_offset,
                                 int entries_offset, int parents_offset, Vector3& out, int max_index = 8192) noexcept {
        uint64_t data = rpm<uint64_t>(native + uint64_t(data_offset));
        int index = rpm<int>(native + uint64_t(index_offset));
        if (!plausible(data) || index < 0 || index > max_index) return false;
        uint64_t entries = rpm<uint64_t>(data + uint64_t(entries_offset));
        uint64_t parents = rpm<uint64_t>(data + uint64_t(parents_offset));
        if (!plausible(entries) || !plausible(parents) || entries == parents) return false;
        TransformEntry current{};
        if (!mem_read(entries + uint64_t(index) * sizeof(TransformEntry), &current, sizeof(current)) ||
            !entry_valid(current)) return false;
        Vector3 result(current.position.x, current.position.y, current.position.z);
        int parent = rpm<int>(parents + uint64_t(index) * sizeof(int));
        int guard = 0;
        while (parent >= 0 && parent <= max_index && guard++ < 128) {
            TransformEntry entry{};
            if (!mem_read(entries + uint64_t(parent) * sizeof(TransformEntry), &entry, sizeof(entry)) ||
                !entry_valid(entry)) return false;
            Vector3 scaled(result.x * entry.scale.x, result.y * entry.scale.y, result.z * entry.scale.z);
            result = rotate(entry.rotation, scaled) + Vector3(entry.position.x, entry.position.y, entry.position.z);
            int next = rpm<int>(parents + uint64_t(parent) * sizeof(int));
            if (next == parent) return false;
            parent = next;
        }
        if (guard >= 128 || parent < -1 || !std::isfinite(result.x) || !std::isfinite(result.y) ||
            !std::isfinite(result.z) || std::fabs(result.x) > 100000.f ||
            std::fabs(result.y) > 100000.f || std::fabs(result.z) > 100000.f) return false;
        out = result;
        return true;
    }

    inline bool native_transform_position(uint64_t native, Vector3& out) noexcept {
        if (g_transform_layout.pid != proc::pid) {
            g_transform_layout = {};
            g_transform_layout.pid = proc::pid;
        }
        if (g_transform_layout.data_offset >= 0 &&
            decode_transform(native, g_transform_layout.data_offset, g_transform_layout.index_offset,
                             g_transform_layout.entries_offset, g_transform_layout.parents_offset, out)) return true;
        if (decode_transform(native, 0x38, 0x40, 0x18, 0x20, out)) {
            g_transform_layout = {0x38, 0x40, 0x18, 0x20, proc::pid};
            return true;
        }

        const long long now = diag::now_ms();
        if (now - g_last_layout_probe < 1000) return false;
        g_last_layout_probe = now;
        diag::log("PROBE", "probing Unity 6 Transform layout native=0x%llx", (unsigned long long)native);
        for (int data_offset = 0x10; data_offset <= 0xF8; data_offset += 8) {
            uint64_t data = rpm<uint64_t>(native + uint64_t(data_offset));
            if (!plausible(data)) continue;
            uint64_t data_words[16]{};
            if (!mem_read(data, data_words, sizeof(data_words))) continue;
            for (int index_offset = 0x10; index_offset <= 0xFC; index_offset += 4) {
                int index = rpm<int>(native + uint64_t(index_offset));
                if (index < 0 || index > 8192) continue;
                for (int entries_i = 0; entries_i < 16; ++entries_i) {
                    if (!plausible(data_words[entries_i])) continue;
                    for (int parents_i = 0; parents_i < 16; ++parents_i) {
                        if (parents_i == entries_i || !plausible(data_words[parents_i])) continue;
                        Vector3 candidate{};
                        if (!decode_transform(native, data_offset, index_offset,
                                              entries_i * 8, parents_i * 8, candidate)) continue;
                        if (std::fabs(candidate.x) <= .001f && std::fabs(candidate.y) <= .001f &&
                            std::fabs(candidate.z) <= .001f) continue;
                        g_transform_layout = {data_offset, index_offset, entries_i * 8,
                                              parents_i * 8, proc::pid};
                        out = candidate;
                        diag::log("INFO", "Unity Transform layout found data=+0x%x index=+0x%x entries=+0x%x parents=+0x%x pos=(%.3f %.3f %.3f)",
                                  data_offset, index_offset, entries_i * 8, parents_i * 8,
                                  out.x, out.y, out.z);
                        return true;
                    }
                }
            }
        }
        diag::log("WARN", "Unity Transform layout probe found no valid hierarchy");
        return false;
    }

    inline Vector4 quaternion_multiply(const Vector4& a, const Vector4& b) noexcept {
        return Vector4(
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    }

    inline bool normalize_quaternion(Vector4& q) noexcept {
        const float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (!std::isfinite(norm) || norm < .25f || norm > 2.f) return false;
        q.x /= norm; q.y /= norm; q.z /= norm; q.w /= norm;
        return true;
    }

    inline bool decode_transform_pose(uint64_t native, Vector3& position, Vector4& rotation, int index_offset = -1) noexcept {
        const auto& layout = g_transform_layout;
        if (layout.pid != proc::pid || layout.data_offset < 0) {
            if (!native_transform_position(native, position)) return false;
        }
        if (index_offset < 0) index_offset = g_transform_layout.index_offset;
        uint64_t data = rpm<uint64_t>(native + uint64_t(g_transform_layout.data_offset));
        int index = rpm<int>(native + uint64_t(index_offset));
        uint64_t entries = rpm<uint64_t>(data + uint64_t(g_transform_layout.entries_offset));
        uint64_t parents = rpm<uint64_t>(data + uint64_t(g_transform_layout.parents_offset));
        if (!plausible(data) || !plausible(entries) || !plausible(parents) || index < 0 || index > 8192) return false;
        TransformEntry current{};
        if (!mem_read(entries + uint64_t(index) * sizeof(TransformEntry), &current, sizeof(current)) ||
            !entry_valid(current)) return false;
        position = Vector3(current.position.x, current.position.y, current.position.z);
        rotation = current.rotation;
        int parent = rpm<int>(parents + uint64_t(index) * sizeof(int));
        int guard = 0;
        while (parent >= 0 && parent <= 8192 && guard++ < 128) {
            TransformEntry entry{};
            if (!mem_read(entries + uint64_t(parent) * sizeof(TransformEntry), &entry, sizeof(entry)) ||
                !entry_valid(entry)) return false;
            Vector3 scaled(position.x * entry.scale.x, position.y * entry.scale.y, position.z * entry.scale.z);
            position = rotate(entry.rotation, scaled) + Vector3(entry.position.x, entry.position.y, entry.position.z);
            rotation = quaternion_multiply(entry.rotation, rotation);
            const int next = rpm<int>(parents + uint64_t(parent) * sizeof(int));
            if (next == parent) return false;
            parent = next;
        }
        const float norm = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y +
                                     rotation.z * rotation.z + rotation.w * rotation.w);
        if (guard >= 128 || parent < -1 || !std::isfinite(norm) || norm < .01f) return false;
        rotation.x /= norm; rotation.y /= norm; rotation.z /= norm; rotation.w /= norm;
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    }

    inline matrix view_from_pose(const Vector3& position, const Vector4& rotation) noexcept {
        const Vector3 right = rotate(rotation, Vector3(1.f, 0.f, 0.f));
        const Vector3 up = rotate(rotation, Vector3(0.f, 1.f, 0.f));
        const Vector3 forward = rotate(rotation, Vector3(0.f, 0.f, 1.f));
        matrix view{};
        float* m = reinterpret_cast<float*>(&view);
        m[0] = right.x;    m[4] = right.y;    m[8] = right.z;
        m[1] = up.x;       m[5] = up.y;       m[9] = up.z;
        m[2] = -forward.x; m[6] = -forward.y; m[10] = -forward.z;
        m[12] = -(right.x * position.x + right.y * position.y + right.z * position.z);
        m[13] = -(up.x * position.x + up.y * position.y + up.z * position.z);
        m[14] = forward.x * position.x + forward.y * position.y + forward.z * position.z;
        m[15] = 1.f;
        return view;
    }

    // Vertical FOV of the synthetic camera. Only used when the game's own Camera was not
    // found; the native Camera carries the live (scoped) FOV in its projection matrix.
    inline float g_fallback_fov = 60.f;

    inline matrix perspective(float vertical_fov_degrees, float aspect) noexcept {
        matrix projection{};
        float* p = reinterpret_cast<float*>(&projection);
        const float near_plane = .03f, far_plane = 1000.f;
        const float f = 1.f / std::tan(vertical_fov_degrees * 0.00872664626f);
        p[0] = f / std::max(aspect, .1f);
        p[5] = f;
        p[10] = -(far_plane + near_plane) / (far_plane - near_plane);
        p[11] = -1.f;
        p[14] = -(2.f * far_plane * near_plane) / (far_plane - near_plane);
        return projection;
    }

    inline uint64_t find_part(uint64_t player, uint64_t part_class) noexcept {
        const uint64_t container = rpm<uint64_t>(player + v100::player_parts);
        const uint64_t list = rpm<uint64_t>(container + v100::parts_list);
        const int count = rpm<int>(list + v100::list_count);
        const uint64_t array = rpm<uint64_t>(list + v100::list_items);
        const uint64_t length = rpm<uint64_t>(array + v100::array_length);
        if (!part_class || !plausible(container) || !plausible(list) || !plausible(array) ||
            count <= 0 || count > 64 || length < uint64_t(count) || length > 256) return 0;
        for (int i = 0; i < count; ++i) {
            const uint64_t part = rpm<uint64_t>(array + v100::array_data + uint64_t(i) * 8);
            if (exact_class(part, part_class)) return part;
        }
        return 0;
    }

    inline std::vector<camera_candidate> camera_fallback_candidates(uint64_t local_player) {
        std::vector<camera_candidate> result;
        const uint64_t ios_class = rpm<uint64_t>(proc::lib + v100::ios_typeinfo);
        const uint64_t controller = find_player_component(local_player, ios_class);
        // dump.cs (1.0.0): ios.tryh +0x38 -> eir, with live quaternions at
        // +0x10/+0x20. Runtime axis inspection below identifies their roles.
        const uint64_t aim_state = controller ? rpm<uint64_t>(controller + 0x38) : 0;
        // Runtime values establish the obfuscated field semantics: +0x10 is
        // already the complete view rotation (yaw + pitch), while +0x20 is
        // its yaw-only companion. Do not multiply them or yaw is applied twice.
        Vector4 view_rotation = aim_state ? rpm<Vector4>(aim_state + 0x10) : Vector4{};
        Vector4 yaw = aim_state ? rpm<Vector4>(aim_state + 0x20) : Vector4{};
        const bool view_rotation_valid = normalize_quaternion(view_rotation);
        const bool yaw_valid = normalize_quaternion(yaw);
        Vector4 state_rotation{};
        bool state_rotation_valid = false;
        if (view_rotation_valid) {
            state_rotation = view_rotation;
            state_rotation_valid = true;
        }
        // The weapon hit transform is the game's authoritative live look ray.
        // Unlike the ios pivots, its world rotation contains both yaw and pitch.
        const uint64_t weapon_part_class = rpm<uint64_t>(proc::lib + v100::weapon_part_typeinfo);
        const uint64_t weapon_part = find_part(local_player, weapon_part_class);
        const uint64_t aim_managed = weapon_part ? rpm<uint64_t>(weapon_part + 0x40) : 0;
        const uint64_t aim_native = aim_managed ? rpm<uint64_t>(aim_managed + v100::unity_cached_ptr) : 0;
        Vector3 aim_position{};
        Vector4 aim_rotation{};
        const bool live_aim = aim_native && decode_transform_pose(aim_native, aim_position, aim_rotation);
        uint64_t selected_offset = 0;
        float forward_y = 0.f;
        // These transforms still provide the stable view origin. Their world
        // rotation is replaced by the live hit-transform rotation when valid.
        for (uint64_t offset : {uint64_t(0x58), uint64_t(0x50)}) {
            const uint64_t managed = rpm<uint64_t>(controller + offset);
            const uint64_t native = rpm<uint64_t>(managed + v100::unity_cached_ptr);
            Vector3 position{}; Vector4 rotation{};
            if (!native || !decode_transform_pose(native, position, rotation)) continue;
            // The ios pivot is at the character root. Lemming's real Camera
            // matrix originates at eye level, so mirror that vertical origin.
            position.y += 1.60f;
            if (state_rotation_valid) rotation = state_rotation;
            else if (live_aim) rotation = aim_rotation;
            forward_y = rotate(rotation, Vector3(0.f, 0.f, 1.f)).y;
            result.push_back({native, view_from_pose(position, rotation),
                              perspective(g_fallback_fov, g_sw / std::max(g_sh, 1.f)), true});
            selected_offset = offset;
            break;
        }
        static long long fallback_log = 0;
        if (diag::every(fallback_log, 5000))
            diag::log("CAMERA", "transform fallback controller=0x%llx generated=%zu origin=ios+0x%llx rotation=%s state=0x%llx viewQ=(%.3f %.3f %.3f %.3f) yawQ=(%.3f %.3f %.3f %.3f valid=%d) forwardY=%.3f fov=60 aspect=%.3f",
                      (unsigned long long)controller, result.size(),
                      (unsigned long long)selected_offset,
                      state_rotation_valid ? "eir:view(+0x10)" : (live_aim ? "nrk+0x40" : "ios"),
                      (unsigned long long)aim_state, view_rotation.x, view_rotation.y,
                      view_rotation.z, view_rotation.w, yaw.x, yaw.y, yaw.z, yaw.w,
                      yaw_valid ? 1 : 0,
                      forward_y, g_sw / std::max(g_sh, 1.f));
        return result;
    }

    inline void dump_memory(const char* label, uint64_t address, size_t bytes) {
        if (!address || bytes == 0 || bytes > 0x200) return;
        unsigned char buffer[0x200]{};
        if (!mem_read(address, buffer, bytes)) {
            diag::log("DEEP", "%s address=0x%llx bytes=0x%zx unreadable", label,
                      (unsigned long long)address, bytes);
            return;
        }
        diag::log("DEEP", "%s address=0x%llx bytes=0x%zx", label,
                  (unsigned long long)address, bytes);
        for (size_t offset = 0; offset + 16 <= bytes; offset += 16) {
            uint64_t q0 = 0, q1 = 0;
            float f[4]{};
            std::memcpy(&q0, buffer + offset, 8);
            std::memcpy(&q1, buffer + offset + 8, 8);
            std::memcpy(f, buffer + offset, 16);
            diag::log("DEEP", "%s+0x%03zx q=%016llx %016llx f=(%g %g %g %g)", label,
                      offset, (unsigned long long)q0, (unsigned long long)q1,
                      f[0], f[1], f[2], f[3]);
        }
    }

    inline void dump_code(const char* label, uint64_t address) {
        uint32_t words[16]{};
        if (!mem_read(address, words, sizeof(words))) return;
        diag::log("CODE", "%s address=0x%llx", label, (unsigned long long)address);
        for (int i = 0; i < 16; i += 4)
            diag::log("CODE", "+0x%02x %08x %08x %08x %08x", i * 4,
                      words[i], words[i + 1], words[i + 2], words[i + 3]);
    }

    inline void deep_transform_diagnostics(uint64_t player, const std::vector<uint64_t>& candidates) {
        diag::log("DEEP", "begin player=0x%llx libunity=0x%llx candidates=%zu",
                  (unsigned long long)player, (unsigned long long)proc::lib, candidates.size());
        dump_code("ake::xdzx", proc::lib + 0x9489018);
        dump_code("Transform::get_position", proc::lib + 0x67EF458);
        dump_code("Transform::get_position_Injected", proc::lib + 0x67EF4E8);
        dump_code("Transform::GetPositionAndRotation(native)", proc::lib + 0x53764E0);
        dump_memory("player", player, 0xB0);

        uint64_t float3_array = rpm<uint64_t>(player + 0x60);
        uint64_t float3_length = rpm<uint64_t>(float3_array + v100::array_length);
        diag::log("DEEP", "player+0x60 float3Array=0x%llx length=%llu interface68=0x%llx",
                  (unsigned long long)float3_array, (unsigned long long)float3_length,
                  (unsigned long long)rpm<uint64_t>(player + 0x68));
        if (float3_array && float3_length > 0 && float3_length <= 64)
            dump_memory("player.float3[]", float3_array + v100::array_data,
                        std::min<size_t>(size_t(float3_length) * 12, 0xC0));

        for (size_t i = 0; i < candidates.size(); ++i) {
            char managed_label[32], native_label[32], pointer_label[40];
            std::snprintf(managed_label, sizeof(managed_label), "managed[%zu]", i);
            std::snprintf(native_label, sizeof(native_label), "native[%zu]", i);
            dump_memory(managed_label, candidates[i], 0x60);
            uint64_t native = rpm<uint64_t>(candidates[i] + v100::unity_cached_ptr);
            dump_memory(native_label, native, 0x100);
            uint64_t words[32]{};
            if (!native || !mem_read(native, words, sizeof(words))) continue;
            int dumped = 0;
            for (size_t word = 0; word < 32 && dumped < 8; ++word) {
                uint64_t pointer = words[word];
                if (!plausible(pointer) || (pointer >= native && pointer < native + 0x200)) continue;
                bool duplicate = false;
                for (size_t previous = 0; previous < word; ++previous)
                    if (words[previous] == pointer) duplicate = true;
                if (duplicate) continue;
                std::snprintf(pointer_label, sizeof(pointer_label), "native[%zu].ptr+0x%zx", i, word * 8);
                dump_memory(pointer_label, pointer, 0x40);
                ++dumped;
            }
        }
        diag::log("DEEP", "end player=0x%llx", (unsigned long long)player);
    }

    inline std::vector<uint64_t> transform_candidates(uint64_t p) {
        std::vector<uint64_t> result;
        uint64_t transforms_class = rpm<uint64_t>(proc::lib + v100::transforms_typeinfo);
        uint64_t model_class = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
        uint64_t container = rpm<uint64_t>(p + v100::player_parts);
        uint64_t list = rpm<uint64_t>(container + v100::parts_list);
        int count = rpm<int>(list + v100::list_count);
        uint64_t array = rpm<uint64_t>(list + v100::list_items);
        uint64_t length = rpm<uint64_t>(array + v100::array_length);
        if (!transforms_class || !container || !list || !array || count <= 0 || count > 64 || length < uint64_t(count) || length > 256) return result;
        for (int i = 0; i < count; ++i) {
            uint64_t part = rpm<uint64_t>(array + v100::array_data + uint64_t(i) * 8);
            if (exact_class(part, transforms_class)) {
                uint64_t a = rpm<uint64_t>(part + v100::transform_a);
                uint64_t b = rpm<uint64_t>(part + v100::transform_b);
                if (a) result.push_back(a);
                if (b && b != a) result.push_back(b);
            } else if (model_class && exact_class(part, model_class)) {
                uint64_t transform = rpm<uint64_t>(part + v100::model_transform);
                if (transform) result.push_back(transform);
            }
        }
        return result;
    }

    inline Vector3 position(uint64_t p) noexcept {
        std::vector<uint64_t> candidates = transform_candidates(p);
        Vector3 zero_fallback{};
        bool decoded = false;
        for (uint64_t transform : candidates) {
            uint64_t native = rpm<uint64_t>(transform + v100::unity_cached_ptr);
            Vector3 result{};
            if (native && native_transform_position(native, result)) {
                decoded = true;
                zero_fallback = result;
                if (std::fabs(result.x) > .001f || std::fabs(result.y) > .001f || std::fabs(result.z) > .001f)
                    return result;
            }
        }
        static long long last = 0;
        if (rpm<uint8_t>(p + v100::player_local) != 0 && diag::every(last, 5000)) {
            diag::log("TRANSFORM", "player=0x%llx managedCandidates=%zu decoded=%d allZero=%d",
                      (unsigned long long)p, candidates.size(), decoded ? 1 : 0, decoded ? 1 : 0);
            for (size_t i = 0; i < candidates.size(); ++i) {
                uint64_t native = rpm<uint64_t>(candidates[i] + v100::unity_cached_ptr);
                diag::log("TRANSFORM", "candidate[%zu] managed=0x%llx native=0x%llx data38=0x%llx index40=%d",
                          i, (unsigned long long)candidates[i], (unsigned long long)native,
                          (unsigned long long)rpm<uint64_t>(native + 0x38), rpm<int>(native + 0x40));
            }
            deep_transform_diagnostics(p, candidates);
        }
        return decoded ? zero_fallback : Vector3{};
    }

    inline bool local(uint64_t p) noexcept { return rpm<uint8_t>(p + v100::player_local) != 0; }
    inline int team(uint64_t p) noexcept { return int(rpm<uint8_t>(p + v100::player_team)); }

    inline int health(uint64_t p) noexcept {
        uint64_t stats = rpm<uint64_t>(p + v100::player_stats);
        uint64_t stats_class = rpm<uint64_t>(proc::lib + v100::stats_typeinfo);
        if (!exact_class(stats, stats_class)) return 0;
        uint16_t hp = rpm<uint16_t>(stats + v100::stats_hp);
        uint16_t max_hp = rpm<uint16_t>(stats + v100::stats_max_hp);
        return max_hp > 0 && max_hp <= 10000 && hp <= max_hp ? int(hp) : 0;
    }

    // Armor, max armor and helmet from the same stats object as the health (offsets from the 1.0.0 dump, the
    // meanings are the project's own reading of it): valid only when the values make sense together.
    struct Armor { int armor = 0, max = 0; bool helmet = false, valid = false; };

    inline Armor armor(uint64_t p) noexcept {
        Armor out;
        uint64_t stats = rpm<uint64_t>(p + v100::player_stats);
        uint64_t stats_class = rpm<uint64_t>(proc::lib + v100::stats_typeinfo);
        if (!exact_class(stats, stats_class)) return out;
        const uint16_t armor = rpm<uint16_t>(stats + v100::stats_armor);
        const uint16_t max_armor = rpm<uint16_t>(stats + v100::stats_max_armor);
        const uint8_t helmet = rpm<uint8_t>(stats + v100::stats_helmet);
        if (max_armor > 1000 || armor > max_armor || helmet > 1) return out;
        out = {int(armor), int(max_armor), helmet == 1, true};
        return out;
    }
}
