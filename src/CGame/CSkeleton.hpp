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
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <tuple>
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
        std::vector<Node> nodes;  // every mapped bone (fingers, toes...); empty unless g_all_bones
        long long stamp{};
        bool live{};  // false = last good pose being held
    };

    // What to read for one rig: the bones, every ancestor up to the hierarchy root and, with
    // g_all_bones, every other mapped bone. Parents do not change, so a plan is made once
    // per rebuild and a frame is only a few reads of the entries.
    struct Run { int first{}; int count{}; int start{}; int span{}; };  // read entries [start, start+span)
    struct Plan {
        std::vector<int> index;   // ascending hierarchy indices that are read
        std::vector<int> parent;  // position in `index` of the parent, -1 = none
        std::vector<char> rig;    // 1 = a mapped bone drawn by "All bones"
        std::array<int, kCount> bone_pos{};  // position in `index`, -1 = bone not usable
        int root_pos{-1};
        std::vector<Run> runs;
        bool ok{};
    };

    struct Cache {
        std::array<uint64_t, kCount> index_of{};  // transform index per bone, 0 = unknown
        std::array<bool, kCount> known{};
        uint64_t data{};
        int min_index{};
        int max_index{};
        long long built{};
        long long touched{};
        long long plan_stamp{-1};  // `built` of the rebuild the plan belongs to
        Plan plan{};
        Pose pose{};
        std::string stage = "new";  // where the last attempt stopped, logged by draw_skeletons
        // Every bone of the BonesDictionary in this rig (HumanBodyBones key, hierarchy index):
        // fingers, toes, jaw and upper chest too. "All bones" draws exactly these, so props
        // parented under a bone (the weapon in the hand) are never drawn.
        std::vector<std::pair<int, int>> mapped;
    };

    inline std::unordered_map<uint64_t, Cache> g_cache;
    inline uint64_t g_dict_offset = 0;
    inline constexpr long long kRebuildMs = 1000;
    inline constexpr long long kHoldMs = 1000;
    inline constexpr int kMaxIndex = 262144;  // sanity bound for a transform index
    inline constexpr int kRunGap = 32;        // entries read between two indices rather than starting a new read
    inline constexpr float kRigRadius = 3.f;  // meters from the head; rejects transforms read past the end of the hierarchy
    inline bool g_all_bones = true;           // also draw the non-humanoid-21 mapped bones (fingers, toes, jaw)

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

    // Unity 6 transform index. The layout probe runs on a player root, whose index is 0, so a
    // field that merely happens to be 0 there passes it; on bones that field is garbage (the
    // device log showed index=631199440 and heads composed exactly at the feet). Measure the
    // index offset on the hips instead: the right one puts the hips about a meter above the
    // player's feet.
    inline int g_index_offset = -1;
    inline int g_index_pid = -1;

    inline int bone_index_offset(uint64_t hips_native, const Vector3& feet, const player::TransformLayout& l) {
        if (g_index_pid == proc::pid && g_index_offset >= 0) return g_index_offset;
        for (int off = 0x8; off <= 0x80; off += 4) {
            if (off >= l.data_offset && off < l.data_offset + 8) continue;
            const int index = rpm<int>(hips_native + uint64_t(off));
            if (index <= 0 || index >= kMaxIndex) continue;  // the hips are never the hierarchy root
            Vector3 at;
            if (!player::decode_transform(hips_native, l.data_offset, off, l.entries_offset, l.parents_offset, at, kMaxIndex)) continue;
            const float dx = at.x - feet.x, dy = at.y - feet.y, dz = at.z - feet.z;
            if (dx * dx + dz * dz > 1.5f * 1.5f || dy < 0.2f || dy > 2.0f) continue;
            g_index_offset = off;
            g_index_pid = proc::pid;
            diag::log("INFO", "bone transform index offset = +0x%x (layout probe said +0x%x; hips %.2f m above the feet)",
                      off, l.index_offset, dy);
            return off;
        }
        return -1;
    }

    inline bool build(uint64_t p, const Vector3& feet, Cache& cache, long long now) {
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
        cache.min_index = 0;
        cache.max_index = 0;
        std::array<uint64_t, kCount> data_of{};
        std::vector<std::tuple<int, int, uint64_t>> all;  // key, index, hierarchy
        int bad_native = 0, bad_data = 0, bad_index = 0;
        char trace[200] = "";
        int index_offset = -1;
        for (const BoneRef& ref : refs) {
            if (ref.key != Hips) continue;
            const uint64_t native = rpm<uint64_t>(ref.managed + v100::unity_cached_ptr);
            if (plausible(native)) index_offset = bone_index_offset(native, feet, *l);
        }
        const bool measured = index_offset >= 0;
        if (!measured) index_offset = l->index_offset;
        for (const BoneRef& ref : refs) {
            const uint64_t native = rpm<uint64_t>(ref.managed + v100::unity_cached_ptr);
            const uint64_t data = plausible(native) ? rpm<uint64_t>(native + uint64_t(l->data_offset)) : 0;
            const int index = plausible(native) ? rpm<int>(native + uint64_t(index_offset)) : -1;
            if (ref.key >= kCount) {
                if (plausible(data) && index >= 0 && index < kMaxIndex) all.emplace_back(ref.key, index, data);
                continue;
            }
            if (ref.key == Hips || (ref.key == Head && !trace[0]))
                std::snprintf(trace, sizeof(trace), "bone %d managed=0x%llx native=0x%llx data=0x%llx index=%d",
                              ref.key, (unsigned long long)ref.managed, (unsigned long long)native,
                              (unsigned long long)data, index);
            if (!plausible(native)) { ++bad_native; continue; }
            if (!plausible(data)) { ++bad_data; continue; }
            if (index < 0 || index >= kMaxIndex) { ++bad_index; continue; }
            data_of[ref.key] = data;
            cache.index_of[ref.key] = uint64_t(index);
            cache.known[ref.key] = true;
            all.emplace_back(ref.key, index, data);
        }
        // The rig is one hierarchy; a bone that points elsewhere is not part of it.
        cache.data = data_of[Hips] ? data_of[Hips] : data_of[Head];
        if (!cache.data) {
            char text[256];
            std::snprintf(text, sizeof(text), "bone transforms unreadable (native=%d data=%d index=%d bad of %zu, index +0x%x %s) %s",
                          bad_native, bad_data, bad_index, refs.size(), index_offset, measured ? "measured" : "NOT measured", trace);
            cache.stage = text;
            return false;
        }
        int usable = 0;
        cache.min_index = kMaxIndex;
        for (int i = 0; i < kCount; ++i) {
            if (cache.known[i] && data_of[i] != cache.data) cache.known[i] = false;
            if (!cache.known[i]) continue;
            ++usable;
            cache.min_index = std::min(cache.min_index, int(cache.index_of[i]));
            cache.max_index = std::max(cache.max_index, int(cache.index_of[i]));
        }
        if (usable == 0) cache.min_index = 0;
        cache.mapped.clear();
        for (const auto& bone : all)
            if (std::get<2>(bone) == cache.data) cache.mapped.emplace_back(std::get<0>(bone), std::get<1>(bone));
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

    // Reads the parents array only where the bones are: each bone's chain up to the hierarchy
    // root. Only bones from the BonesDictionary are planned, so nothing parented under them
    // (weapons, attachments) is ever read or drawn.
    inline bool make_plan(const Cache& cache, Plan& plan) {
        plan = Plan{};
        plan.bone_pos.fill(-1);
        const auto* l = layout();
        if (!l || !cache.data) return false;
        const uint64_t parents = rpm<uint64_t>(cache.data + uint64_t(l->parents_offset));
        if (!plausible(parents)) return false;

        std::unordered_map<int, int> parent_of;  // hierarchy index -> parent index, -1 = root, -2 = unreadable
        auto parent_at = [&](int i) {
            const auto found = parent_of.find(i);
            if (found != parent_of.end()) return found->second;
            int p = -2;
            if (i >= 0 && i < kMaxIndex) p = rpm<int>(parents + uint64_t(i) * 4);
            if (p < -1 || p >= kMaxIndex || p == i) p = -2;
            parent_of[i] = p;
            return p;
        };

        std::vector<int> wanted;
        std::array<bool, kCount> chain_ok{};
        // A bone is usable when its parent chain reaches the hierarchy root.
        auto chain = [&](int i) {
            for (int guard = 0; guard < 128; ++guard) {
                wanted.push_back(i);
                const int p = parent_at(i);
                if (p == -1) return true;
                if (p < 0) return false;
                i = p;
            }
            return false;
        };
        for (int bone = 0; bone < kCount; ++bone)
            if (cache.known[bone]) chain_ok[bone] = chain(int(cache.index_of[bone]));

        // The rig drawn by "All bones": every mapped bone with a full chain (the 21 humanoid
        // bones when the cache carries no mapping, as in hand-built caches).
        std::vector<int> rig_bones;
        if (g_all_bones) {
            if (!cache.mapped.empty()) {
                for (const auto& bone : cache.mapped) if (chain(bone.second)) rig_bones.push_back(bone.second);
            } else {
                for (int bone = 0; bone < kCount; ++bone)
                    if (cache.known[bone] && chain_ok[bone]) rig_bones.push_back(int(cache.index_of[bone]));
            }
        }
        int root = -1;
        for (int bone : {Hips, Spine, Chest, Neck, Head})
            if (cache.known[bone] && chain_ok[bone]) { root = int(cache.index_of[bone]); break; }

        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        if (wanted.empty()) return false;
        plan.index = wanted;
        std::unordered_map<int, int> pos_of;
        for (size_t k = 0; k < wanted.size(); ++k) pos_of[wanted[k]] = int(k);

        plan.parent.assign(wanted.size(), -1);
        plan.rig.assign(wanted.size(), 0);
        for (size_t k = 0; k < wanted.size(); ++k) {
            const int p = parent_at(wanted[k]);
            const auto found = p >= 0 ? pos_of.find(p) : pos_of.end();
            plan.parent[k] = found != pos_of.end() ? found->second : -1;
        }
        for (int i : rig_bones) plan.rig[size_t(pos_of[i])] = 1;
        if (root >= 0) plan.root_pos = pos_of[root];
        int placed = 0;
        for (int bone = 0; bone < kCount; ++bone) {
            if (!cache.known[bone] || !chain_ok[bone]) continue;
            plan.bone_pos[size_t(bone)] = pos_of[int(cache.index_of[bone])];
            ++placed;
        }

        for (size_t k = 0; k < wanted.size();) {
            size_t last = k;
            while (last + 1 < wanted.size() && wanted[last + 1] - wanted[last] <= kRunGap) ++last;
            plan.runs.push_back({int(k), int(last - k + 1), wanted[k], wanted[last] - wanted[k] + 1});
            k = last + 1;
        }
        plan.ok = placed >= 8;
        return plan.ok;
    }

    // Composes every cached bone, and with g_all_bones the whole rig, from a few reads of the
    // hierarchy entries.
    inline bool update(Cache& cache, Pose& out) {
        const auto* l = layout();
        if (!l || !cache.data) return false;
        if (cache.plan_stamp != cache.built) {  // one attempt per rebuild
            cache.plan_stamp = cache.built;
            make_plan(cache, cache.plan);
        }
        const Plan& plan = cache.plan;
        if (!plan.ok) return false;
        const uint64_t entries = rpm<uint64_t>(cache.data + uint64_t(l->entries_offset));
        if (!plausible(entries)) return false;

        const size_t count = plan.index.size();
        std::vector<player::TransformEntry> table(count), buffer;
        for (const Run& run : plan.runs) {
            buffer.assign(size_t(run.span), player::TransformEntry{});
            if (!mem_read(entries + uint64_t(run.start) * sizeof(player::TransformEntry), buffer.data(),
                          size_t(run.span) * sizeof(player::TransformEntry))) return false;
            for (int k = 0; k < run.count; ++k)
                table[size_t(run.first + k)] = buffer[size_t(plan.index[size_t(run.first + k)] - run.start)];
        }

        std::vector<Vector3> wpos(count);
        std::vector<Vector4> wrot(count), wscale(count);
        std::vector<uint8_t> state(count, 0);  // 0 = todo, 1 = composed, 2 = unusable
        std::vector<int> chain;
        auto resolve = [&](int i) {
            chain.clear();
            int cur = i, guard = 0;
            while (cur >= 0 && state[size_t(cur)] == 0 && guard++ < 128) {
                chain.push_back(cur);
                cur = plan.parent[size_t(cur)];
            }
            bool ok = guard < 128 && (cur < 0 || state[size_t(cur)] == 1);
            for (size_t k = chain.size(); k-- > 0;) {
                const int pos = chain[k];
                const auto& e = table[size_t(pos)];
                const int p = plan.parent[size_t(pos)];
                if (!ok || !player::entry_valid(e) || (p >= 0 && state[size_t(p)] != 1)) {
                    state[size_t(pos)] = 2;
                    ok = false;
                    continue;
                }
                if (p < 0) {
                    wpos[size_t(pos)] = Vector3(e.position.x, e.position.y, e.position.z);
                    wrot[size_t(pos)] = e.rotation;
                    wscale[size_t(pos)] = e.scale;
                } else {
                    const Vector4& ps = wscale[size_t(p)];
                    const Vector3 scaled(e.position.x * ps.x, e.position.y * ps.y, e.position.z * ps.z);
                    wpos[size_t(pos)] = wpos[size_t(p)] + player::rotate(wrot[size_t(p)], scaled);
                    wrot[size_t(pos)] = qmul(wrot[size_t(p)], e.rotation);
                    wscale[size_t(pos)] = Vector4(ps.x * e.scale.x, ps.y * e.scale.y, ps.z * e.scale.z, 0.f);
                }
                state[size_t(pos)] = 1;
            }
            return state[size_t(i)] == 1;
        };
        auto sane = [](const Vector3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                   std::fabs(v.x) <= 10000.f && std::fabs(v.y) <= 10000.f && std::fabs(v.z) <= 10000.f;
        };

        int composed = 0;
        for (int bone = 0; bone < kCount; ++bone) {
            out.valid[size_t(bone)] = false;
            const int pos = plan.bone_pos[size_t(bone)];
            if (pos < 0 || !resolve(pos) || !sane(wpos[size_t(pos)])) continue;
            out.position[size_t(bone)] = wpos[size_t(pos)];
            out.valid[size_t(bone)] = true;
            ++composed;
        }
        const bool ok = composed >= 8 && out.valid[Head];

        out.nodes.clear();
        if (g_all_bones && ok) {
            const Vector3& head = out.position[Head];
            std::vector<int> node_of(count, -1);
            for (size_t pos = 0; pos < count; ++pos) {
                if (!plan.rig[pos] || !resolve(int(pos)) || !sane(wpos[pos])) continue;
                const float dx = wpos[pos].x - head.x, dy = wpos[pos].y - head.y, dz = wpos[pos].z - head.z;
                if (dx * dx + dy * dy + dz * dz > kRigRadius * kRigRadius) continue;
                node_of[pos] = int(out.nodes.size());
                out.nodes.push_back({wpos[pos], plan.index[pos], -1});
            }
            // Link each drawn bone to its nearest drawn ancestor (fingers to the hand, the hand
            // to the lower arm), skipping unmapped helper transforms in between.
            for (size_t pos = 0; pos < count; ++pos) {
                if (node_of[pos] < 0) continue;
                int p = plan.parent[pos];
                for (int guard = 0; p >= 0 && node_of[size_t(p)] < 0 && guard < 64; ++guard) p = plan.parent[size_t(p)];
                if (p >= 0) out.nodes[size_t(node_of[pos])].parent = node_of[size_t(p)];
            }
        }
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
            if (!build(p, feet, cache, now)) cache.built = now;  // retry in a second, keep any held pose
        }
        Pose fresh = cache.pose;
        const bool composed = cache.data != 0 && update(cache, fresh);
        const bool got = composed && plausible_pose(fresh, feet);
        if (cache.data != 0) {
            if (got) cache.stage = "live";
            else if (!composed) cache.stage = cache.plan.ok ? "hierarchy unreadable" : "read plan failed (parents unreadable or <8 bones chained)";
            else {
                char text[200];
                std::snprintf(text, sizeof(text), "pose rejected: head=(%.2f %.2f %.2f) feet=(%.2f %.2f %.2f)",
                              fresh.position[Head].x, fresh.position[Head].y, fresh.position[Head].z, feet.x, feet.y, feet.z);
                cache.stage = text;
            }
        }
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
