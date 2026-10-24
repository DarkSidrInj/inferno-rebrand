#include "imgui.h"
#include "CGame/CGameContext.hpp"
#include "CMath/CGameMath.hpp"
#include "CGame/CPlayer.hpp"
#include "CMenu/Theme/CTheme.hpp"
#include "CGame/CCamera.hpp"
#include "CGame/CWorld.hpp"
#include "CGame/CSkeleton.hpp"
#include "CGame/CFog.hpp"
#include "CGame/CAnimCull.hpp"
#include "CGame/CChams.hpp"
#include "CGame/CThirdPerson.hpp"
#include "CGame/CAntiAim.hpp"
#include "CGame/CLocalView.hpp"
#include "CGame/CSceneNode.hpp"
#include "CGame/CSkins.hpp"
#include "CGame/CStrafe.hpp"
#include "CGame/CBhop.hpp"
#include "fixtures/rs_accessors.h"
#include "CConfigs/CConfig.hpp"
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile uint64_t g_remote_flag = 0;
extern "C" __attribute__((noinline)) uint64_t remote_target(volatile uint64_t* p, uint64_t v) { *p = v; return v + 1; }

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The touch screen of the host build: a finger list the tests set.
namespace touch {
    static int g_fake_count = 0;
    static Finger g_fake[4];
    int fingers(Finger* out, int max) {
        const int n = g_fake_count < max ? g_fake_count : max;
        for (int i = 0; i < n; ++i) out[i] = g_fake[i];
        return n;
    }
}

int main() {
    proc::pid = getpid();
    // Fake libunity big enough for the largest RVA we dereference.
    const size_t lib_size = 0xC500000;
    void* lib = mmap(nullptr, lib_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(lib != MAP_FAILED, "mmap");
    proc::lib = reinterpret_cast<uint64_t>(lib);

    // ---- 1. class-name offset calibration -------------------------------------------------
    const int name_off = 0x10;  // pretend Il2CppClass.name lives at +0x10
    struct Known { uint64_t rva; const char* name; };
    const Known known[] = {{v100::player_typeinfo, "ake"}, {v100::registry_typeinfo, "gat"}, {v100::store_typeinfo, "gzx"},
                           {v100::transforms_typeinfo, "dwc"}, {v100::model_typeinfo, "jgi"}, {v100::stats_typeinfo, "ylw"},
                           {v100::weapon_part_typeinfo, "nrk"}, {v100::ios_typeinfo, "ios"}};
    auto make_class = [&](const char* name) {
        uint8_t* k = static_cast<uint8_t*>(calloc(1, 0x300));
        char* text = static_cast<char*>(calloc(1, 16)); std::strcpy(text, name);
        *reinterpret_cast<uint64_t*>(k + name_off) = reinterpret_cast<uint64_t>(text);
        // Decoys: other pointers to other strings must not confuse the measurement.
        for (int o = 0x20; o < 0x100; o += 8) { char* d = static_cast<char*>(calloc(1, 16)); std::strcpy(d, "zzz"); *reinterpret_cast<uint64_t*>(k + o) = reinterpret_cast<uint64_t>(d); }
        return k;
    };
    for (const auto& k : known) *reinterpret_cast<uint64_t*>(proc::lib + k.rva) = reinterpret_cast<uint64_t>(make_class(k.name));
    klass::g_pid = -1; klass::g_calibration_stamp = 0;
    CHECK(klass::calibrate(), "calibrate");
    CHECK(klass::g_name_offset == name_off, "offset=%d", klass::g_name_offset);

    // ---- 2. kind classification by name ------------------------------------------------------
    uint8_t* cim = make_class("cim"); uint8_t* qak = make_class("qak"); uint8_t* njg = make_class("njg"); uint8_t* other = make_class("xyz");
    CHECK(klass::kind_of(reinterpret_cast<uint64_t>(cim)) == klass::Kind::Grenade, "cim");
    CHECK(klass::kind_of(reinterpret_cast<uint64_t>(qak)) == klass::Kind::GrenadeCollision, "qak");
    CHECK(klass::kind_of(reinterpret_cast<uint64_t>(njg)) == klass::Kind::WeaponDrop, "njg");
    CHECK(klass::kind_of(reinterpret_cast<uint64_t>(other)) == klass::Kind::Other, "other");
    CHECK(klass::kind_of(rpm<uint64_t>(proc::lib + v100::player_typeinfo)) == klass::Kind::Player, "player");

    // ---- 3. Transform hierarchy: batched skeleton compose == reference decode_transform ----
    const int n = 40;
    auto* entries = static_cast<player::TransformEntry*>(calloc(1024, sizeof(player::TransformEntry)));
    auto* parents = static_cast<int*>(calloc(1024, sizeof(int)));
    for (int i = 0; i < n; ++i) {
        entries[i].position = Vector4(0.1f * i, 0.05f * (i % 5), 0.02f * i, 0.f);
        const float a = 0.05f * i; // small z rotation
        entries[i].rotation = Vector4(0.f, 0.f, std::sin(a * .5f), std::cos(a * .5f));
        entries[i].scale = Vector4(1.f, 1.f, 1.f, 0.f);
        parents[i] = i == 0 ? -1 : (i - 1) / 2;
    }
    uint8_t* hierarchy = static_cast<uint8_t*>(calloc(1, 0x100));
    *reinterpret_cast<uint64_t*>(hierarchy + 0x18) = reinterpret_cast<uint64_t>(entries);
    *reinterpret_cast<uint64_t*>(hierarchy + 0x20) = reinterpret_cast<uint64_t>(parents);
    player::g_transform_layout = {0x38, 0x40, 0x18, 0x20, proc::pid};

    skeleton::Cache cache{};
    cache.data = reinterpret_cast<uint64_t>(hierarchy);
    for (int b = 0; b < skeleton::kCount; ++b) { cache.known[b] = true; cache.index_of[b] = uint64_t(3 + b); cache.max_index = std::max(cache.max_index, 3 + b); }
    skeleton::Pose pose{};
    CHECK(skeleton::update(cache, pose), "update");
    int compared = 0;
    for (int b = 0; b < skeleton::kCount; ++b) {
        uint8_t* nativ = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(nativ + 0x38) = reinterpret_cast<uint64_t>(hierarchy);
        *reinterpret_cast<int*>(nativ + 0x40) = 3 + b;
        Vector3 ref{};
        CHECK(player::decode_transform(reinterpret_cast<uint64_t>(nativ), 0x38, 0x40, 0x18, 0x20, ref), "ref decode b=%d", b);
        CHECK(pose.valid[b], "valid b=%d", b);
        const Vector3 got = pose.position[b];
        CHECK(std::fabs(got.x - ref.x) < 1e-4f && std::fabs(got.y - ref.y) < 1e-4f && std::fabs(got.z - ref.z) < 1e-4f,
              "bone %d got(%.4f %.4f %.4f) ref(%.4f %.4f %.4f)", b, got.x, got.y, got.z, ref.x, ref.y, ref.z);
        ++compared;
    }
    CHECK(compared == skeleton::kCount, "compared");

    // ---- 4. BonesDictionary entry parsing and inner-dictionary probing ----------------------
    const int count = 21;
    uint8_t* raw = static_cast<uint8_t*>(calloc(1, 0x20 + count * 0x18));
    for (int i = 0; i < count; ++i) {
        uint8_t* e = raw + 0x20 + i * 0x18;
        int hash = i * 7 + 1, next = -1, key = i;
        uint64_t value = 0x7f0000100000ull + uint64_t(i) * 0x100;
        std::memcpy(e, &hash, 4); std::memcpy(e + 4, &next, 4); std::memcpy(e + 8, &key, 4); std::memcpy(e + 0x10, &value, 8);
    }
    uint8_t* dict = static_cast<uint8_t*>(calloc(1, 0x80));
    *reinterpret_cast<uint64_t*>(dict + v100::dict_entries) = reinterpret_cast<uint64_t>(raw);
    *reinterpret_cast<int*>(dict + v100::dict_count) = count;
    uint8_t* bones = static_cast<uint8_t*>(calloc(1, 0x80));
    *reinterpret_cast<uint64_t*>(bones + 0x18) = reinterpret_cast<uint64_t>(dict);  // inner dict not at 0x10 on purpose
    std::vector<skeleton::BoneRef> refs;
    skeleton::g_dict_offset = 0;
    CHECK(skeleton::bone_refs(reinterpret_cast<uint64_t>(bones), refs), "bone_refs");
    CHECK(skeleton::g_dict_offset == 0x18, "dict offset 0x%llx", (unsigned long long)skeleton::g_dict_offset);
    CHECK(refs.size() == size_t(count), "refs=%zu", refs.size());
    CHECK(refs[10].key == 10 && refs[10].managed == 0x7f0000100000ull + 10 * 0x100, "ref[10]");

    // ---- 5. camera holder read (xmt.Camera @0x20 -> managed -> cached ptr @0x10 -> native) ----
    uint8_t* native_cam = static_cast<uint8_t*>(calloc(1, 0x800));
    *reinterpret_cast<uint64_t*>(native_cam) = proc::lib + v100::data_relro_begin + 0x1000;      // vtable inside data range
    *reinterpret_cast<uint64_t*>(native_cam + 0x20) = 0x7f0000200000ull;                          // game object
    float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 1,2,3,1};   // column-major: w row = (0,0,0,1)
    float proj[16] = {1.2f,0,0,0, 0,1.8f,0,0, 0,0,-1.02f,-1, 0,0,-2.02f,0};
    std::memcpy(native_cam + v100::camera_view, view, 64); std::memcpy(native_cam + v100::camera_projection, proj, 64);
    uint8_t* managed_cam = static_cast<uint8_t*>(calloc(1, 0x40));
    *reinterpret_cast<uint64_t*>(managed_cam + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(native_cam);
    uint8_t* holder = static_cast<uint8_t*>(calloc(1, 0x80));
    *reinterpret_cast<uint64_t*>(holder + v100::gamecam_camera) = reinterpret_cast<uint64_t>(managed_cam);
    game_camera::Reading reading;
    CHECK(game_camera::read_holder(reinterpret_cast<uint64_t>(holder), reading), "read_holder");
    CHECK(reading.native == reinterpret_cast<uint64_t>(native_cam), "native");
    const float fov = game_camera::fov_degrees(reading.projection);
    CHECK(std::fabs(fov - 2.f * std::atan(1.f / 1.8f) * 57.29578f) < 0.01f, "fov=%.2f", fov);
    uint8_t* not_a_holder = static_cast<uint8_t*>(calloc(1, 0x80));
    game_camera::Reading r2;
    CHECK(!game_camera::read_holder(reinterpret_cast<uint64_t>(not_a_holder), r2), "false positive holder");

    // ---- 6. part collection finds a holder placed in the wmz Dictionary<Type, chx> ----------
    uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
    uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x100));
    *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
    uint8_t* dentries = static_cast<uint8_t*>(calloc(1, 0x20 + 3 * 0x18));
    for (int i = 0; i < 3; ++i) *reinterpret_cast<uint64_t*>(dentries + 0x20 + i * 0x18 + 0x10) = i == 2 ? reinterpret_cast<uint64_t>(holder) : reinterpret_cast<uint64_t>(not_a_holder);
    uint8_t* d2 = static_cast<uint8_t*>(calloc(1, 0x80));
    *reinterpret_cast<uint64_t*>(d2 + v100::dict_entries) = reinterpret_cast<uint64_t>(dentries);
    *reinterpret_cast<int*>(d2 + v100::dict_count) = 3;
    *reinterpret_cast<uint64_t*>(wmz + v100::wmz_dictionary) = reinterpret_cast<uint64_t>(d2);
    game_camera::g_holder = 0; game_camera::g_last_search = 0; game_camera::g_pid = -1;
    std::vector<uint64_t> players{reinterpret_cast<uint64_t>(net)};
    game_camera::Reading r3;
    CHECK(game_camera::acquire(players, reinterpret_cast<uint64_t>(net), r3), "acquire via dictionary");
    CHECK(r3.holder == reinterpret_cast<uint64_t>(holder), "holder");
    CHECK(game_camera::acquire(players, reinterpret_cast<uint64_t>(net), r3), "cached acquire");

    // ---- 9. whole rig: nodes below the hips, composed like the reference decode --------------------
    {
        CHECK(pose.nodes.size() > 5, "rig nodes=%zu", pose.nodes.size());
        int linked = 0, matched = 0;
        for (const auto& node : pose.nodes) {
            uint8_t* nativ = static_cast<uint8_t*>(calloc(1, 0x100));
            *reinterpret_cast<uint64_t*>(nativ + 0x38) = reinterpret_cast<uint64_t>(hierarchy);
            *reinterpret_cast<int*>(nativ + 0x40) = node.index;
            Vector3 ref{};
            if (player::decode_transform(reinterpret_cast<uint64_t>(nativ), 0x38, 0x40, 0x18, 0x20, ref) &&
                std::fabs(ref.x - node.position.x) + std::fabs(ref.y - node.position.y) + std::fabs(ref.z - node.position.z) < 1e-3f) ++matched;
            if (node.parent >= 0) {
                ++linked;
                bool ancestor = false;
                for (int i = parents[node.index], guard = 0; i >= 0 && guard < 64; i = parents[i], ++guard)
                    if (i == pose.nodes[size_t(node.parent)].index) { ancestor = true; break; }
                CHECK(ancestor, "parent link index=%d -> %d is not an ancestor", node.index, pose.nodes[size_t(node.parent)].index);
            }
        }
        CHECK(matched == int(pose.nodes.size()), "rig matched %d of %zu", matched, pose.nodes.size());
        int expected = 0;  // nodes with any drawn ancestor in this tree
        for (const auto& node : pose.nodes) {
            bool has = false;
            for (int i = parents[node.index], guard = 0; i >= 0 && guard < 64 && !has; i = parents[i], ++guard)
                for (const auto& other : pose.nodes) if (other.index == i) { has = true; break; }
            expected += has;
        }
        CHECK(linked == expected, "linked=%d expected=%d nodes=%zu", linked, expected, pose.nodes.size());

        // A weapon parented under the right hand is never part of the drawn rig.
        skeleton::Cache armed = cache;
        armed.plan_stamp = -1;
        for (int b = 0; b < skeleton::kCount; ++b) armed.mapped.emplace_back(b, int(armed.index_of[b]));
        const int weapon = 30;                                  // a transform below RHand in the test tree
        armed.mapped.emplace_back(51, 3 + skeleton::LHand);    // a mapped finger-like bone (a humanoid key above 20)
        skeleton::Pose armed_pose{};
        CHECK(skeleton::update(armed, armed_pose), "armed update");
        bool weapon_drawn = false;
        for (const auto& node : armed_pose.nodes) if (node.index == weapon) weapon_drawn = true;
        CHECK(!weapon_drawn, "weapon transform drawn");
        CHECK(armed_pose.nodes.size() <= size_t(skeleton::kCount + 1), "only mapped bones drawn: %zu", armed_pose.nodes.size());

        // A hierarchy this far into a big scene must still compose: bones at index 9000+.
        const int big = 12000;
        auto* big_entries = static_cast<player::TransformEntry*>(calloc(big + 64, sizeof(player::TransformEntry)));
        auto* big_parents = static_cast<int*>(calloc(big + 64, sizeof(int)));
        for (int i = 0; i < big + 64; ++i) { big_entries[i].rotation = Vector4(0, 0, 0, 1); big_entries[i].scale = Vector4(1, 1, 1, 0); big_parents[i] = i == 0 ? -1 : 0; }
        big_entries[0].position = Vector4(5.f, 0.f, 0.f, 0.f);
        const int base = 9000;
        for (int b = 0; b < skeleton::kCount; ++b) { big_parents[base + b] = b == 0 ? 0 : base + (b - 1) / 2 * (b > 0); big_entries[base + b].position = Vector4(0.f, 0.1f, 0.f, 0.f); }
        big_parents[base] = 0;
        uint8_t* big_hierarchy = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(big_hierarchy + 0x18) = reinterpret_cast<uint64_t>(big_entries);
        *reinterpret_cast<uint64_t*>(big_hierarchy + 0x20) = reinterpret_cast<uint64_t>(big_parents);
        skeleton::Cache far_cache{};
        far_cache.data = reinterpret_cast<uint64_t>(big_hierarchy);
        far_cache.min_index = base; far_cache.max_index = base + skeleton::kCount - 1;
        for (int b = 0; b < skeleton::kCount; ++b) { far_cache.known[b] = true; far_cache.index_of[b] = uint64_t(base + b); }
        skeleton::Pose far_pose{};
        CHECK(skeleton::update(far_cache, far_pose), "update at index %d", base);
        CHECK(std::fabs(far_pose.position[skeleton::Hips].x - 5.f) < 1e-3f, "far hips x=%.3f", far_pose.position[skeleton::Hips].x);
    }

    // ---- 10. bone index offset is measured on hips + head + foot, not taken from the root probe -----
    {
        // Root at (10, 0, 10) with hips 1.0, head 1.7 and a foot 0.1 above it. Each bone keeps a
        // decoy index at +0x40 (as on a player root) and the real one at +0x48. Decoy +0x44 holds
        // an index that puts every bone on the hips (the device's +0x14 trap).
        auto* e = static_cast<player::TransformEntry*>(calloc(8, sizeof(player::TransformEntry)));
        auto* par = static_cast<int*>(calloc(8, sizeof(int)));
        for (int i = 0; i < 8; ++i) { e[i].rotation = Vector4(0, 0, 0, 1); e[i].scale = Vector4(1, 1, 1, 0); par[i] = i == 0 ? -1 : 0; }
        e[0].position = Vector4(10.f, 0.f, 10.f, 0.f);
        e[1].position = Vector4(0.f, 1.0f, 0.f, 0.f);   // hips
        e[2].position = Vector4(0.f, 1.7f, 0.f, 0.f);   // head
        e[3].position = Vector4(0.1f, 0.1f, 0.f, 0.f);  // left foot
        uint8_t* h = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(h + 0x18) = reinterpret_cast<uint64_t>(e);
        *reinterpret_cast<uint64_t*>(h + 0x20) = reinterpret_cast<uint64_t>(par);
        auto bone = [&](int real) {
            uint8_t* n = static_cast<uint8_t*>(calloc(1, 0x100));
            *reinterpret_cast<uint64_t*>(n + 0x38) = reinterpret_cast<uint64_t>(h);
            *reinterpret_cast<int*>(n + 0x40) = 0;
            *reinterpret_cast<int*>(n + 0x44) = 1;          // decoy: always the hips
            *reinterpret_cast<int*>(n + 0x48) = real;
            return reinterpret_cast<uint64_t>(n);
        };
        skeleton::BoneNatives natives{};
        natives[skeleton::Hips] = bone(1);
        natives[skeleton::Head] = bone(2);
        natives[skeleton::LFoot] = bone(3);
        const player::TransformLayout probed{0x38, 0x40, 0x18, 0x20, proc::pid};
        skeleton::g_index_offset = -1; skeleton::g_index_pid = -1;
        CHECK(skeleton::bone_index_offset(natives, Vector3(10.f, 0.f, 10.f), probed) == 0x48,
              "measured index offset +0x%x", skeleton::g_index_offset);
        // A locked offset that stops fitting is measured again after three misses.
        skeleton::g_index_offset = 0x44;
        for (int i = 0; i < 3; ++i) skeleton::bone_index_offset(natives, Vector3(10.f, 0.f, 10.f), probed);
        CHECK(skeleton::g_index_offset == 0x48, "re-measured after misses: +0x%x", skeleton::g_index_offset);
        skeleton::g_index_offset = -1; skeleton::g_index_pid = -1;
        CHECK(skeleton::bone_index_offset(natives, Vector3(50.f, 0.f, 50.f), probed) == -1,
              "no offset when the rig is nowhere near the feet");
    }

    // ---- 7. find_model: jgi directly in the parts, and jgi behind a vds part at +0x58 ------------
    {
        const uint64_t jgi_class = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
        uint8_t* jgi = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(jgi) = jgi_class;
        uint8_t* vds = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(vds + v100::part_model) = reinterpret_cast<uint64_t>(jgi);
        uint8_t* decoy = static_cast<uint8_t*>(calloc(1, 0x80));
        uint8_t* wmz2 = static_cast<uint8_t*>(calloc(1, 0x100));
        uint8_t* net2 = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(net2 + v100::net_systems) = reinterpret_cast<uint64_t>(wmz2);
        uint8_t* arr = static_cast<uint8_t*>(calloc(1, 0x20 + 2 * 8));
        *reinterpret_cast<uint64_t*>(arr + v100::array_length) = 2;
        *reinterpret_cast<uint64_t*>(arr + v100::array_data) = reinterpret_cast<uint64_t>(decoy);
        *reinterpret_cast<uint64_t*>(arr + v100::array_data + 8) = reinterpret_cast<uint64_t>(vds);
        *reinterpret_cast<uint64_t*>(wmz2 + v100::wmz_first_array) = reinterpret_cast<uint64_t>(arr);
        CHECK(skeleton::find_model(reinterpret_cast<uint64_t>(net2)) == reinterpret_cast<uint64_t>(jgi), "jgi behind vds");
        *reinterpret_cast<uint64_t*>(arr + v100::array_data + 8) = reinterpret_cast<uint64_t>(jgi);
        CHECK(skeleton::find_model(reinterpret_cast<uint64_t>(net2)) == reinterpret_cast<uint64_t>(jgi), "jgi direct");
        *reinterpret_cast<uint64_t*>(arr + v100::array_data + 8) = reinterpret_cast<uint64_t>(decoy);
        CHECK(skeleton::find_model(reinterpret_cast<uint64_t>(net2)) == 0, "no jgi");
    }

    // ---- 8. fog: field addresses read out of compiled accessor code -------------------------------
    // tests/fixtures/rs_accessors.c, built for aarch64 by clang: il2cpp wrappers with lazily filled
    // icall slots, native icalls with a TLS main-thread check and an error reporter, PLT calls into
    // symbols that are not there. Laid out and relocated here the way the loader would.
    {
        namespace fx = rs_fixture;
        const uint64_t lib = proc::lib;
        const uint64_t base = lib + 0x7800000;                      // fixture "libunity" inside the fake lib
        std::memcpy(reinterpret_cast<void*>(base), fx::image, fx::image_size);
        const uint64_t external = base - 0x1000;                    // what the loader binds missing symbols to
        const uint32_t ret_insn = 0xD65F03C0;
        std::memcpy(reinterpret_cast<void*>(external), &ret_insn, 4);
        for (const auto& r : fx::relocs) {
            uint64_t value = 0;
            if (r.kind == fx::Relative) value = base + r.addend;
            else if (r.kind == fx::Symbol && r.defined) value = base + r.value + r.addend;
            else if (r.kind == fx::Symbol) value = external;        // SetDirty, il2cpp_codegen_resolve_icall, ...
            else continue;                                          // TLS descriptor: left 0
            *reinterpret_cast<uint64_t*>(base + r.offset) = value;
        }
        auto at = [&](uint64_t off) { return reinterpret_cast<uint64_t*>(base + off); };

        uint8_t* rs = static_cast<uint8_t*>(calloc(1, 0x200));
        auto put_f = [](uint8_t* o, int off, float v) { std::memcpy(o + off, &v, 4); };
        auto init_rs = [&](uint8_t* o) {  // device layout: fog 0x40, mode 0x44, color 0x48, start 0x58, end 0x5C, density 0x60
            o[0x40] = 0;
            const int32_t mode = 3; std::memcpy(o + 0x44, &mode, 4);
            put_f(o, 0x48, .5f); put_f(o, 0x4C, .5f); put_f(o, 0x50, .5f); put_f(o, 0x54, 1.f);
            put_f(o, 0x58, 0.f); put_f(o, 0x5C, 300.f); put_f(o, 0x60, .01f);
            put_f(o, 0x70, .1f); put_f(o, 0x74, .2f); put_f(o, 0x78, .3f); put_f(o, 0x7C, 1.f);   // ambient sky
            put_f(o, 0x80, .3f); put_f(o, 0x84, .35f); put_f(o, 0x88, .4f); put_f(o, 0x8C, 1.f);  // ambient equator
        };
        init_rs(rs);
        at(fx::g_managers)[7] = reinterpret_cast<uint64_t>(rs);

        // Slots the game has used are filled; the rest stay empty, as in a real process.
        *at(fx::slot_get_start) = base + fx::RenderSettings_get_fogStartDistance;
        *at(fx::slot_set_end) = base + fx::RenderSettings_set_fogEndDistance;       // end getter slot left empty
        *at(fx::slot_set_start) = base + fx::RenderSettings_set_fogStartDistance;
        *at(fx::slot_get_fog) = base + fx::RenderSettings_get_fog;
        *at(fx::slot_get_mode) = base + fx::RenderSettings_get_fogMode;
        *at(fx::slot_get_color) = base + fx::RenderSettings_get_fogColor_Injected;
        *at(fx::slot_get_object) = base + fx::RenderSettings_GetRenderSettings_Injected;
        // slot_get_equator stays empty: found through the icall table below.

        // The script.json RVAs jump to the compiled wrappers; the others trap.
        auto branch = [](uint64_t from, uint64_t to) { return 0x14000000u | (uint32_t((to - from) >> 2) & 0x3FFFFFFu); };
        auto route = [&](uint64_t rva, uint64_t target) {
            const uint32_t w = target ? branch(lib + rva, target) : 0xD4200000u;
            std::memcpy(reinterpret_cast<void*>(lib + rva), &w, 4);
        };
        const uint64_t traps[] = {v100::rs_set_fog, v100::rs_set_fog_mode, v100::rs_get_fog_density,
                                  v100::rs_set_fog_density, v100::rs_set_fog_color_injected, v100::rs_set_ambient_equator_injected,
                                  v100::rs_get_ambient_sky_injected, v100::rs_set_ambient_sky_injected,
                                  v100::rs_get_ambient_intensity, v100::rs_set_ambient_intensity};
        for (uint64_t rva : traps) route(rva, 0);
        route(v100::rs_get_fog, base + fx::UnityEngine_RenderSettings__get_fog);
        route(v100::rs_get_fog_start, base + fx::UnityEngine_RenderSettings__get_fogStartDistance);
        route(v100::rs_get_fog_end, base + fx::UnityEngine_RenderSettings__get_fogEndDistance);
        route(v100::rs_set_fog_end, base + fx::UnityEngine_RenderSettings__set_fogEndDistance);
        route(v100::rs_set_fog_start, base + fx::UnityEngine_RenderSettings__set_fogStartDistance);
        route(v100::rs_get_fog_mode, base + fx::UnityEngine_RenderSettings__get_fogMode);
        route(v100::rs_get_fog_color_injected, base + fx::UnityEngine_RenderSettings__get_fogColor_Injected);
        route(v100::rs_get_ambient_equator_injected, base + fx::UnityEngine_RenderSettings__get_ambientEquatorColor_Injected);
        route(v100::rs_get_render_settings_injected, base + fx::UnityEngine_RenderSettings__GetRenderSettings_Injected);

        // A file-backed "libunity.so" holding icall names, a {name, fn} pair (equator) and a
        // registration call site in an executable page (sky).
        auto adrp = [](int rd, uint64_t pc, uint64_t target) {
            const int64_t imm = (int64_t(target & ~uint64_t(0xFFF)) - int64_t(pc & ~uint64_t(0xFFF))) >> 12;
            return uint32_t(0x90000000u | (uint32_t(imm & 3) << 29) | (uint32_t((imm >> 2) & 0x7FFFF) << 5) | uint32_t(rd));
        };
        auto add_imm = [](int rd, int rn, uint32_t imm) { return 0x91000000u | (imm << 10) | (uint32_t(rn) << 5) | uint32_t(rd); };
        char unity_path[] = "/tmp/bycmd_selftest_XXXXXX";
        CHECK(mkdtemp(unity_path) != nullptr, "mkdtemp");
        const std::string so = std::string(unity_path) + "/libunity.so";
        {
            FILE* f = std::fopen(so.c_str(), "wb");
            std::vector<uint8_t> zero(0x2000, 0);
            std::fwrite(zero.data(), 1, zero.size(), f);
            std::fclose(f);
        }
        const int fd = open(so.c_str(), O_RDWR);
        uint8_t* unity = static_cast<uint8_t*>(mmap(nullptr, 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
        CHECK(unity != MAP_FAILED, "unity mmap");
        const char eq_name[] = "UnityEngine.RenderSettings::get_ambientEquatorColor_Injected";
        const char sky_name[] = "UnityEngine.RenderSettings::get_ambientSkyColor_Injected(UnityEngine.Color&)";
        std::memcpy(unity + 0x100, eq_name, sizeof(eq_name));
        std::memcpy(unity + 0x200, sky_name, sizeof(sky_name));
        *reinterpret_cast<uint64_t*>(unity + 0x400) = reinterpret_cast<uint64_t>(unity + 0x100);
        *reinterpret_cast<uint64_t*>(unity + 0x408) = base + fx::RenderSettings_get_ambientEquatorColor_Injected;
        // The Camera matrix setter has no managed wrapper (stripped): only its {name, fn} pair.
        const char cam_set_name[] = "UnityEngine.Camera::set_worldToCameraMatrix_Injected";
        std::memcpy(unity + 0x300, cam_set_name, sizeof(cam_set_name));
        *reinterpret_cast<uint64_t*>(unity + 0x420) = reinterpret_cast<uint64_t>(unity + 0x300);
        *reinterpret_cast<uint64_t*>(unity + 0x428) = base + fx::Camera_set_worldToCameraMatrix_Injected;
        const uint64_t site = reinterpret_cast<uint64_t>(unity + 0x1000);
        const uint64_t sky_fn = base + fx::RenderSettings_get_ambientSkyColor_Injected;
        const int64_t reach = int64_t(sky_fn) - int64_t(site);
        const bool sky_reachable = reach > -(int64_t(1) << 32) && reach < (int64_t(1) << 32);
        if (sky_reachable) {
            const uint32_t words[] = {0xD503201F, adrp(0, site + 4, reinterpret_cast<uint64_t>(unity + 0x200)),
                                      add_imm(0, 0, uint32_t(reinterpret_cast<uint64_t>(unity + 0x200) & 0xFFF)),
                                      adrp(1, site + 12, sky_fn), add_imm(1, 1, uint32_t(sky_fn & 0xFFF)),
                                      branch(site + 20, site) /* b scripting_add_internal_call */, 0xD65F03C0};
            std::memcpy(unity + 0x1000, words, sizeof(words));
        }
        const bool exec_ok = mprotect(unity + 0x1000, 0x1000, PROT_READ | PROT_EXEC) == 0;

        cfg::fog::replay = true; cfg::fog::constants = true;   // the whole engine path; the parts are tested alone below
        cfg::fog::enabled = true; cfg::fog::auto_color = true; cfg::fog::min_distance = 25.f; cfg::fog::max_distance = 90.f;
        worldfog::g_pid = -1;
        // The renderer still holds the map's fog params (start 0, end 300, density 0.01). The
        // search is limited to the fake library: this process would otherwise find its own
        // buffers and thread stacks (the game is always another process).
        float* params_live = reinterpret_cast<float*>(base + fx::g_fog_params);
        worldfog::params_for(.01f, 0.f, 300.f, params_live);
        g_scan_maps = {remote_map{lib, lib + 0xC500000, true}};
        worldfog::tick();                       // starts the resolution thread
        worldfog::wait();
        CHECK(worldfog::g_params.size() == 1 && worldfog::g_params[0].address == base + fx::g_fog_params,
              "renderer params found: %zu", worldfog::g_params.size());
        worldfog::tick();                       // applies (field changes on the worker)
        worldfog::wait();
        const uint64_t obj = reinterpret_cast<uint64_t>(rs);
        auto how = [](worldfog::Field f) { return worldfog::g_fields[f].how.c_str(); };
        CHECK(worldfog::g_object == obj, "GetRenderSettings traced");
        CHECK(worldfog::g_fields[worldfog::Start].address == obj + 0x58, "start: %s", how(worldfog::Start));
        CHECK(worldfog::g_fields[worldfog::End].address == obj + 0x5C && worldfog::g_fields[worldfog::End].setter, "end via setter: %s", how(worldfog::End));
        CHECK(worldfog::g_fields[worldfog::Fog].address == obj + 0x40, "fog: %s", how(worldfog::Fog));
        CHECK(worldfog::g_fields[worldfog::Mode].address == obj + 0x44, "mode: %s", how(worldfog::Mode));
        CHECK(worldfog::g_fields[worldfog::FogColor].address == obj + 0x48, "color: %s", how(worldfog::FogColor));
        CHECK(worldfog::g_fields[worldfog::Density].address == obj + 0x60 && worldfog::g_fields[worldfog::Density].how == "inferred from fog start",
              "density inferred: %s", how(worldfog::Density));
        CHECK(worldfog::g_setter[worldfog::Start] && worldfog::g_setter[worldfog::End], "start/end setters found");
        CHECK(worldfog::g_fields[worldfog::Equator].address == obj + 0x80, "equator via icall table: %s", how(worldfog::Equator));
        if (sky_reachable && exec_ok)
            CHECK(worldfog::g_fields[worldfog::Sky].address == obj + 0x70, "sky via registration site: %s", how(worldfog::Sky));
        else std::printf("note: registration call-site case skipped (exec=%d reach=%d)\n", exec_ok, sky_reachable);

        float f = 0.f;
        int32_t mode = 0;
        CHECK(rs[0x40] == 1, "fog on");
        std::memcpy(&mode, rs + 0x44, 4); CHECK(mode == 1, "linear");
        std::memcpy(&f, rs + 0x58, 4); CHECK(f == 25.f, "start %.1f", f);
        std::memcpy(&f, rs + 0x5C, 4); CHECK(f == 90.f, "end %.1f", f);
        std::memcpy(&f, rs + 0x48, 4); CHECK(std::fabs(f - .3f) < 1e-5f, "estimated color r=%.2f", f);
        std::memcpy(&f, rs + 0x60, 4); CHECK(std::fabs(f - .01f) < 1e-6f, "density untouched");
        // The replayed setters ran ApplyFog: the renderer's parameters follow the new range.
        const float* params = reinterpret_cast<const float*>(base + fx::g_fog_params);
        CHECK(std::fabs(params[2] - (-1.f / 65.f)) < 1e-6f && std::fabs(params[3] - (90.f / 65.f)) < 1e-5f,
              "fog params from replayed setters: %f %f", params[2], params[3]);
        CHECK(std::fabs(params[1] - .01f * 1.4426950f) < 1e-7f, "density term %f", params[1]);

        cfg::fog::auto_color = false; cfg::fog::color = ImVec4(.9f, .1f, .2f, 1.f);
        worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
        worldfog::tick();
        worldfog::wait();
        std::memcpy(&f, rs + 0x48, 4); CHECK(std::fabs(f - .9f) < 1e-5f, "manual color r=%.2f", f);

        // Scene load: RenderSettings moves; the start trace notices and everything follows.
        uint8_t* rs2 = static_cast<uint8_t*>(calloc(1, 0x200));
        init_rs(rs2);
        at(fx::g_managers)[7] = reinterpret_cast<uint64_t>(rs2);
        worldfog::g_last_check = 0;
        worldfog::tick();                       // notices the move
        worldfog::tick();                       // resolves again
        worldfog::wait();
        worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
        worldfog::tick();                       // applies
        worldfog::wait();
        CHECK(worldfog::g_fields[worldfog::Start].address == reinterpret_cast<uint64_t>(rs2) + 0x58, "followed the move");
        CHECK(rs2[0x40] == 1, "fog on after the move");

        cfg::fog::enabled = false;
        worldfog::tick();
        worldfog::wait();
        std::memcpy(&f, rs2 + 0x5C, 4);
        std::memcpy(&mode, rs2 + 0x44, 4);
        CHECK(rs2[0x40] == 0 && mode == 3 && f == 300.f, "restored fog=%d mode=%d end=%.1f", rs2[0x40], mode, f);
        CHECK(std::fabs(params[3] - 1.f) < 1e-5f, "renderer params restored with the range: %f", params[3]);

        // Night mode alone (fog off): dark ambient light, a dark closing fog; everything back when it is off.
        {
            auto rd = [&](int off) { float v; std::memcpy(&v, rs2 + off, 4); return v; };
            cfg::night::enabled = true; cfg::night::strength = 1.f;
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            CHECK(rs2[0x40] == 1 && std::fabs(rd(0x5C) - 70.f) < 1e-3f && rd(0x58) == 0.f, "night fog: on, 0..%.1f m", rd(0x5C));
            CHECK(std::fabs(rd(0x48) - .02f) < 1e-4f && std::fabs(rd(0x50) - .08f) < 1e-4f, "night fog color %.3f %.3f", rd(0x48), rd(0x50));
            CHECK(std::fabs(rd(0x80) - .02f) < 1e-4f && std::fabs(rd(0x88) - .07f) < 1e-4f, "night ambient equator %.3f %.3f", rd(0x80), rd(0x88));
            CHECK(std::fabs(rd(0x70) - .03f) < 1e-4f && std::fabs(rd(0x78) - .12f) < 1e-4f, "night ambient sky %.3f %.3f", rd(0x70), rd(0x78));
            cfg::night::enabled = false;
            worldfog::tick();
            worldfog::wait();
            CHECK(rs2[0x40] == 0 && std::fabs(rd(0x80) - .3f) < 1e-5f && std::fabs(rd(0x70) - .1f) < 1e-5f && rd(0x5C) == 300.f,
                  "night off: the map's fog and ambient light are back (fog %d end %.1f equator %.2f)", rs2[0x40], rd(0x5C), rd(0x80));

            // Constants only: RenderSettings is never written, the renderer's fog constants carry the night fog.
            cfg::fog::replay = false; cfg::fog::constants = true;
            cfg::night::enabled = true;
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            const float* safe_params = reinterpret_cast<const float*>(base + fx::g_fog_params);
            CHECK(rs2[0x40] == 0 && rd(0x5C) == 300.f && std::fabs(rd(0x80) - .3f) < 1e-5f && std::fabs(rd(0x48) - .5f) < 1e-5f,
                  "constants only: RenderSettings untouched (fog %d end %.1f equator %.2f)", rs2[0x40], rd(0x5C), rd(0x80));
            CHECK(std::fabs(safe_params[2] - (-1.f / 70.f)) < 1e-6f, "constants only: the renderer's fog constants carry the night fog (%f)", safe_params[2]);
            cfg::night::enabled = false;
            worldfog::tick();
            worldfog::wait();
            CHECK(std::fabs(safe_params[2] - (-1.f / 300.f)) < 1e-6f, "constants only, off: the constants are back (%f)", safe_params[2]);

            // Both off: nothing of the game is read or written.
            cfg::fog::replay = false; cfg::fog::constants = false;
            cfg::night::enabled = true; cfg::fog::enabled = true;
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            worldfog::g_last_write = 0; worldfog::g_last_replay = 0;
            worldfog::tick();
            worldfog::wait();
            CHECK(rs2[0x40] == 0 && rd(0x5C) == 300.f && std::fabs(safe_params[2] - (-1.f / 300.f)) < 1e-6f, "both off: nothing touched");
            cfg::night::enabled = false;
            cfg::fog::enabled = false;
            cfg::fog::replay = true; cfg::fog::constants = true;

            // The game putting RenderSettings back every time: the engine setters are paused, not run ten times a second.
            cfg::fog::constants = false; cfg::fog::enabled = true;
            worldfog::g_paused = false; worldfog::g_fights = 0;
            worldfog::tick();   // (re)start from the off state
            worldfog::wait();
            for (int i = 0; i < 9; ++i) {
                rs2[0x40] = 0;   // the game's own scene logic sets it back
                worldfog::g_last_replay = diag::now_ms() - 1500;
                worldfog::g_last_write = 0;
                worldfog::tick();
                worldfog::wait();
            }
            CHECK(worldfog::g_paused, "the game puts the fields back every time: the setters are paused (fights %d)", worldfog::g_fights);
            cfg::fog::enabled = false;
            worldfog::tick();
            worldfog::wait();
            CHECK(!worldfog::g_paused, "switching fog off clears the pause");
            cfg::fog::replay = true; cfg::fog::constants = true;
        }

        // Occlusion bypass: the culling offset comes from tracing Animator.get_cullingMode_Injected
        // on a real enemy animator; enemies go to AlwaysAnimate and come back when switched off.
        {
            route(v100::animator_get_culling_injected, base + fx::UnityEngine_Animator__get_cullingMode_Injected);
            route(v100::animator_set_culling_injected, base + fx::UnityEngine_Animator__set_cullingMode_Injected);
            *at(fx::slot_get_culling) = base + fx::Animator_get_cullingMode_Injected;
            *at(fx::slot_set_culling) = base + fx::Animator_set_cullingMode_Injected;
            const uint64_t jgi_class = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
            auto make_enemy = [&](int32_t mode, uint8_t*& native_out) {
                uint8_t* native = static_cast<uint8_t*>(calloc(1, 0x100));
                std::memcpy(native + 0x58, &mode, 4);
                uint8_t* managed = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(native);
                uint8_t* model = static_cast<uint8_t*>(calloc(1, 0x80));
                *reinterpret_cast<uint64_t*>(model) = jgi_class;
                *reinterpret_cast<uint64_t*>(model + v100::model_animator) = reinterpret_cast<uint64_t>(managed);
                uint8_t* array = static_cast<uint8_t*>(calloc(1, 0x30));
                *reinterpret_cast<uint64_t*>(array + v100::array_length) = 1;
                *reinterpret_cast<uint64_t*>(array + v100::array_data) = reinterpret_cast<uint64_t>(model);
                uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
                *reinterpret_cast<uint64_t*>(wmz + v100::wmz_first_array) = reinterpret_cast<uint64_t>(array);
                uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x100));
                *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
                native_out = native;
                return reinterpret_cast<uint64_t>(net);
            };
            uint8_t *a1 = nullptr, *a2 = nullptr;
            const std::vector<uint64_t> enemies{make_enemy(1, a1), make_enemy(2, a2)};
            animcull::g_pid = -1;
            animcull::tick(enemies, true);          // starts resolution
            animcull::wait();
            animcull::tick(enemies, true);          // applies
            int32_t m1 = -1, m2 = -1;
            std::memcpy(&m1, a1 + 0x58, 4); std::memcpy(&m2, a2 + 0x58, 4);
            CHECK(animcull::g_offset.load() == 0x58, "culling offset +0x%x", animcull::g_offset.load());
            CHECK(m1 == 0 && m2 == 0, "AlwaysAnimate applied (%d %d)", m1, m2);
            CHECK(animcull::g_setter.load() != 0, "culling setter found");
            const int* bookkeeping = reinterpret_cast<const int*>(base + fx::g_always_animate_count);
            CHECK(*bookkeeping == 2, "setter side effects replayed: %d", *bookkeeping);
            animcull::tick(enemies, false);
            std::memcpy(&m1, a1 + 0x58, 4); std::memcpy(&m2, a2 + 0x58, 4);
            CHECK(m1 == 1 && m2 == 2, "culling restored (%d %d)", m1, m2);
            CHECK(*bookkeeping == 0, "restore replayed through the setter: %d", *bookkeeping);
        }

        // Chams: the material array is located by tracing Renderer.GetMaterialCount on a live
        // renderer; enemies get the chosen material in every slot, teammates are left alone, and
        // switching off restores each slot.
        {
            route(v100::renderer_material_count_injected, base + fx::UnityEngine_Renderer__GetMaterialCount_Injected);
            *at(fx::slot_get_material_count) = base + fx::Renderer_GetMaterialCount_Injected;
            const uint64_t jgi_class = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
            const uint64_t stats_class = rpm<uint64_t>(proc::lib + v100::stats_typeinfo);
            auto make_player = [&](int team, bool local, int32_t* ids, int count, uint8_t*& renderer_out, int32_t outline_id) {
                uint8_t* renderer = static_cast<uint8_t*>(calloc(1, 0x200));
                *reinterpret_cast<uint64_t*>(renderer + 0x118) = reinterpret_cast<uint64_t>(ids);
                *reinterpret_cast<uint64_t*>(renderer + 0x128) = uint64_t(count);
                *reinterpret_cast<uint64_t*>(renderer + 0x130) = uint64_t(count);
                uint8_t* managed_renderer = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed_renderer + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(renderer);
                uint8_t* material = static_cast<uint8_t*>(calloc(1, 0x40));
                std::memcpy(material + v100::native_instance_id, &outline_id, 4);
                uint8_t* managed_material = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed_material + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(material);
                uint8_t* bindings = static_cast<uint8_t*>(calloc(1, 0x80));
                *reinterpret_cast<uint64_t*>(bindings + v100::bindings_character_renderer) = reinterpret_cast<uint64_t>(managed_renderer);
                *reinterpret_cast<uint64_t*>(bindings + v100::bindings_outline_material) = reinterpret_cast<uint64_t>(managed_material);
                uint8_t* model = static_cast<uint8_t*>(calloc(1, 0x80));
                *reinterpret_cast<uint64_t*>(model) = jgi_class;
                *reinterpret_cast<uint64_t*>(model + v100::model_bindings) = reinterpret_cast<uint64_t>(bindings);
                uint8_t* array = static_cast<uint8_t*>(calloc(1, 0x30));
                *reinterpret_cast<uint64_t*>(array + v100::array_length) = 1;
                *reinterpret_cast<uint64_t*>(array + v100::array_data) = reinterpret_cast<uint64_t>(model);
                uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
                *reinterpret_cast<uint64_t*>(wmz + v100::wmz_first_array) = reinterpret_cast<uint64_t>(array);
                uint8_t* stats = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(stats) = stats_class;
                *reinterpret_cast<uint16_t*>(stats + v100::stats_hp) = 100;
                *reinterpret_cast<uint16_t*>(stats + v100::stats_max_hp) = 100;
                uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x200));
                *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
                *reinterpret_cast<uint64_t*>(net + v100::player_stats) = reinterpret_cast<uint64_t>(stats);
                net[v100::player_team] = uint8_t(team);
                net[v100::player_local] = local ? 1 : 0;
                renderer_out = renderer;
                return reinterpret_cast<uint64_t>(net);
            };
            int32_t enemy_ids[2] = {-5000, -5002}, mate_ids[1] = {-6000}, local_ids[1] = {-7000};
            uint8_t *enemy_r = nullptr, *mate_r = nullptr, *local_r = nullptr;
            const uint64_t local = make_player(2, true, local_ids, 1, local_r, -9000);
            const uint64_t mate = make_player(2, false, mate_ids, 1, mate_r, -9000);
            const uint64_t enemy = make_player(1, false, enemy_ids, 2, enemy_r, -9000);
            const std::vector<uint64_t> players{local, mate, enemy};
            route(v100::canvas_default_material_injected, base + fx::UnityEngine_Canvas__GetDefaultCanvasMaterial_Injected);
            // The Canvas accessor's icall slot stays empty here: the material comes from uGUI's
            // Graphic.s_DefaultUI, through a TypeInfo global in libunity's data.
            uint8_t* ui_material = static_cast<uint8_t*>(calloc(1, 0x40));
            const int32_t ui_id = -11000;
            std::memcpy(ui_material + v100::native_instance_id, &ui_id, 4);
            *reinterpret_cast<uint64_t*>(ui_material) = reinterpret_cast<uint64_t>(ui_material) + 0x20;  // a vtable pointer
            *at(fx::g_default_ui_material) = reinterpret_cast<uint64_t>(ui_material);
            uint8_t* graphic_class = static_cast<uint8_t*>(calloc(1, 0x300));
            char* graphic_name = static_cast<char*>(calloc(1, 16));
            std::strcpy(graphic_name, "Graphic");
            *reinterpret_cast<uint64_t*>(graphic_class + klass::g_name_offset) = reinterpret_cast<uint64_t>(graphic_name);
            uint8_t* graphic_statics = static_cast<uint8_t*>(calloc(1, 0x40));
            uint8_t* managed_ui = static_cast<uint8_t*>(calloc(1, 0x40));
            *reinterpret_cast<uint64_t*>(managed_ui + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(ui_material);
            *reinterpret_cast<uint64_t*>(graphic_statics) = reinterpret_cast<uint64_t>(managed_ui);  // s_DefaultUI
            *reinterpret_cast<uint64_t*>(graphic_class + v100::il2cpp_class_static_fields_profile) = reinterpret_cast<uint64_t>(graphic_statics);
            *reinterpret_cast<uint64_t*>(unity + 0x600) = reinterpret_cast<uint64_t>(graphic_class);  // UnityEngine_UI_Graphic_TypeInfo
            cfg::chams::enabled = true; cfg::chams::enemies = true; cfg::chams::teammates = false; cfg::chams::local_body = false;
            cfg::chams::local_arms = false; cfg::chams::local_weapon = false; cfg::chams::material = 0; cfg::chams::through_walls = false;
            chams::g_pid = -1;
            chams::tick(players, local, 2);   // starts resolution
            chams::wait();
            CHECK(chams::g_data_offset.load() == 0x118 && chams::g_size_offset.load() == 0x128,
                  "material array +0x%x size +0x%x", chams::g_data_offset.load(), chams::g_size_offset.load());
            chams::tick(players, local, 2);   // applies
            CHECK(enemy_ids[0] == 0 && enemy_ids[1] == 0, "flat chams on the enemy (%d %d)", enemy_ids[0], enemy_ids[1]);
            CHECK(mate_ids[0] == -6000 && local_ids[0] == -7000, "teammate and local untouched");
            cfg::chams::material = 2;
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);
            CHECK(enemy_ids[0] == -9000 && enemy_ids[1] == -9000, "outline material id (%d %d)", enemy_ids[0], enemy_ids[1]);
            CHECK(chams::g_ui_material.load() == ui_id, "default UI material traced: %d", chams::g_ui_material.load());
            cfg::chams::through_walls = true;
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);
            CHECK(enemy_ids[0] == ui_id && enemy_ids[1] == ui_id, "through walls uses the UI material (%d %d)", enemy_ids[0], enemy_ids[1]);
            cfg::chams::through_walls = false;
            cfg::chams::teammates = true;
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);
            CHECK(mate_ids[0] == -9000 && local_ids[0] == -7000, "teammates on, the local player still untouched");

            // Local body, first-person arms and gloves, and the local weapon found by its owner.
            uint8_t* local_net = reinterpret_cast<uint8_t*>(local);
            const uint64_t local_bindings = chams::bindings_of(local);
            CHECK(local_bindings != 0, "local bindings");
            int32_t arms_ids[1] = {-7100}, gloves_ids[1] = {-7200}, weapon_ids[2] = {-7300, -7301};
            auto make_renderer = [&](int32_t* ids, int count) {
                uint8_t* r = static_cast<uint8_t*>(calloc(1, 0x200));
                *reinterpret_cast<uint64_t*>(r + 0x118) = reinterpret_cast<uint64_t>(ids);
                *reinterpret_cast<uint64_t*>(r + 0x128) = uint64_t(count);
                uint8_t* managed = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(r);
                return reinterpret_cast<uint64_t>(managed);
            };
            *reinterpret_cast<uint64_t*>(local_bindings + v100::bindings_arms_renderer) = make_renderer(arms_ids, 1);
            *reinterpret_cast<uint64_t*>(local_bindings + v100::bindings_gloves_renderer) = make_renderer(gloves_ids, 1);
            // gby: class named "gby", List<MeshRenderer> at +0x30, owner bindings at +0x48; it lives in
            // its own mapping so the search can be limited to it.
            uint8_t* heap = static_cast<uint8_t*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            uint8_t* gby_class = static_cast<uint8_t*>(calloc(1, 0x300));
            char* gby_name = static_cast<char*>(calloc(1, 16));
            std::strcpy(gby_name, "gby");
            *reinterpret_cast<uint64_t*>(gby_class + klass::g_name_offset) = reinterpret_cast<uint64_t>(gby_name);
            uint8_t* gby = heap + 0x4000;
            *reinterpret_cast<uint64_t*>(gby) = reinterpret_cast<uint64_t>(gby_class);
            uint8_t* items = static_cast<uint8_t*>(calloc(1, 0x40));
            *reinterpret_cast<uint64_t*>(items + v100::array_data) = make_renderer(weapon_ids, 2);
            uint8_t* list = static_cast<uint8_t*>(calloc(1, 0x40));
            *reinterpret_cast<uint64_t*>(list + v100::list_items) = reinterpret_cast<uint64_t>(items);
            *reinterpret_cast<int*>(list + v100::list_count) = 1;
            *reinterpret_cast<uint64_t*>(gby + 0x30) = reinterpret_cast<uint64_t>(list);
            *reinterpret_cast<uint64_t*>(gby + 0x48) = local_bindings;
            uint8_t* decoy = heap + 0x8000;  // same owner pointer, wrong class
            *reinterpret_cast<uint64_t*>(decoy + 0x48) = local_bindings;
            g_scan_maps = {remote_map{reinterpret_cast<uint64_t>(heap), reinterpret_cast<uint64_t>(heap) + 0x10000, true}};
            cfg::chams::local_body = true; cfg::chams::local_arms = true; cfg::chams::local_weapon = true;
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);   // starts the weapon search
            chams::wait();
            CHECK(chams::g_weapon_objects.size() == 1 && chams::g_weapon_objects[0] == reinterpret_cast<uint64_t>(gby),
                  "weapon content found: %zu", chams::g_weapon_objects.size());
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);
            CHECK(local_ids[0] == -9000, "local body (%d)", local_ids[0]);
            CHECK(arms_ids[0] == -9000 && gloves_ids[0] == -9000, "arms and gloves (%d %d)", arms_ids[0], gloves_ids[0]);
            CHECK(weapon_ids[0] == -9000 && weapon_ids[1] == -9000, "weapon (%d %d)", weapon_ids[0], weapon_ids[1]);
            (void)local_net;
            cfg::chams::local_body = false; cfg::chams::local_arms = false; cfg::chams::local_weapon = false;
            chams::g_last_apply = 0;
            chams::tick(players, local, 2);
            CHECK(local_ids[0] == -7000 && arms_ids[0] == -7100 && gloves_ids[0] == -7200 && weapon_ids[0] == -7300 && weapon_ids[1] == -7301,
                  "local targets restored when switched off");
            cfg::chams::enabled = false;
            chams::tick(players, local, 2);
            CHECK(enemy_ids[0] == -5000 && enemy_ids[1] == -5002 && mate_ids[0] == -6000, "materials restored (%d %d %d)",
                  enemy_ids[0], enemy_ids[1], mate_ids[0]);
        }

        // Third person: the implicit-matrix flag comes from the setter found by name (then from the
        // getter's flag check), the culling mask from its getter; the camera keeps what is written.
        {
            route(v100::camera_get_world_to_camera_injected, base + fx::UnityEngine_Camera__get_worldToCameraMatrix_Injected);
            route(v100::camera_get_culling_mask_injected, base + fx::UnityEngine_Camera__get_cullingMask_Injected);
            route(v100::camera_set_culling_mask_injected, 0);
            *at(fx::slot_get_world_to_camera) = base + fx::Camera_get_worldToCameraMatrix_Injected;
            *at(fx::slot_get_culling_mask) = base + fx::Camera_get_cullingMask_Injected;
            uint8_t* cam = static_cast<uint8_t*>(calloc(1, 0x300));
            *reinterpret_cast<uint64_t*>(cam) = proc::lib + v100::data_relro_begin + 0x10;
            *reinterpret_cast<uint64_t*>(cam + 0x20) = reinterpret_cast<uint64_t>(calloc(1, 0x100));
            cam[0x240] = 1;
            const uint32_t mask0 = 0x00FF00FF;
            std::memcpy(cam + 0x220, &mask0, 4);
            const Vector3 eye_pos(1.f, 2.f, 3.f), eye_euler(10.f, 30.f, 0.f);
            uint8_t* ckk = static_cast<uint8_t*>(calloc(1, 0x60));
            std::memcpy(ckk + v100::camstate_position, &eye_pos, 12);
            std::memcpy(ckk + v100::camstate_euler, &eye_euler, 12);
            uint8_t* holder = static_cast<uint8_t*>(calloc(1, 0x60));
            *reinterpret_cast<uint64_t*>(holder + v100::gamecam_state) = reinterpret_cast<uint64_t>(ckk);
            const Vector4 eye_q = thirdperson::euler(eye_euler);
            const matrix live = player::view_from_pose(eye_pos, eye_q);
            std::memcpy(cam + v100::camera_view, &live, sizeof(live));
            const matrix proj = player::perspective(60.f, 16.f / 9.f);
            std::memcpy(cam + v100::camera_projection, &proj, sizeof(proj));
            game_camera::Reading reading;
            reading.holder = reinterpret_cast<uint64_t>(holder);
            reading.native = reinterpret_cast<uint64_t>(cam);
            reading.view = live;
            cfg::thirdperson::enabled = true; cfg::thirdperson::distance = 3.f; cfg::thirdperson::height = 0.f;
            cfg::thirdperson::side = 0.f; cfg::thirdperson::show_body = true;
            thirdperson::g_pid = -1;
            matrix out{};
            CHECK(!thirdperson::tick(&reading, out), "first tick only starts the resolution");
            thirdperson::wait();
            CHECK(thirdperson::g_flag_offset.load() == 0x240 && thirdperson::g_matrix_offset.load() == 0x70,
                  "setter traced: flag +0x%x matrix +0x%x", thirdperson::g_flag_offset.load(), thirdperson::g_matrix_offset.load());
            CHECK(thirdperson::g_mask_offset.load() == 0x220, "culling mask +0x%x", thirdperson::g_mask_offset.load());
            CHECK(thirdperson::tick(&reading, out), "camera taken");
            CHECK(thirdperson::g_source == thirdperson::State, "pose from the ckk state");
            const Vector3 fwd = player::rotate(eye_q, Vector3(0.f, 0.f, 1.f));
            const matrix expected = player::view_from_pose(eye_pos - fwd * 3.f, eye_q);
            matrix written{};
            std::memcpy(&written, cam + 0x70, sizeof(written));
            CHECK(thirdperson::diff(out, expected) < 1e-5f && thirdperson::diff(written, expected) < 1e-5f, "third-person matrix written");
            CHECK(cam[0x240] == 0, "flag off (%u)", cam[0x240]);
            thirdperson::g_taken_at -= 1000;
            thirdperson::tick(&reading, out);
            CHECK(thirdperson::g_verified && !thirdperson::g_failed, "the matrix held");
            cfg::thirdperson::enabled = false;
            thirdperson::tick(&reading, out);
            uint32_t mask = 0;
            std::memcpy(&mask, cam + 0x220, 4);
            CHECK(cam[0x240] == 1 && mask == mask0, "camera given back: flag %u, mask untouched 0x%08x", cam[0x240], mask);

            // No setter: the getter's flag check gives the flag.
            {
                std::lock_guard<std::mutex> lock(icalls::g_mutex);
                auto& c = icalls::g_candidates;
                c.erase(std::remove_if(c.begin(), c.end(), [](const icalls::Candidate& x) {
                    return x.name == "UnityEngine.Camera::set_worldToCameraMatrix_Injected"; }), c.end());
            }
            cfg::thirdperson::enabled = true;
            thirdperson::g_pid = -1;
            thirdperson::tick(&reading, out);
            thirdperson::wait();
            CHECK(thirdperson::g_flag_offset.load() == 0x240 && thirdperson::g_matrix_offset.load() == 0x70,
                  "getter traced: flag +0x%x matrix +0x%x", thirdperson::g_flag_offset.load(), thirdperson::g_matrix_offset.load());
            // The game keeps recomputing the matrix: third person must notice and give the camera back.
            CHECK(thirdperson::tick(&reading, out), "taken again");
            for (int i = 0; i < 5; ++i) {
                std::memcpy(cam + 0x70, &live, sizeof(live));
                thirdperson::tick(&reading, out);
            }
            thirdperson::g_taken_at -= 1000;
            thirdperson::tick(&reading, out);
            CHECK(thirdperson::g_failed && cam[0x240] == 1, "overwritten matrix detected (failed %d, flag %u)", thirdperson::g_failed, cam[0x240]);
            cfg::thirdperson::enabled = false;
        }

        // Third person look and anti-aim on one local player: the body renderer is switched on with
        // the game's set_enabled (strict replay), the second camera loses the first-person layer, and
        // the model root transform turns.
        {
            route(v100::renderer_get_enabled_injected, base + fx::UnityEngine_Renderer__get_enabled_Injected);
            route(v100::renderer_set_enabled_injected, base + fx::UnityEngine_Renderer__set_enabled_Injected);
            *at(fx::slot_get_enabled) = base + fx::Renderer_get_enabled_Injected;
            *at(fx::slot_set_enabled) = base + fx::Renderer_set_enabled_Injected;
            auto make_renderer = [&](bool enabled, int32_t* ids, int count, uint8_t*& native_out) {
                uint8_t* native = static_cast<uint8_t*>(calloc(1, 0x200));
                *reinterpret_cast<uint64_t*>(native + 0x118) = reinterpret_cast<uint64_t>(ids);  // m_Materials, as chams found it
                *reinterpret_cast<uint64_t*>(native + 0x128) = uint64_t(count);
                *reinterpret_cast<uint64_t*>(native) = reinterpret_cast<uint64_t>(native) + 0x1F0;  // a vtable pointer
                native[0x109] = uint8_t(0x05 | (enabled ? 0x10 : 0));  // other bits set: the flag is a bit
                uint8_t* managed = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(native);
                native_out = native;
                return reinterpret_cast<uint64_t>(managed);
            };
            uint8_t *body = nullptr, *arms = nullptr;
            uint8_t* bindings = static_cast<uint8_t*>(calloc(1, 0x80));
            int32_t body_ids[2] = {-8000, -8001}, arms_ids[1] = {-8100};
            *reinterpret_cast<uint64_t*>(bindings + v100::bindings_character_renderer) = make_renderer(false, body_ids, 2, body);
            *reinterpret_cast<uint64_t*>(bindings + v100::bindings_arms_renderer) = make_renderer(true, arms_ids, 1, arms);

            // Main and first-person cameras (same vtable) in one heap block, the search's only map.
            uint8_t* heap = static_cast<uint8_t*>(aligned_alloc(16, 0x1000));
            std::memset(heap, 0, 0x1000);
            const matrix view0 = player::view_from_pose(Vector3(0.f, 1.f, 0.f), Vector4(0.f, 0.f, 0.f, 1.f));
            const matrix proj0 = player::perspective(60.f, 16.f / 9.f);
            auto make_camera = [&](size_t off, uint32_t mask) {
                uint8_t* c = heap + off;
                *reinterpret_cast<uint64_t*>(c) = proc::lib + v100::data_relro_begin + 0x10;
                *reinterpret_cast<uint64_t*>(c + 0x20) = reinterpret_cast<uint64_t>(heap + 0xF00);  // a game object
                std::memcpy(c + v100::camera_view, &view0, sizeof(view0));
                std::memcpy(c + v100::camera_projection, &proj0, sizeof(proj0));
                std::memcpy(c + 0x220, &mask, 4);
                return c;
            };
            uint8_t* main_cam = make_camera(0x000, 0xFFFFFEFFu);
            uint8_t* view_cam = make_camera(0x400, 0x00000100u);
            uint8_t* ui_cam = make_camera(0x800, 0x00000020u);
            thirdperson::g_mask_offset = 0x220;
            g_scan_maps = {remote_map{reinterpret_cast<uint64_t>(heap), reinterpret_cast<uint64_t>(heap) + 0x1000, true}};

            auto* aa_entries = static_cast<player::TransformEntry*>(calloc(8, sizeof(player::TransformEntry)));
            auto* aa_parents = static_cast<int*>(calloc(8, sizeof(int)));
            for (int i = 0; i < 8; ++i) {
                aa_entries[i].position = Vector4(.1f * i, 0.f, 0.f, 0.f);
                aa_entries[i].rotation = Vector4(0.f, 0.f, 0.f, 1.f);
                aa_entries[i].scale = Vector4(1.f, 1.f, 1.f, 0.f);
                aa_parents[i] = -1;
            }
            uint8_t* data = static_cast<uint8_t*>(calloc(1, 0x40));
            *reinterpret_cast<uint64_t*>(data + 0x18) = reinterpret_cast<uint64_t>(aa_entries);
            *reinterpret_cast<uint64_t*>(data + 0x20) = reinterpret_cast<uint64_t>(aa_parents);
            player::g_transform_layout = {0x38, 0x40, 0x18, 0x20, proc::pid};
            uint8_t* tnative = static_cast<uint8_t*>(calloc(1, 0x80));
            *reinterpret_cast<uint64_t*>(tnative + 0x38) = reinterpret_cast<uint64_t>(data);
            const int model_index = 1, wrong = 7;  // the layout's +0x40 points elsewhere
            std::memcpy(tnative + 0x30, &model_index, 4);
            std::memcpy(tnative + 0x40, &wrong, 4);
            uint8_t* tmanaged = static_cast<uint8_t*>(calloc(1, 0x40));
            *reinterpret_cast<uint64_t*>(tmanaged + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(tnative);

            uint8_t* model = static_cast<uint8_t*>(calloc(1, 0x80));
            *reinterpret_cast<uint64_t*>(model) = rpm<uint64_t>(proc::lib + v100::model_typeinfo);
            *reinterpret_cast<uint64_t*>(model + v100::model_bindings) = reinterpret_cast<uint64_t>(bindings);
            *reinterpret_cast<uint64_t*>(model + v100::model_transform) = reinterpret_cast<uint64_t>(tmanaged);
            uint8_t* array = static_cast<uint8_t*>(calloc(1, 0x30));
            *reinterpret_cast<uint64_t*>(array + v100::array_length) = 1;
            *reinterpret_cast<uint64_t*>(array + v100::array_data) = reinterpret_cast<uint64_t>(model);
            uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
            *reinterpret_cast<uint64_t*>(wmz + v100::wmz_first_array) = reinterpret_cast<uint64_t>(array);
            uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x200));
            *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
            const uint64_t me = reinterpret_cast<uint64_t>(net);
            auto enabled_bit = [](const uint8_t* r) { return (r[0x109] >> 4) & 1; };
            auto mask_of = [](const uint8_t* c) { uint32_t m; std::memcpy(&m, c + 0x220, 4); return m; };
            const int* scene_count = reinterpret_cast<const int*>(base + fx::g_scene_count);

            cfg::thirdperson::show_body = true;
            cfg::thirdperson::try_enable_body = true;
            cfg::thirdperson::flat_body = true;
            cfg::chams::enabled = false;
            localview::g_pid = -1;
            localview::tick(me, true, reinterpret_cast<uint64_t>(main_cam));
            localview::wait();
            CHECK(localview::g_enabled_getter && localview::g_enabled_setter, "Renderer.enabled accessors found");
            CHECK(enabled_bit(body) == 1 && (body[0x109] & 0x05) == 0x05 && *scene_count == 1,
                  "body switched on through set_enabled (bit %d, scene %d)", enabled_bit(body), *scene_count);
            CHECK(localview::g_view_cameras.size() == 1 && localview::g_view_cameras[0].native == reinterpret_cast<uint64_t>(view_cam),
                  "first-person camera found: %zu", localview::g_view_cameras.size());
            CHECK(mask_of(view_cam) == 0 && mask_of(ui_cam) == 0x20 && mask_of(main_cam) == 0xFFFFFEFFu,
                  "layer 8 off the first-person camera only (0x%08x 0x%08x)", mask_of(view_cam), mask_of(ui_cam));
            CHECK(body_ids[0] == 0 && body_ids[1] == 0 && arms_ids[0] == -8100, "body flat, arms untouched (%d %d)", body_ids[0], arms_ids[0]);
            localview::g_last_pass = 0;
            localview::tick(me, false);
            localview::wait();
            CHECK(enabled_bit(body) == 0 && enabled_bit(arms) == 1, "body switched off again (%d)", enabled_bit(body));
            CHECK(mask_of(view_cam) == 0x100, "first-person camera mask restored (0x%08x)", mask_of(view_cam));
            CHECK(body_ids[0] == -8000 && body_ids[1] == -8001, "body materials restored (%d %d)", body_ids[0], body_ids[1]);

            // Flag fallback: the byte and bit behind get_enabled, found from the getter alone.
            {
                uint64_t addr = 0;
                int bit = -1;
                CHECK(localview::find_flag(reinterpret_cast<uint64_t>(body), addr, bit) && addr == reinterpret_cast<uint64_t>(body) + 0x109 && bit == 4,
                      "enabled flag found: +0x%llx bit %d", (unsigned long long)(addr - reinterpret_cast<uint64_t>(body)), bit);
            }

            // Chams layer pass: the scene keeps its own layer per renderer (indexed by renderer+0x198). The
            // layout is found from renderers with known layers, then enemies move to layer 30 in the scene.
            for (int shape = 0; shape < 2; ++shape) {  // 0: array of nodes with a layer mask, 1: flat byte array
                const size_t kBlock = 0x20000;
                uint8_t* scene = static_cast<uint8_t*>(aligned_alloc(16, kBlock));
                uint32_t noise = 12345u + uint32_t(shape);
                for (size_t i = 0; i < kBlock; ++i) { noise = noise * 1664525u + 1013904223u; scene[i] = uint8_t(noise >> 24); }
                const int kStride = 96, kPtr = 8, kField = 0x30;  // node: pointer at +8, layer mask at +0x30
                auto make_ref = [&](int index, int layer) {
                    uint8_t* r = static_cast<uint8_t*>(calloc(1, 0x300));
                    *reinterpret_cast<uint64_t*>(r) = reinterpret_cast<uint64_t>(r) + 0x2F0;
                    uint8_t* go = static_cast<uint8_t*>(calloc(1, 0x100));
                    *reinterpret_cast<uint32_t*>(go + 0x40) = uint32_t(layer);
                    *reinterpret_cast<uint64_t*>(r + 0x20) = reinterpret_cast<uint64_t>(go);
                    *reinterpret_cast<int32_t*>(r + 0x198) = index;
                    if (shape == 0) {
                        uint8_t* node = scene + 0x400 + size_t(index) * kStride;
                        *reinterpret_cast<uint64_t*>(node + kPtr) = reinterpret_cast<uint64_t>(r);
                        *reinterpret_cast<uint32_t*>(node + kField) = 1u << layer;
                    } else {
                        scene[0x400 + index] = uint8_t(layer);
                    }
                    return r;
                };
                std::vector<uint64_t> enemies_r, mates_r, arms_r;
                std::vector<uint8_t*> enemy_ptrs;
                const int enemy_index[] = {7, 19, 23, 31}, mate_index[] = {11, 13, 29}, arm_index[] = {41, 42};
                for (int idx : enemy_index) {
                    uint8_t* r = make_ref(idx, 0);
                    // Some enemies carry no scene index (as on device): the node array is searched for their pointer.
                    if (shape == 0 && (idx == 19 || idx == 23)) *reinterpret_cast<int32_t*>(r + 0x198) = -1;
                    enemy_ptrs.push_back(r);
                    enemies_r.push_back(reinterpret_cast<uint64_t>(r));
                }
                for (int idx : mate_index) mates_r.push_back(reinterpret_cast<uint64_t>(make_ref(idx, 0)));
                for (int idx : arm_index) arms_r.push_back(reinterpret_cast<uint64_t>(make_ref(idx, 8)));
                if (shape == 1)  // a flat array needs more renderers to be unambiguous: add layer-8 parts
                    for (int idx = 50; idx < 60; ++idx) arms_r.push_back(reinterpret_cast<uint64_t>(make_ref(idx, 8)));
                std::vector<scenenode::Ref> refs;
                for (uint64_t r : arms_r) refs.push_back({r, scenenode::index_of(r), 8});
                for (uint64_t r : enemies_r) refs.push_back({r, scenenode::index_of(r), 0});
                for (uint64_t r : mates_r) refs.push_back({r, scenenode::index_of(r), 0});
                g_scan_maps = {remote_map{reinterpret_cast<uint64_t>(scene), reinterpret_cast<uint64_t>(scene) + kBlock, true}};
                auto node_value = [&](int idx) -> uint32_t {
                    if (shape == 0) return *reinterpret_cast<uint32_t*>(scene + 0x400 + size_t(idx) * kStride + kField);
                    return scene[0x400 + idx];
                };
                scenenode::Layout found;
                const bool calibrated = scenenode::calibrate(refs, found);
                CHECK(calibrated && found.ok && found.soa == (shape == 1) && found.mask == (shape == 0),
                      "scene layout found (shape %d): ok %d soa %d mask %d width %d stride %d rel %d", shape, found.ok,
                      found.soa, found.mask, found.width, found.stride, found.rel);
                localview::g_layout = found;
                localview::g_anchor_refs.assign(refs.begin(), refs.begin() + 2);
                cfg::chams::enabled = true; cfg::chams::layer_pass = true;
                localview::layer_pass_tick(reinterpret_cast<uint64_t>(main_cam), enemies_r, {}, 0, 0);
                const uint32_t moved = shape == 0 ? (1u << 30) : 30u;
                const uint32_t original = shape == 0 ? 1u : 0u;
                CHECK(node_value(7) == moved && node_value(19) == moved && node_value(23) == moved && node_value(31) == moved,
                      "enemy nodes moved to layer 30 (%u %u)", node_value(7), node_value(31));
                CHECK(node_value(11) == original && node_value(41) == (shape == 0 ? 0x100u : 8u), "teammates and arms untouched (%u %u)", node_value(11), node_value(41));
                CHECK(mask_of(main_cam) == (0xFFFFFEFFu & ~(1u << 30)) && mask_of(view_cam) == (0x100u | (1u << 30)),
                      "masks: main 0x%08x, first-person 0x%08x", mask_of(main_cam), mask_of(view_cam));
                localview::layer_pass_tick(reinterpret_cast<uint64_t>(main_cam), {}, {}, 0, 0);   // enemies gone: nodes put back
                CHECK(node_value(7) == original && mask_of(main_cam) == 0xFFFFFEFFu, "nodes put back when the list is empty (%u)", node_value(7));
                localview::layer_pass_tick(reinterpret_cast<uint64_t>(main_cam), enemies_r, {}, 0, 0);
                cfg::chams::layer_pass = false;
                localview::layer_pass_tick(reinterpret_cast<uint64_t>(main_cam), enemies_r, {}, 0, 0);
                CHECK(node_value(19) == original && node_value(31) == original && mask_of(main_cam) == 0xFFFFFEFFu && mask_of(view_cam) == 0x100u,
                      "layer pass off restores nodes and masks (%u, 0x%08x 0x%08x)", node_value(19), mask_of(main_cam), mask_of(view_cam));
                cfg::chams::enabled = false;
                if (shape == 0) {
                    // Real body: the arms' node is handed to the body renderer and back.
                    const int arms_idx = 60;
                    uint8_t* arms_node = scene + 0x400 + size_t(arms_idx) * kStride;
                    *reinterpret_cast<uint64_t*>(arms_node + kPtr) = reinterpret_cast<uint64_t>(arms);
                    *reinterpret_cast<uint32_t*>(arms_node + kField) = 0x100u;
                    *reinterpret_cast<int32_t*>(arms + 0x198) = arms_idx;
                    *reinterpret_cast<int32_t*>(body + 0x198) = -1;
                    localview::g_layout = found;
                    localview::g_anchor_refs.clear();
                    localview::g_transplant = {};
                    localview::g_transplant_failed = false;
                    localview::transplant_tick(true, me, {}, 1);
                    CHECK(localview::g_transplant.active && *reinterpret_cast<uint64_t*>(arms_node + kPtr) == reinterpret_cast<uint64_t>(body),
                          "arms node holds the body renderer");
                    CHECK(*reinterpret_cast<int32_t*>(body + 0x198) == arms_idx && *reinterpret_cast<int32_t*>(arms + 0x198) == -1,
                          "scene indices swapped (%d %d)", *reinterpret_cast<int32_t*>(body + 0x198), *reinterpret_cast<int32_t*>(arms + 0x198));
                    CHECK(*reinterpret_cast<uint32_t*>(arms_node + kField) == 1u && enabled_bit(body) == 1,
                          "node layer 0, body enabled (%u, %d)", *reinterpret_cast<uint32_t*>(arms_node + kField), enabled_bit(body));
                    localview::transplant_tick(true, me, {}, 1);  // nothing to redo
                    localview::transplant_tick(false, me, {}, 1);
                    CHECK(!localview::g_transplant.active && *reinterpret_cast<uint64_t*>(arms_node + kPtr) == reinterpret_cast<uint64_t>(arms) &&
                          *reinterpret_cast<int32_t*>(arms + 0x198) == arms_idx && *reinterpret_cast<int32_t*>(body + 0x198) == -1 &&
                          *reinterpret_cast<uint32_t*>(arms_node + kField) == 0x100u && enabled_bit(body) == 0,
                          "hand-over undone (node 0x%llx, arms %d, body %d, layer 0x%x, enabled %d)",
                          (unsigned long long)*reinterpret_cast<uint64_t*>(arms_node + kPtr), *reinterpret_cast<int32_t*>(arms + 0x198),
                          *reinterpret_cast<int32_t*>(body + 0x198), *reinterpret_cast<uint32_t*>(arms_node + kField), enabled_bit(body));
                }
                free(scene);
            }
            g_scan_maps = {remote_map{reinterpret_cast<uint64_t>(heap), reinterpret_cast<uint64_t>(heap) + 0x1000, true}};

            // Chams through walls: the outline effect's mask materials, found by the rdj class pointer.
            uint8_t* rdj_class = static_cast<uint8_t*>(calloc(1, 0x300));
            char* rdj_name = static_cast<char*>(calloc(1, 16));
            std::strcpy(rdj_name, "rdj");
            *reinterpret_cast<uint64_t*>(rdj_class + klass::g_name_offset) = reinterpret_cast<uint64_t>(rdj_name);
            *reinterpret_cast<uint64_t*>(unity + 0x700) = reinterpret_cast<uint64_t>(rdj_class);  // its TypeInfo global
            uint8_t* rdj = heap + 0xC00;
            *reinterpret_cast<uint64_t*>(rdj) = reinterpret_cast<uint64_t>(rdj_class);
            for (int k = 0; k < 2; ++k) {
                uint8_t* mat = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(mat) = reinterpret_cast<uint64_t>(mat) + 0x20;
                const int32_t id = -12000 - k;
                std::memcpy(mat + v100::native_instance_id, &id, 4);
                uint8_t* managed_mat = static_cast<uint8_t*>(calloc(1, 0x40));
                *reinterpret_cast<uint64_t*>(managed_mat + v100::unity_cached_ptr) = reinterpret_cast<uint64_t>(mat);
                *reinterpret_cast<uint64_t*>(rdj + 0x38 + k * 8) = reinterpret_cast<uint64_t>(managed_mat);
            }
            chams::resolve_outline_masks();
            CHECK(chams::g_mask_ids[0].load() == -12000 && chams::g_mask_ids[1].load() == -12001,
                  "outline mask materials %d %d", chams::g_mask_ids[0].load(), chams::g_mask_ids[1].load());
            cfg::chams::through_walls = true;
            cfg::chams::wall_material = 1;
            CHECK(chams::desired_id(reinterpret_cast<uint64_t>(bindings), true) == -12001, "wall material choice");
            cfg::chams::wall_material = 0;
            CHECK(chams::desired_id(reinterpret_cast<uint64_t>(bindings), true) == -12000, "wall material default");

            // Occlusion culling off on the main camera while through walls is on, back after.
            route(v100::camera_set_occlusion_injected, base + fx::UnityEngine_Camera__set_useOcclusionCulling_Injected);
            *at(fx::slot_set_occlusion) = base + fx::Camera_set_useOcclusionCulling_Injected;
            main_cam[0x250] = 1;
            cfg::chams::enabled = true; cfg::chams::no_occlusion = true;
            chams::occlusion_tick(reinterpret_cast<uint64_t>(main_cam));  // resolves
            chams::wait();
            CHECK(chams::g_occlusion_offset.load() == 0x250, "useOcclusionCulling +0x%x", chams::g_occlusion_offset.load());
            chams::occlusion_tick(reinterpret_cast<uint64_t>(main_cam));
            CHECK(main_cam[0x250] == 0, "occlusion culling off");
            cfg::chams::through_walls = false;
            chams::occlusion_tick(reinterpret_cast<uint64_t>(main_cam));
            CHECK(main_cam[0x250] == 1, "occlusion culling back on");
            cfg::chams::enabled = false;

            // renderer+0x134: patched with the array only where it holds slot 0's id.
            {
                int32_t ids_a[1] = {-9100}, ids_b[1] = {-9200};
                uint8_t* ra = static_cast<uint8_t*>(calloc(1, 0x200));
                uint8_t* rb = static_cast<uint8_t*>(calloc(1, 0x200));
                for (uint8_t* r : {ra, rb}) *reinterpret_cast<uint64_t*>(r + chams::g_data_offset.load() + 0x10) = 1;
                *reinterpret_cast<uint64_t*>(ra + chams::g_data_offset.load()) = reinterpret_cast<uint64_t>(ids_a);
                *reinterpret_cast<uint64_t*>(rb + chams::g_data_offset.load()) = reinterpret_cast<uint64_t>(ids_b);
                *reinterpret_cast<int32_t*>(ra + 0x134) = -9100;  // a copy of the id
                *reinterpret_cast<int32_t*>(rb + 0x134) = 77;     // something else
                chams::patch(reinterpret_cast<uint64_t>(ra), -424);
                chams::patch(reinterpret_cast<uint64_t>(rb), -424);
                CHECK(ids_a[0] == -424 && *reinterpret_cast<int32_t*>(ra + 0x134) == -424, "+0x134 copy patched");
                CHECK(ids_b[0] == -424 && *reinterpret_cast<int32_t*>(rb + 0x134) == 77, "+0x134 left alone when it is not the id");
                chams::restore_all();
                CHECK(ids_a[0] == -9100 && *reinterpret_cast<int32_t*>(ra + 0x134) == -9100 && *reinterpret_cast<int32_t*>(rb + 0x134) == 77,
                      "+0x134 restored");
            }
            cfg::chams::through_walls = false;

            // Anti-aim turns only the model root.
            skeleton::g_index_pid = -1;
            cfg::antiaim::enabled = true; cfg::antiaim::yaw_mode = 3; cfg::antiaim::pitch_mode = 0;
            antiaim::g_pid = -1;
            const Vector3 feet(0.f, 0.f, 0.f);
            antiaim::tick(me, feet);
            CHECK(antiaim::g_model.index_offset == 0x30, "model index offset +0x%x", antiaim::g_model.index_offset);
            auto yaw180 = [](const Vector4& q) { return std::fabs(q.y) > .999f && std::fabs(q.w) < 1e-3f; };
            CHECK(yaw180(aa_entries[1].rotation), "backwards written");
            CHECK(aa_entries[0].rotation.w == 1.f && aa_entries[2].rotation.w == 1.f, "other entries untouched");
            antiaim::tick(me, feet);  // the game left the model alone: no compounding
            CHECK(yaw180(aa_entries[1].rotation) && antiaim::g_model.held == 1, "held without compounding");
            aa_entries[1].rotation = Vector4(0.f, 0.f, 0.f, 1.f);  // the game re-posed the model
            antiaim::tick(me, feet);
            CHECK(yaw180(aa_entries[1].rotation) && antiaim::g_model.rewritten == 1, "re-applied on top of the game's pose");
            antiaim::tick(me, feet);
            cfg::antiaim::enabled = false;
            antiaim::tick(me, feet);
            CHECK(aa_entries[1].rotation.w == 1.f, "model restored on disable (%.3f)", aa_entries[1].rotation.w);

            // Skin changer: the weapon content resolver gives the knife the Butterfly template's mesh,
            // animations and sounds; the registry entry of the knife in hands carries the skin id for
            // one load and gets its own back after; all put back on Original / disable.
            {
                auto named_class = [&](const char* name) {
                    uint8_t* k = static_cast<uint8_t*>(calloc(1, 0x300));
                    char* t = static_cast<char*>(calloc(1, 32)); std::strcpy(t, name);
                    *reinterpret_cast<uint64_t*>(k + klass::g_name_offset) = reinterpret_cast<uint64_t>(t);
                    return k;
                };
                uint8_t* beh_class = named_class("beh");
                uint8_t* ybt_class = named_class("ybt");
                uint8_t* dict_class = named_class("Dictionary`2");
                auto make_dict = [&](const std::vector<std::pair<uint64_t, uint64_t>>& kv) {
                    uint8_t* entries = static_cast<uint8_t*>(calloc(1, 0x20 + kv.size() * 0x18));
                    *reinterpret_cast<uint64_t*>(entries + 0x18) = kv.size();
                    for (size_t i = 0; i < kv.size(); ++i) {
                        uint8_t* e = entries + 0x20 + i * 0x18;
                        *reinterpret_cast<int32_t*>(e) = 1;
                        *reinterpret_cast<int32_t*>(e + 4) = -1;
                        *reinterpret_cast<uint64_t*>(e + 8) = kv[i].first;
                        *reinterpret_cast<uint64_t*>(e + 0x10) = kv[i].second;
                    }
                    uint8_t* d = static_cast<uint8_t*>(calloc(1, 0x50));
                    *reinterpret_cast<uint64_t*>(d) = reinterpret_cast<uint64_t>(dict_class);
                    *reinterpret_cast<uint64_t*>(d + 0x18) = reinterpret_cast<uint64_t>(entries);
                    *reinterpret_cast<int32_t*>(d + 0x20) = int32_t(kv.size());
                    return d;
                };
                auto managed = [](const char* text) {
                    const size_t n = std::strlen(text);
                    uint8_t* o = static_cast<uint8_t*>(calloc(1, 0x20 + n * 2));
                    *reinterpret_cast<int32_t*>(o + 0x10) = int32_t(n);
                    for (size_t i = 0; i < n; ++i) *reinterpret_cast<uint16_t*>(o + 0x14 + i * 2) = uint16_t(text[i]);
                    return reinterpret_cast<uint64_t>(o);
                };
                auto value_of = [&](uint8_t* d, size_t i) { return reinterpret_cast<uint64_t*>(*reinterpret_cast<uint64_t*>(d + 0x18) + 0x20 + i * 0x18 + 0x10); };
                // Params per kind k: knife 70 = 0x7000 + k, gun 33 = 0x3300 + k, Butterfly template = 0x7500 + k.
                uint8_t* by_weapon[3];
                const uint64_t butterfly = managed("skinTemplate/KnifeButterfly"), karambit = managed("skinTemplate/KnifeKarambit");
                for (int k = 0; k < 3; ++k) {
                    by_weapon[k] = make_dict({{70, 0x7000u + k}, {33, 0x3300u + k}});
                    uint8_t* by_name = make_dict({{karambit, 0x6600u + k}, {butterfly, 0x7500u + k}});
                    uint8_t* empty = make_dict({});
                    uint8_t* iwf = static_cast<uint8_t*>(calloc(1, 0x80));
                    *reinterpret_cast<uint64_t*>(iwf + 0x38) = reinterpret_cast<uint64_t>(by_weapon[k]);
                    *reinterpret_cast<uint64_t*>(iwf + 0x40) = reinterpret_cast<uint64_t>(by_name);
                    *reinterpret_cast<uint64_t*>(iwf + 0x48) = reinterpret_cast<uint64_t>(empty);
                    static uint8_t* iwfs[3];
                    iwfs[k] = iwf;
                    if (k == 2) {
                        uint8_t* knife_content = static_cast<uint8_t*>(calloc(1, 0x40));
                        for (int j = 0; j < 3; ++j) *reinterpret_cast<uint64_t*>(knife_content + 0x10 + j * 8) = 0x7000u + j;
                        uint8_t* other_content = static_cast<uint8_t*>(calloc(1, 0x40));  // someone's skin 99999
                        for (int j = 0; j < 3; ++j) *reinterpret_cast<uint64_t*>(other_content + 0x10 + j * 8) = 0x9900u + j;
                        uint8_t* butterfly_content = static_cast<uint8_t*>(calloc(1, 0x40));  // built for WeaponId 72 with the Butterfly template's params
                        *reinterpret_cast<uint64_t*>(butterfly_content + 0x10) = 0x7500u;
                        uint8_t* cache = make_dict({{70, reinterpret_cast<uint64_t>(knife_content)}, {44 | (uint64_t(99999) << 32), reinterpret_cast<uint64_t>(other_content)},
                                                    {75, reinterpret_cast<uint64_t>(butterfly_content)}});
                        uint8_t* ybt = static_cast<uint8_t*>(calloc(1, 0x60));
                        *reinterpret_cast<uint64_t*>(ybt) = reinterpret_cast<uint64_t>(ybt_class);
                        *reinterpret_cast<uint64_t*>(ybt + 0x38) = reinterpret_cast<uint64_t>(iwfs[0]);
                        *reinterpret_cast<uint64_t*>(ybt + 0x28) = reinterpret_cast<uint64_t>(iwfs[1]);
                        *reinterpret_cast<uint64_t*>(ybt + 0x30) = reinterpret_cast<uint64_t>(iwfs[2]);
                        *reinterpret_cast<uint64_t*>(ybt + 0x40) = reinterpret_cast<uint64_t>(cache);
                        // Registry: entry 0 blank (meta id 0), entry 1 someone's skin.
                        uint8_t* items = static_cast<uint8_t*>(calloc(1, 0x20 + 4 * 0x38));
                        *reinterpret_cast<uint64_t*>(items + 0x18) = 4;
                        *reinterpret_cast<int32_t*>(items + 0x20 + 0x38 + 4) = 240095;
                        uint8_t* list = static_cast<uint8_t*>(calloc(1, 0x20));
                        *reinterpret_cast<uint64_t*>(list + 0x10) = reinterpret_cast<uint64_t>(items);
                        *reinterpret_cast<int32_t*>(list + 0x18) = 2;
                        uint8_t* yvh = static_cast<uint8_t*>(calloc(1, 0x20));
                        *reinterpret_cast<uint64_t*>(yvh + 0x10) = reinterpret_cast<uint64_t>(list);
                        uint8_t* beh = static_cast<uint8_t*>(calloc(1, 0xC0));
                        *reinterpret_cast<uint64_t*>(beh) = reinterpret_cast<uint64_t>(beh_class);
                        beh[0x40] = 70;
                        *reinterpret_cast<uint64_t*>(beh + 0x68) = reinterpret_cast<uint64_t>(ybt);
                        *reinterpret_cast<uint64_t*>(beh + 0x80) = reinterpret_cast<uint64_t>(yvh);
                        uint8_t* parts = static_cast<uint8_t*>(calloc(1, 0x30));
                        *reinterpret_cast<uint64_t*>(parts + 0x18) = 1;
                        *reinterpret_cast<uint64_t*>(parts + 0x20) = reinterpret_cast<uint64_t>(beh);
                        uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x50));
                        *reinterpret_cast<uint64_t*>(wmz + 0x18) = reinterpret_cast<uint64_t>(parts);
                        uint8_t* player = static_cast<uint8_t*>(calloc(1, 0x100));
                        *reinterpret_cast<uint64_t*>(player + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
                        const uint64_t local_s = reinterpret_cast<uint64_t>(player);
                        auto field = [](uint8_t* c, int j) { return *reinterpret_cast<uint64_t*>(c + 0x10 + j * 8); };
                        auto beh_meta = [&] { return *reinterpret_cast<uint16_t*>(beh + 0x42); };
                        auto entry0 = [&] { return *reinterpret_cast<int32_t*>(items + 0x20 + 4); };
                        auto game_loads = [&] {  // the game loads the weapon in hands
                            *reinterpret_cast<uint16_t*>(beh + 0x42) = 0;
                            *reinterpret_cast<int32_t*>(beh + 0x48) += 1;
                            skins::g_reload_at = 0;
                            skins::tick(local_s, {local_s});
                        };

                        skins::g_pid = -1;
                        cfg::skins::enabled = true;
                        cfg::skins::force_model = false;
                        cfg::skins::chosen[70] = 47502;
                        skins::tick(local_s, {local_s});
                        CHECK(skins::g_params[0].by_weapon == reinterpret_cast<uint64_t>(by_weapon[0]) && skins::g_params[2].by_name, "tables found");
                        CHECK(*value_of(by_weapon[0], 0) == 0x7000 && field(knife_content, 0) == 0x7000 && entry0() == 47502 && beh_meta() == 0xFFFE,
                              "knife's skin id in its entry, knife loads, tables untouched (entry %d)", entry0());
                        const skins::View v = skins::view();
                        CHECK(v.weapon == 70 && v.names.size() == 16 && v.current == 1, "menu: weapon %d current %d", v.weapon, v.current);
                        game_loads();
                        CHECK(entry0() == 47502 && beh_meta() == 0, "the skin id stays while the knife is in hands");
                        *reinterpret_cast<int32_t*>(beh + 0x48) += 1;  // the game loads the knife again itself
                        skins::tick(local_s, {local_s});
                        CHECK(entry0() == 47502 && beh_meta() == 0, "the game's own loads see it, nothing to do");
                        cfg::skins::chosen[70] = 240007;
                        skins::tick(local_s, {local_s});
                        CHECK(entry0() == 240007 && beh_meta() == 0xFFFE, "another Butterfly skin loads");
                        game_loads();
                        beh[0x40] = 33;  // a gun in hands
                        skins::tick(local_s, {local_s});
                        CHECK(entry0() == 0 && beh_meta() == 0xFFFE, "gun in hands: entry back, the gun loads clean");
                        *reinterpret_cast<uint16_t*>(beh + 0x42) = 0;
                        beh[0x40] = 70;  // the knife again
                        game_loads();
                        CHECK(entry0() == 240007 && beh_meta() == 0xFFFE, "knife again: skin id back, knife loads");
                        game_loads();
                        // The earlier way (no menu entry): the knife's own entries get the template's mesh and sounds.
                        cfg::skins::force_model = true;
                        cfg::skins::chosen[70] = 47502;
                        skins::tick(local_s, {local_s});
                        CHECK(*value_of(by_weapon[0], 0) == 0x7500 && *value_of(by_weapon[2], 0) == 0x7502 && *value_of(by_weapon[1], 0) == 0x7001 &&
                              *value_of(by_weapon[0], 1) == 0x3300 && field(knife_content, 0) == 0x7500 && field(knife_content, 1) == 0x7001 &&
                              entry0() == 47502, "force model: Butterfly template mesh and sounds");
                        game_loads();
                        cfg::skins::chosen[70] = 12345;   // unknown: nothing, choice dropped, knife back
                        skins::tick(local_s, {local_s});
                        CHECK(cfg::skins::chosen[70] == 0 && *value_of(by_weapon[0], 0) == 0x7000 && field(knife_content, 0) == 0x7000 &&
                              beh_meta() == 0xFFFE && entry0() == 0, "unknown skin: knife back to its own");
                        game_loads();
                        cfg::skins::chosen[70] = 99999;   // not in the tables: from another player's built content
                        skins::tick(local_s, {local_s});
                        CHECK(*value_of(by_weapon[0], 0) == 0x9900 && field(knife_content, 2) == 0x9902 && entry0() == 99999,
                              "skin from another player's content");
                        game_loads();
                        cfg::skins::force_model = false;
                        skins::tick(local_s, {local_s});
                        CHECK(*value_of(by_weapon[0], 0) == 0x7000 && field(knife_content, 2) == 0x7002, "force model off: tables back");
                        game_loads();
                        // Code patch: two instructions of the loader become calls into a cave that answers weapon 75 and
                        // the skin id for the local knife; the registry is left alone.
                        {
                            const uint64_t site1 = proc::lib + skins::kSite1Rva, site2 = proc::lib + skins::kSite2Rva, cave = proc::lib + 0xC000000;
                            *reinterpret_cast<uint32_t*>(site1) = skins::kOriginal1;
                            *reinterpret_cast<uint32_t*>(site2) = skins::kOriginal2;
                            skins::g_cave_override = cave;
                            auto data = [&] { return *reinterpret_cast<skins::CaveData*>(cave + skins::kCaveData); };
                            auto bl_target = [](uint64_t at) { const uint32_t w = *reinterpret_cast<uint32_t*>(at); return (w >> 26) == 0x25 ? at + (int64_t(int32_t(w << 6) >> 6) * 4) : 0; };
                            auto count = [&] { return int(*reinterpret_cast<int32_t*>(list + 0x18)); };
                            const int count_before = count();
                            cfg::skins::code_patch = true;
                            cfg::skins::chosen[70] = 47502;
                            skins::tick(local_s, {local_s});
                            CHECK(bl_target(site1) == cave && bl_target(site2) == cave + skins::kCaveEntryB, "both sites call the cave");
                            CHECK(std::memcmp(reinterpret_cast<void*>(cave), skins::kCave, sizeof(skins::kCave)) == 0, "cave written");
                            CHECK(data().beh == reinterpret_cast<uint64_t>(beh) && data().weapon == 75 && data().skin == 47502 &&
                                  count() == count_before, "knives load as 75 with skin 47502 (data %u/%u, count %d)",
                                  data().weapon, data().skin, count());
                            CHECK(entry0() == 0 && beh_meta() == 0xFFFE, "the blank entry is left alone, the knife reloads (entry0 %d)", entry0());
                            game_loads();
                            beh[0x40] = 75;   // the game now caches the knife as the Butterfly: still the knife for the choices
                            skins::g_reload_at = 0;
                            skins::tick(local_s, {local_s});
                            CHECK(skins::view().weapon == 70 && cfg::skins::chosen[75] == 0 && beh_meta() == 0, "loaded as 75: still the knife for the menu (weapon %d)", skins::view().weapon);
                            cfg::skins::chosen[70] = 240007;   // another skin: new data, and the knife loads again
                            skins::tick(local_s, {local_s});
                            CHECK(data().skin == 240007 && data().weapon == 75 && count() == count_before && beh_meta() == 0xFFFE, "another skin id in the cave");
                            game_loads();
                            skins::probe_request();   // the probe hands out WeaponIds and names the one that builds the Butterfly's params
                            skins::tick(local_s, {local_s});
                            CHECK(skins::g_probe.on && data().weapon == 75 && data().skin == 47502, "probe starts with id 75 (weapon %u)", data().weapon);
                            skins::tick(local_s, {local_s});
                            CHECK(cfg::skins::model == 75 && !skins::g_probe.waiting && skins::g_probe.index == 1001, "probe found the Butterfly as 75 (model %d, index %d)", cfg::skins::model, skins::g_probe.index);
                            skins::g_probe.on = false;
                            cfg::skins::model = 75;
                            cfg::skins::chosen[70] = 0;
                            skins::tick(local_s, {local_s});
                            CHECK(data().weapon == 0 && data().skin == 0 && data().beh == 0 && beh_meta() == 0xFFFE, "no skin: neutral cave data, knife reloads");
                            beh[0x40] = 70;
                            game_loads();
                            cfg::skins::chosen[70] = 47502;
                            skins::tick(local_s, {local_s});
                            game_loads();
                            cfg::skins::code_patch = false;   // switched off: code back, our entry gone
                            skins::tick(local_s, {local_s});
                            CHECK(*reinterpret_cast<uint32_t*>(site1) == skins::kOriginal1 && *reinterpret_cast<uint32_t*>(site2) == skins::kOriginal2 &&
                                  *reinterpret_cast<uint32_t*>(cave) == 0 && count() == count_before, "code patch switched off: both instructions and cave back (count %d)", count());
                            game_loads();
                            *reinterpret_cast<uint32_t*>(site2) = 0x12345678;   // another build: refused, nothing written
                            cfg::skins::code_patch = true;
                            skins::tick(local_s, {local_s});
                            CHECK(!skins::g_patch.site1 && *reinterpret_cast<uint32_t*>(site1) == skins::kOriginal1 && *reinterpret_cast<uint32_t*>(site2) == 0x12345678 &&
                                  *reinterpret_cast<uint32_t*>(cave) == 0, "unknown instruction: not patched");
                            cfg::skins::code_patch = false;
                            *reinterpret_cast<uint32_t*>(site1) = 0;
                            *reinterpret_cast<uint32_t*>(site2) = 0;
                            skins::g_cave_override = 0;
                        }
                        game_loads();
                        cfg::skins::chosen[70] = 47502;
                        skins::tick(local_s, {local_s});
                        cfg::skins::enabled = false;
                        skins::g_reload_at = 0;
                        *reinterpret_cast<uint16_t*>(beh + 0x42) = 0;
                        skins::tick(local_s, {local_s});
                        CHECK(*value_of(by_weapon[0], 0) == 0x7000 && *value_of(by_weapon[2], 0) == 0x7002 && field(knife_content, 0) == 0x7000 &&
                              field(knife_content, 1) == 0x7001 && entry0() == 0, "disabled: all back");
                    }
                }
            }
        }

        // Every slot empty: no trace may invent an address, and the code dump must run.
        for (uint64_t slot : {fx::slot_get_start, fx::slot_set_end, fx::slot_set_start, fx::slot_get_fog, fx::slot_get_mode, fx::slot_get_color,
                              fx::slot_get_object, fx::slot_get_end, fx::slot_get_equator})
            *at(slot) = 0;
        munmap(unity, 0x2000);
        close(fd);
        unlink(so.c_str());
        rmdir(unity_path);
        cfg::fog::enabled = true;
        worldfog::g_pid = -1;
        worldfog::tick();
        worldfog::wait();
        CHECK(!worldfog::g_ready && !worldfog::g_fields[worldfog::Start].address, "nothing resolved without slots");
        CHECK(worldfog::g_dumped, "wrapper code dumped");
        cfg::fog::enabled = false;
    }

    // Remote call: a child whose main thread naps in nanosleep (a restartable system call) under the
    // game's thread name; the call runs on it, and it goes on napping afterwards.
    {
        const pid_t child = fork();
        if (child == 0) {
            prctl(PR_SET_NAME, "UnityMain");
            const timespec nap{0, 2000000};
            for (int i = 0; i < 5000 && g_remote_flag != 2; ++i) nanosleep(&nap, nullptr);
            _exit(g_remote_flag == 2 ? 7 : 1);
        }
        usleep(50000);
        uint64_t ret = 0;
        std::string why;
        const uint64_t fn = reinterpret_cast<uint64_t>(&remote_target), flag = reinterpret_cast<uint64_t>(&g_remote_flag);
        const bool first = remote::call_on(child, "UnityMain", fn, {flag, 1}, ret, why);
        uint64_t seen = 0;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/mem", child);
        const int fd = open(path, O_RDONLY);
        if (fd >= 0) { pread(fd, &seen, sizeof(seen), off_t(flag)); close(fd); }
        CHECK(first && ret == 2 && seen == 1, "remote call ran on the child: %s ret=%llu flag=%llu", first ? "ok" : why.c_str(),
              (unsigned long long)ret, (unsigned long long)seen);
        const bool second = remote::call_on(child, "UnityMain", fn, {flag, 2}, ret, why);
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(second && WIFEXITED(status) && WEXITSTATUS(status) == 7, "child went on after the calls: %s status=0x%x",
              second ? "ok" : why.c_str(), status);
    }

    // Armor, max armor and helmet from the stats object; values that do not fit together are refused.
    {
        const uint64_t stats_class = rpm<uint64_t>(proc::lib + v100::stats_typeinfo);
        uint8_t* stats = static_cast<uint8_t*>(calloc(1, 0x40));
        *reinterpret_cast<uint64_t*>(stats) = stats_class;
        *reinterpret_cast<uint16_t*>(stats + v100::stats_armor) = 75;
        *reinterpret_cast<uint16_t*>(stats + v100::stats_max_armor) = 100;
        stats[v100::stats_helmet] = 1;
        uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x200));
        *reinterpret_cast<uint64_t*>(net + v100::player_stats) = reinterpret_cast<uint64_t>(stats);
        const player::Armor a = player::armor(reinterpret_cast<uint64_t>(net));
        CHECK(a.valid && a.armor == 75 && a.max == 100 && a.helmet, "armor %d/%d helmet %d", a.armor, a.max, a.helmet);
        *reinterpret_cast<uint16_t*>(stats + v100::stats_armor) = 150;
        CHECK(!player::armor(reinterpret_cast<uint64_t>(net)).valid, "armor above max refused");
        stats[v100::stats_helmet] = 7;
        *reinterpret_cast<uint16_t*>(stats + v100::stats_armor) = 0;
        CHECK(!player::armor(reinterpret_cast<uint64_t>(net)).valid, "helmet byte that is no bool refused");
    }

    // Auto strafe: the velocity vector is told from two others by following the real movement, then in the air
    // the vector turns with the view and on the ground it does not.
    {
        uint8_t* tof = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(tof) = reinterpret_cast<uint64_t>(make_class("tof"));
        uint8_t* nip = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(nip) = reinterpret_cast<uint64_t>(make_class("nip"));
        *reinterpret_cast<uint64_t*>(nip + 0x48) = reinterpret_cast<uint64_t>(tof);
        uint8_t* array = static_cast<uint8_t*>(calloc(1, 0x30));
        *reinterpret_cast<uint64_t*>(array + v100::array_length) = 1;
        *reinterpret_cast<uint64_t*>(array + v100::array_data) = reinterpret_cast<uint64_t>(nip);
        uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(wmz + v100::wmz_first_array) = reinterpret_cast<uint64_t>(array);
        uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x200));
        *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
        const uint64_t local = reinterpret_cast<uint64_t>(net);

        float* a_vec = reinterpret_cast<float*>(tof + 0x10);   // not a velocity
        float* b_vec = reinterpret_cast<float*>(tof + 0x1C);   // the velocity
        a_vec[0] = 1.f; a_vec[1] = 2.f; a_vec[2] = 3.f;
        b_vec[0] = 8.f; b_vec[1] = 0.f; b_vec[2] = 0.f;        // 8 m/s east (+X)
        auto view_with_yaw = [](float degrees) {
            matrix m{};
            float* a = reinterpret_cast<float*>(&m);
            a[0] = a[5] = a[10] = a[15] = 1.f;
            a[2] = -std::sin(degrees * 0.01745329f);
            a[10] = -std::cos(degrees * 0.01745329f);
            return m;
        };
        auto angle = [&] { return std::atan2(b_vec[0], b_vec[2]) * 57.29578f; };
        cfg::strafe::enabled = true; cfg::strafe::turn = 4.f; cfg::strafe::gain = 0.f; cfg::strafe::min_speed = 2.f; cfg::strafe::log = false;
        strafe::g_pid = -1;
        Vector3 position(0.f, 0.f, 0.f);
        float yaw = 90.f;
        long long last = diag::now_ms();
        auto step = [&](float rise) {   // the game moves the player along the vector
            usleep(10000);
            const long long now = diag::now_ms();
            const float dt = float(now - last) / 1000.f;
            last = now;
            position.x += b_vec[0] * dt; position.z += b_vec[2] * dt; position.y += rise * dt;
            const matrix view = view_with_yaw(yaw);
            strafe::tick(local, position, &view);
        };
        for (int i = 0; i < 70; ++i) step(0.f);
        CHECK(strafe::g_locked == 1 && std::fabs(strafe::g_scale - 1.0) < 0.15, "velocity vector found: index %d, factor %.2f", strafe::g_locked, strafe::g_scale);
        for (int i = 0; i < 10; ++i) { yaw += 1.f; step(0.f); }
        CHECK(std::fabs(angle() - 90.f) < 0.5f, "on the ground the vector stays (%.2f deg)", angle());
        for (int i = 0; i < 25; ++i) { yaw += 2.f; step(3.f); }
        const float speed = std::sqrt(b_vec[0] * b_vec[0] + b_vec[2] * b_vec[2]);
        CHECK(angle() > 100.f && angle() < yaw + 0.5f, "in the air the vector follows the view: %.1f deg (view %.1f)", angle(), yaw);
        CHECK(std::fabs(speed - 8.f) < 0.4f, "speed kept (%.2f)", speed);
        CHECK(strafe::g_writes > 3 && strafe::g_kept > 0, "writes %d, kept %d", strafe::g_writes, strafe::g_kept);
        cfg::strafe::enabled = false;
        strafe::tick(0, Vector3{}, nullptr);
    }

    // Bunnyhop: with a finger on the jump button the player hops again on every landing; without it, never.
    {
        uint8_t* tof = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(tof) = reinterpret_cast<uint64_t>(make_class("tof"));
        uint8_t* nip = static_cast<uint8_t*>(calloc(1, 0x80));
        *reinterpret_cast<uint64_t*>(nip) = reinterpret_cast<uint64_t>(make_class("nip"));
        *reinterpret_cast<uint64_t*>(nip + 0x48) = reinterpret_cast<uint64_t>(tof);
        uint8_t* array = static_cast<uint8_t*>(calloc(1, 0x30));
        *reinterpret_cast<uint64_t*>(array + v100::array_length) = 1;
        *reinterpret_cast<uint64_t*>(array + v100::array_data) = reinterpret_cast<uint64_t>(nip);
        uint8_t* wmz = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(wmz + v100::wmz_first_array) = reinterpret_cast<uint64_t>(array);
        uint8_t* net = static_cast<uint8_t*>(calloc(1, 0x200));
        *reinterpret_cast<uint64_t*>(net + v100::net_systems) = reinterpret_cast<uint64_t>(wmz);
        const uint64_t local = reinterpret_cast<uint64_t>(net);
        float* a_vec = reinterpret_cast<float*>(tof + 0x10);
        float* b_vec = reinterpret_cast<float*>(tof + 0x1C);   // velocity: x, y (vertical), z
        a_vec[0] = 1.f; a_vec[1] = 2.f; a_vec[2] = 3.f;
        b_vec[0] = 8.f; b_vec[1] = 0.f; b_vec[2] = 0.f;
        cfg::strafe::enabled = false; cfg::strafe::log = false;
        cfg::bhop::enabled = true; cfg::bhop::strafe = true; cfg::bhop::log = false;
        cfg::bhop::jump_x = 100.f; cfg::bhop::jump_y = 100.f; cfg::bhop::radius = 50.f;
        strafe::g_pid = -1; bhop::g_pid = -1;
        Vector3 position(0.f, 0.f, 0.f);
        long long last = diag::now_ms();
        auto step = [&] {   // the game: gravity, the ground at y = 0
            usleep(10000);
            const long long now = diag::now_ms();
            const float dt = float(now - last) / 1000.f;
            last = now;
            if (position.y > 0.f || b_vec[1] > 0.f) b_vec[1] -= 20.f * dt;
            position.x += b_vec[0] * dt; position.y += b_vec[1] * dt; position.z += b_vec[2] * dt;
            if (position.y <= 0.f) { position.y = 0.f; if (b_vec[1] < 0.f) b_vec[1] = 0.f; }
            const matrix view = matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            bhop::tick(local, position);
            strafe::tick(local, position, &view);
        };
        // no finger on the button: running on the ground, then your own jump; nothing is added
        touch::g_fake_count = 0;
        for (int i = 0; i < 60; ++i) step();
        b_vec[1] = 5.5f;   // your jump
        for (int i = 0; i < 120; ++i) step();
        CHECK(bhop::g_jumps == 0 && !bhop::g_held, "no finger: no hops (%d)", bhop::g_jumps);
        CHECK(strafe::g_vert == 1, "vertical speed found in vector %d", strafe::g_vert);
        // finger on the button: hops on every landing
        touch::g_fake_count = 1; touch::g_fake[0] = {110.f, 95.f};
        for (int i = 0; i < 260; ++i) step();
        CHECK(bhop::g_held && bhop::g_jumps >= 2, "finger on the jump button: %d hops", bhop::g_jumps);
        // a finger elsewhere does not count
        const int before = bhop::g_jumps;
        touch::g_fake[0] = {700.f, 400.f};
        for (int i = 0; i < 150; ++i) step();
        CHECK(!bhop::g_held && bhop::g_jumps == before, "finger elsewhere: no more hops (%d)", bhop::g_jumps - before);
        // setting the button: the next finger down after all were up
        bhop::start_calibration();
        touch::g_fake_count = 1;            // still down: the finger that pressed the menu
        bhop::calibration_step();
        CHECK(bhop::g_calibrating, "calibration waits for the old finger to come up");
        touch::g_fake_count = 0; bhop::calibration_step();
        touch::g_fake_count = 1; touch::g_fake[0] = {1800.f, 900.f}; bhop::calibration_step();
        CHECK(!bhop::g_calibrating && cfg::bhop::jump_x == 1800.f && cfg::bhop::jump_y == 900.f, "jump button set at (%.0f, %.0f)", cfg::bhop::jump_x, cfg::bhop::jump_y);
        cfg::bhop::enabled = false;
        bhop::tick(0, Vector3{});
        strafe::tick(0, Vector3{}, nullptr);
    }

    std::printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
