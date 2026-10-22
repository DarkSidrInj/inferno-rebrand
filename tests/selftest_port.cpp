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
#include "fixtures/rs_accessors.h"
#include "CConfigs/CConfig.hpp"
#include <sys/mman.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

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

    // ---- 10. bone index offset is measured on the hips, not taken from the root probe -----------------
    {
        // Root at (10, 0, 10); hips (index 1) 1 m above it. The native transform keeps a 0 at the
        // probed index offset (+0x40, as on a player root) and the real index at +0x48.
        auto* e = static_cast<player::TransformEntry*>(calloc(8, sizeof(player::TransformEntry)));
        auto* par = static_cast<int*>(calloc(8, sizeof(int)));
        for (int i = 0; i < 8; ++i) { e[i].rotation = Vector4(0, 0, 0, 1); e[i].scale = Vector4(1, 1, 1, 0); par[i] = i == 0 ? -1 : 0; }
        e[0].position = Vector4(10.f, 0.f, 10.f, 0.f);
        e[1].position = Vector4(0.f, 1.f, 0.f, 0.f);
        uint8_t* h = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(h + 0x18) = reinterpret_cast<uint64_t>(e);
        *reinterpret_cast<uint64_t*>(h + 0x20) = reinterpret_cast<uint64_t>(par);
        uint8_t* hips = static_cast<uint8_t*>(calloc(1, 0x100));
        *reinterpret_cast<uint64_t*>(hips + 0x38) = reinterpret_cast<uint64_t>(h);
        *reinterpret_cast<int*>(hips + 0x40) = 0;
        *reinterpret_cast<int*>(hips + 0x48) = 1;
        const player::TransformLayout probed{0x38, 0x40, 0x18, 0x20, proc::pid};
        skeleton::g_index_offset = -1; skeleton::g_index_pid = -1;
        CHECK(skeleton::bone_index_offset(reinterpret_cast<uint64_t>(hips), Vector3(10.f, 0.f, 10.f), probed) == 0x48,
              "measured index offset +0x%x", skeleton::g_index_offset);
        skeleton::g_index_offset = -1; skeleton::g_index_pid = -1;
        CHECK(skeleton::bone_index_offset(reinterpret_cast<uint64_t>(hips), Vector3(50.f, 0.f, 50.f), probed) == -1,
              "no offset when the hips are nowhere near the feet");
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
        for (const auto& r : fx::relocs) {
            uint64_t value = 0;
            if (r.kind == fx::Relative) value = base + r.addend;
            else if (r.kind == fx::Symbol && r.defined) value = base + r.value + r.addend;
            else continue;                                          // unresolved symbol / TLS descriptor: left 0
            *reinterpret_cast<uint64_t*>(base + r.offset) = value;
        }
        auto at = [&](uint64_t off) { return reinterpret_cast<uint64_t*>(base + off); };

        uint8_t* rs = static_cast<uint8_t*>(calloc(1, 0x200));
        auto put_f = [](uint8_t* o, int off, float v) { std::memcpy(o + off, &v, 4); };
        auto init_rs = [&](uint8_t* o) {
            o[0x40] = 0;
            put_f(o, 0x44, .5f); put_f(o, 0x48, .5f); put_f(o, 0x4C, .5f); put_f(o, 0x50, 1.f);
            const int32_t mode = 3; std::memcpy(o + 0x54, &mode, 4);
            put_f(o, 0x58, .01f); put_f(o, 0x5C, 0.f); put_f(o, 0x60, 300.f);
            put_f(o, 0x70, .1f); put_f(o, 0x74, .2f); put_f(o, 0x78, .3f); put_f(o, 0x7C, 1.f);   // ambient sky
            put_f(o, 0x80, .3f); put_f(o, 0x84, .35f); put_f(o, 0x88, .4f); put_f(o, 0x8C, 1.f);  // ambient equator
        };
        init_rs(rs);
        at(fx::g_managers)[7] = reinterpret_cast<uint64_t>(rs);

        // Slots the game has used are filled; the rest stay empty, as in a real process.
        *at(fx::slot_get_start) = base + fx::RenderSettings_get_fogStartDistance;
        *at(fx::slot_set_end) = base + fx::RenderSettings_set_fogEndDistance;       // end getter slot left empty
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
        const uint64_t traps[] = {v100::rs_set_fog, v100::rs_set_fog_start, v100::rs_set_fog_mode, v100::rs_get_fog_density,
                                  v100::rs_set_fog_density, v100::rs_set_fog_color_injected, v100::rs_set_ambient_equator_injected,
                                  v100::rs_get_ambient_sky_injected, v100::rs_set_ambient_sky_injected};
        for (uint64_t rva : traps) route(rva, 0);
        route(v100::rs_get_fog, base + fx::UnityEngine_RenderSettings__get_fog);
        route(v100::rs_get_fog_start, base + fx::UnityEngine_RenderSettings__get_fogStartDistance);
        route(v100::rs_get_fog_end, base + fx::UnityEngine_RenderSettings__get_fogEndDistance);
        route(v100::rs_set_fog_end, base + fx::UnityEngine_RenderSettings__set_fogEndDistance);
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

        cfg::fog::enabled = true; cfg::fog::auto_color = true; cfg::fog::min_distance = 25.f; cfg::fog::max_distance = 90.f;
        worldfog::g_pid = -1;
        worldfog::tick();                       // starts the resolution thread
        worldfog::wait();
        worldfog::tick();                       // applies
        const uint64_t obj = reinterpret_cast<uint64_t>(rs);
        auto how = [](worldfog::Field f) { return worldfog::g_fields[f].how.c_str(); };
        CHECK(worldfog::g_object == obj, "GetRenderSettings traced");
        CHECK(worldfog::g_fields[worldfog::Start].address == obj + 0x5C, "start: %s", how(worldfog::Start));
        CHECK(worldfog::g_fields[worldfog::End].address == obj + 0x60 && worldfog::g_fields[worldfog::End].setter, "end via setter: %s", how(worldfog::End));
        CHECK(worldfog::g_fields[worldfog::Fog].address == obj + 0x40, "fog: %s", how(worldfog::Fog));
        CHECK(worldfog::g_fields[worldfog::Mode].address == obj + 0x54, "mode: %s", how(worldfog::Mode));
        CHECK(worldfog::g_fields[worldfog::FogColor].address == obj + 0x44, "color: %s", how(worldfog::FogColor));
        CHECK(worldfog::g_fields[worldfog::Density].address == obj + 0x58 && worldfog::g_fields[worldfog::Density].how == "inferred from fog start",
              "density inferred: %s", how(worldfog::Density));
        CHECK(worldfog::g_fields[worldfog::Equator].address == obj + 0x80, "equator via icall table: %s", how(worldfog::Equator));
        if (sky_reachable && exec_ok)
            CHECK(worldfog::g_fields[worldfog::Sky].address == obj + 0x70, "sky via registration site: %s", how(worldfog::Sky));
        else std::printf("note: registration call-site case skipped (exec=%d reach=%d)\n", exec_ok, sky_reachable);

        float f = 0.f;
        int32_t mode = 0;
        CHECK(rs[0x40] == 1, "fog on");
        std::memcpy(&mode, rs + 0x54, 4); CHECK(mode == 1, "linear");
        std::memcpy(&f, rs + 0x5C, 4); CHECK(f == 25.f, "start %.1f", f);
        std::memcpy(&f, rs + 0x60, 4); CHECK(f == 90.f, "end %.1f", f);
        std::memcpy(&f, rs + 0x44, 4); CHECK(std::fabs(f - .3f) < 1e-5f, "estimated color r=%.2f", f);
        std::memcpy(&f, rs + 0x58, 4); CHECK(std::fabs(f - .01f) < 1e-6f, "density untouched");

        cfg::fog::auto_color = false; cfg::fog::color = ImVec4(.9f, .1f, .2f, 1.f);
        worldfog::g_last_write = 0;
        worldfog::tick();
        std::memcpy(&f, rs + 0x44, 4); CHECK(std::fabs(f - .9f) < 1e-5f, "manual color r=%.2f", f);

        // Scene load: RenderSettings moves; the start trace notices and everything follows.
        uint8_t* rs2 = static_cast<uint8_t*>(calloc(1, 0x200));
        init_rs(rs2);
        at(fx::g_managers)[7] = reinterpret_cast<uint64_t>(rs2);
        worldfog::g_last_check = 0;
        worldfog::tick();                       // notices the move
        worldfog::tick();                       // resolves again
        worldfog::wait();
        worldfog::g_last_write = 0;
        worldfog::tick();                       // applies
        CHECK(worldfog::g_fields[worldfog::Start].address == reinterpret_cast<uint64_t>(rs2) + 0x5C, "followed the move");
        CHECK(rs2[0x40] == 1, "fog on after the move");

        cfg::fog::enabled = false;
        worldfog::tick();
        std::memcpy(&f, rs2 + 0x60, 4);
        std::memcpy(&mode, rs2 + 0x54, 4);
        CHECK(rs2[0x40] == 0 && mode == 3 && f == 300.f, "restored fog=%d mode=%d end=%.1f", rs2[0x40], mode, f);

        // Occlusion bypass: the culling offset comes from tracing Animator.get_cullingMode_Injected
        // on a real enemy animator; enemies go to AlwaysAnimate and come back when switched off.
        {
            route(v100::animator_get_culling_injected, base + fx::UnityEngine_Animator__get_cullingMode_Injected);
            route(v100::animator_set_culling_injected, 0);
            *at(fx::slot_get_culling) = base + fx::Animator_get_cullingMode_Injected;
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
            animcull::tick(enemies, false);
            std::memcpy(&m1, a1 + 0x58, 4); std::memcpy(&m2, a2 + 0x58, 4);
            CHECK(m1 == 1 && m2 == 2, "culling restored (%d %d)", m1, m2);
        }

        // Every slot empty: no trace may invent an address, and the code dump must run.
        for (uint64_t slot : {fx::slot_get_start, fx::slot_set_end, fx::slot_get_fog, fx::slot_get_mode, fx::slot_get_color,
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

    std::printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
