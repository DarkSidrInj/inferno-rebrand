#pragma once
// Player skeleton, skeed style: model part (jgi) -> CharacterBindings -> BonesDictionary
// (HumanBodyBones -> Transform). All bones of one character live in one Unity
// TransformHierarchy, so the entries and parents arrays are read once per frame and
// every bone is composed locally. The managed -> native -> index lookups are cached and
// rebuilt once a second, which keeps a frame at two memory reads per player.

#include "CGameContext.hpp"
#include "CPlayer.hpp"
#include "CCamera.hpp"
#include "COffsets.hpp"
#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace skeleton {
    // UnityEngine.HumanBodyBones
    enum Bone : int {
        Hips = 0, LUpperLeg, RUpperLeg, LLowerLeg, RLowerLeg, LFoot, RFoot, Spine, Chest, Neck, Head,
        LShoulder, RShoulder, LUpperArm, RUpperArm, LLowerArm, RLowerArm, LHand, RHand, LToes, RToes,
        kCount
    };

    // One transform of the animated rig, for drawing every bone and not just the humanoid ones.
    struct Node { Vector3 position; int index{}; int parent{-1}; };  // parent = index into Pose::nodes, -1 = none

    struct Pose {
        std::array<Vector3, kCount> position{};
        std::array<bool, kCount> valid{};
        std::vector<Node> nodes;  // every transform below the hips; empty unless g_all_bones
        long long stamp{};
        bool live{};  // false = last good pose being held
    };

    struct Cache {
        std::array<uint64_t, kCount> index_of{};  // transform index per bone, 0 = unknown
        std::array<bool, kCount> known{};
        uint64_t data{};
        int max_index{};
        long long built{};
        long long touched{};
        Pose pose{};
        const char* stage = "new";  // where the last attempt stopped, logged by draw_skeletons
    };

    inline std::unordered_map<uint64_t, Cache> g_cache;
    inline uint64_t g_dict_offset = 0;
    inline constexpr long long kRebuildMs = 1000;
    inline constexpr long long kHoldMs = 1000;
    inline constexpr float kRigRadius = 3.f;  // meters from the head; rejects transforms read past the end of the hierarchy
    inline bool g_all_bones = true;           // compose and draw the whole rig

    struct BoneRef { int key{}; uint64_t managed{}; };

    // jgi is either a parts entry itself or the field vds.jyfd (+0x58) of a chx part, and
    // chx parts live in the wmz arrays as well as the rct list. Search all of them.
    inline uint64_t find_model(uint64_t p) {
        const uint64_t model_class = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
        if (!plausible(model_class)) return 0;
        std::vector<uint64_t> parts;
        parts.reserve(128);
        game_camera::collect_parts(p, parts);
        for (uint64_t part : parts) {
            if (exact_class(part, model_class)) return part;
            const uint64_t model = rpm<uint64_t>(part + v100::part_model);
            if (plausible(model) && exact_class(model, model_class)) return model;
        }
        return 0;
    }

    inline bool dictionary_refs(uint64_t dict, std::vector<BoneRef>& out) {
        out.clear();
        const uint64_t entries = rpm<uint64_t>(dict + v100::dict_entries);
        const int count = rpm<int>(dict + v100::dict_count);
        if (!plausible(entries) || count <= 0 || count > 64) return false;
        std::vector<uint8_t> raw(size_t(count) * v100::dict_entry_stride);
        if (!mem_read(entries + v100::array_data, raw.data(), raw.size())) return false;
        for (int i = 0; i < count; ++i) {
            const uint8_t* entry = raw.data() + size_t(i) * v100::dict_entry_stride;
            int hash = 0, key = 0;
            uint64_t value = 0;
            std::memcpy(&hash, entry, 4);
            std::memcpy(&key, entry + v100::dict_entry_key, 4);
            std::memcpy(&value, entry + v100::dict_entry_value, 8);
            if (hash < 0 || key < 0 || key >= 64 || !plausible(value)) continue;
            out.push_back({key, value});
        }
        return out.size() >= 8;  // a humanoid has far more than eight mapped bones
    }

    inline bool bone_refs(uint64_t bones, std::vector<BoneRef>& out) {
        if (g_dict_offset) {
            const uint64_t dict = rpm<uint64_t>(bones + g_dict_offset);
            if (plausible(dict) && dictionary_refs(dict, out)) return true;
        }
        for (uint64_t offset = v100::bones_dict_probe_first; offset <= v100::bones_dict_probe_last; offset += 8) {
            const uint64_t dict = rpm<uint64_t>(bones + offset);
            if (!plausible(dict) || !dictionary_refs(dict, out)) continue;
            g_dict_offset = offset;
            diag::log("INFO", "BonesDictionary inner Dictionary found at +0x%llx (%zu bones)",
                      (unsigned long long)offset, out.size());
            return true;
        }
        return false;
    }

    inline const player::TransformLayout* layout() {
        const auto& value = player::g_transform_layout;
        return value.pid == proc::pid && value.data_offset >= 0 ? &value : nullptr;
    }

    inline bool build(uint64_t p, Cache& cache, long long now) {
        const auto* l = layout();
        if (!l) { cache.stage = "transform layout not measured"; return false; }
        const uint64_t model = find_model(p);
        if (!plausible(model)) { cache.stage = "jgi not found in parts"; return false; }
        const uint64_t bindings = rpm<uint64_t>(model + v100::model_bindings);
        if (!plausible(bindings)) { cache.stage = "jgi has no CharacterBindings"; return false; }
        const uint64_t bones = rpm<uint64_t>(bindings + v100::bindings_bones);
        if (!plausible(bones)) { cache.stage = "bindings has no BonesDictionary"; return false; }
        std::vector<BoneRef> refs;
        if (!bone_refs(bones, refs)) { cache.stage = "BonesDictionary unreadable"; return false; }

        cache.known.fill(false);
        cache.index_of.fill(0);
        cache.data = 0;
        cache.max_index = 0;
        std::array<uint64_t, kCount> data_of{};
        for (const BoneRef& ref : refs) {
            if (ref.key >= kCount) continue;
            const uint64_t native = rpm<uint64_t>(ref.managed + v100::unity_cached_ptr);
            if (!plausible(native)) continue;
            const uint64_t data = rpm<uint64_t>(native + uint64_t(l->data_offset));
            const int index = rpm<int>(native + uint64_t(l->index_offset));
            if (!plausible(data) || index < 0 || index > 4096) continue;
            data_of[ref.key] = data;
            cache.index_of[ref.key] = uint64_t(index);
            cache.known[ref.key] = true;
        }
        // The rig is one hierarchy; a bone that points elsewhere is not part of it.
        cache.data = data_of[Hips] ? data_of[Hips] : data_of[Head];
        if (!cache.data) { cache.stage = "bone transforms unreadable"; return false; }
        int usable = 0;
        for (int i = 0; i < kCount; ++i) {
            if (cache.known[i] && data_of[i] != cache.data) cache.known[i] = false;
            if (!cache.known[i]) continue;
            ++usable;
            cache.max_index = std::max(cache.max_index, int(cache.index_of[i]));
        }
        cache.built = now;
        cache.stage = usable >= 8 ? "rig found" : "fewer than 8 bones usable";
        return usable >= 8;
    }

    inline Vector4 qmul(const Vector4& a, const Vector4& b) {
        return Vector4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    }

    // Every transform below the hips (or the highest humanoid bone that is mapped): the whole
    // animated rig, with each node linked to its parent. World transforms are composed once per
    // node from its parent's, so the cost is one pass over the hierarchy.
    inline void compose_rig(const Cache& cache, const std::vector<player::TransformEntry>& table,
                            const std::vector<int>& parent_of, int n, Pose& out) {
        out.nodes.clear();
        int root = -1;
        for (int bone : {Hips, Spine, Chest, Neck, Head})
            if (cache.known[bone]) { root = int(cache.index_of[bone]); break; }
        if (root < 0 || root >= n) return;

        const size_t count = size_t(n);
        std::vector<Vector3> wpos(count);
        std::vector<Vector4> wrot(count), wscale(count);
        std::vector<uint8_t> state(count, 0);  // 0 = todo, 1 = composed, 2 = unusable
        std::vector<int> chain;
        auto resolve = [&](int i) {
            chain.clear();
            int cur = i, guard = 0;
            while (cur >= 0 && cur < n && state[size_t(cur)] == 0 && guard++ < 128) {
                chain.push_back(cur);
                cur = parent_of[size_t(cur)];
            }
            bool ok = guard < 128 && (cur < 0 || (cur < n && state[size_t(cur)] == 1));
            for (size_t k = chain.size(); k-- > 0;) {
                const int idx = chain[k];
                const auto& e = table[size_t(idx)];
                const int p = parent_of[size_t(idx)];
                if (!ok || !player::entry_valid(e) || (p >= 0 && (p >= n || state[size_t(p)] != 1))) {
                    state[size_t(idx)] = 2;
                    ok = false;
                    continue;
                }
                if (p < 0) {
                    wpos[size_t(idx)] = Vector3(e.position.x, e.position.y, e.position.z);
                    wrot[size_t(idx)] = e.rotation;
                    wscale[size_t(idx)] = e.scale;
                } else {
                    const Vector4& ps = wscale[size_t(p)];
                    const Vector3 scaled(e.position.x * ps.x, e.position.y * ps.y, e.position.z * ps.z);
                    wpos[size_t(idx)] = wpos[size_t(p)] + player::rotate(wrot[size_t(p)], scaled);
                    wrot[size_t(idx)] = qmul(wrot[size_t(p)], e.rotation);
                    wscale[size_t(idx)] = Vector4(ps.x * e.scale.x, ps.y * e.scale.y, ps.z * e.scale.z, 0.f);
                }
                state[size_t(idx)] = 1;
            }
            return state[size_t(i)] == 1;
        };
        auto below_root = [&](int i) {
            for (int cur = i, guard = 0; cur >= 0 && cur < n && guard++ < 64; cur = parent_of[size_t(cur)])
                if (cur == root) return true;
            return false;
        };

        const Vector3& head = out.position[Head];
        std::vector<int> node_of(count, -1);
        for (int i = 0; i < n; ++i) {
            if (!below_root(i) || !resolve(i)) continue;
            const Vector3& at = wpos[size_t(i)];
            if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z)) continue;
            const float dx = at.x - head.x, dy = at.y - head.y, dz = at.z - head.z;
            if (dx * dx + dy * dy + dz * dz > kRigRadius * kRigRadius) continue;
            node_of[size_t(i)] = int(out.nodes.size());
            out.nodes.push_back({at, i, -1});
        }
        for (Node& node : out.nodes) {
            const int p = parent_of[size_t(node.index)];
            node.parent = (p >= 0 && p < n && node.index != root) ? node_of[size_t(p)] : -1;
        }
    }

    // Composes every cached bone from one read of the hierarchy arrays.
    inline bool update(Cache& cache, Pose& out) {
        const auto* l = layout();
        if (!l || !cache.data) return false;
        const uint64_t entries = rpm<uint64_t>(cache.data + uint64_t(l->entries_offset));
        const uint64_t parents = rpm<uint64_t>(cache.data + uint64_t(l->parents_offset));
        if (!plausible(entries) || !plausible(parents) || entries == parents) return false;

        // Parents are not always stored before their children, so read well past the highest
        // bone index and fall back to smaller reads if the arrays end sooner.
        const int sizes[] = {std::min(cache.max_index + 1 + 256, 4096), std::min(cache.max_index + 1 + 48, 4096),
                             cache.max_index + 1};
        int n = 0;
        std::vector<player::TransformEntry> table;
        std::vector<int> parent_of;
        for (int size : sizes) {
            table.assign(size_t(size), player::TransformEntry{});
            parent_of.assign(size_t(size), -1);
            if (mem_read(entries, table.data(), size_t(size) * sizeof(player::TransformEntry)) &&
                mem_read(parents, parent_of.data(), size_t(size) * sizeof(int))) { n = size; break; }
        }
        if (!n) return false;

        int composed = 0;
        for (int bone = 0; bone < kCount; ++bone) {
            out.valid[bone] = false;
            if (!cache.known[bone]) continue;
            const int index = int(cache.index_of[bone]);
            if (index >= n || !player::entry_valid(table[size_t(index)])) continue;
            const auto& self = table[size_t(index)];
            Vector3 result(self.position.x, self.position.y, self.position.z);
            int parent = parent_of[size_t(index)];
            int guard = 0;
            bool ok = true;
            while (parent >= 0 && guard++ < 128) {
                if (parent >= n || !player::entry_valid(table[size_t(parent)])) { ok = false; break; }
                const auto& e = table[size_t(parent)];
                const Vector3 scaled(result.x * e.scale.x, result.y * e.scale.y, result.z * e.scale.z);
                result = player::rotate(e.rotation, scaled) + Vector3(e.position.x, e.position.y, e.position.z);
                const int next = parent_of[size_t(parent)];
                if (next == parent) { ok = false; break; }
                parent = next;
            }
            if (!ok || guard >= 128 || !std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z) ||
                std::fabs(result.x) > 10000.f || std::fabs(result.y) > 10000.f || std::fabs(result.z) > 10000.f) continue;
            out.position[bone] = result;
            out.valid[bone] = true;
            ++composed;
        }
        const bool ok = composed >= 8 && out.valid[Head];
        if (g_all_bones && ok) compose_rig(cache, table, parent_of, n, out);
        else out.nodes.clear();
        return ok;
    }

    // A culled animator stops updating the bones of players you cannot see. Accept a pose
    // only when it still stands on the player: head above the feet, rig close to them.
    inline bool plausible_pose(const Pose& pose, const Vector3& feet) {
        if (!pose.valid[Head]) return false;
        const Vector3& head = pose.position[Head];
        const float rise = head.y - feet.y;
        const float dx = head.x - feet.x, dz = head.z - feet.z;
        return rise > 0.3f && rise < 3.0f && dx * dx + dz * dz < 4.f;
    }

    // Pose for one player. Returns false when there is nothing worth drawing.
    inline bool get(uint64_t p, const Vector3& feet, Pose& out) {
        const long long now = diag::now_ms();
        Cache& cache = g_cache[p];
        cache.touched = now;

        if (cache.data == 0 || now - cache.built > kRebuildMs) {
            if (!build(p, cache, now)) cache.built = now;  // retry in a second, keep any held pose
        }
        Pose fresh = cache.pose;
        const bool composed = cache.data != 0 && update(cache, fresh);
        const bool got = composed && plausible_pose(fresh, feet);
        if (cache.data != 0) cache.stage = got ? "live" : composed ? "pose rejected (head/feet)" : "hierarchy unreadable";
        if (got) {
            fresh.stamp = now;
            fresh.live = true;
            cache.pose = fresh;
        } else if (cache.pose.stamp != 0 && now - cache.pose.stamp <= kHoldMs) {
            cache.pose.live = false;
        } else {
            cache.pose = Pose{};
            return false;
        }
        out = cache.pose;
        return true;
    }

    inline void prune(long long now) {
        for (auto it = g_cache.begin(); it != g_cache.end();)
            it = now - it->second.touched > 5000 ? g_cache.erase(it) : std::next(it);
    }
}
