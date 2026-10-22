#include "imgui.h"
#include "CGame/CGameContext.hpp"
#include "CMath/CGameMath.hpp"
#include "CGame/CPlayer.hpp"
#include "CMenu/Theme/CTheme.hpp"
#include "CGame/CCamera.hpp"
#include "CGame/CWorld.hpp"
#include "CGame/CSkeleton.hpp"
#include "CGame/CFog.hpp"
#include "CConfigs/CConfig.hpp"
#include <sys/mman.h>
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
    auto* entries = static_cast<player::TransformEntry*>(calloc(n, sizeof(player::TransformEntry)));
    auto* parents = static_cast<int*>(calloc(n, sizeof(int)));
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

    // ---- 9. whole rig: every node below the hips, composed like the reference decode --------------
    {
        skeleton::Pose rig{};
        rig.position[skeleton::Head] = Vector3(0.f, 0.f, 0.f);  // radius test is exercised below
        std::vector<player::TransformEntry> table(entries, entries + n);
        std::vector<int> parent_of(parents, parents + n);
        skeleton::compose_rig(cache, table, parent_of, n, rig);
        CHECK(rig.nodes.empty() || rig.nodes.size() < size_t(n), "radius around a far head keeps few nodes");

        skeleton::Pose full{};
        full.position[skeleton::Head] = pose.position[skeleton::Head];
        skeleton::compose_rig(cache, table, parent_of, n, full);
        CHECK(full.nodes.size() > 5, "rig nodes=%zu", full.nodes.size());
        int linked = 0, matched = 0;
        for (const auto& node : full.nodes) {
            uint8_t* nativ = static_cast<uint8_t*>(calloc(1, 0x100));
            *reinterpret_cast<uint64_t*>(nativ + 0x38) = reinterpret_cast<uint64_t>(hierarchy);
            *reinterpret_cast<int*>(nativ + 0x40) = node.index;
            Vector3 ref{};
            if (player::decode_transform(reinterpret_cast<uint64_t>(nativ), 0x38, 0x40, 0x18, 0x20, ref) &&
                std::fabs(ref.x - node.position.x) + std::fabs(ref.y - node.position.y) + std::fabs(ref.z - node.position.z) < 1e-3f) ++matched;
            if (node.parent >= 0) {
                ++linked;
                CHECK(full.nodes[size_t(node.parent)].index == parents[node.index], "parent link index=%d", node.index);
            }
        }
        CHECK(matched == int(full.nodes.size()), "rig matched %d of %zu", matched, full.nodes.size());
        CHECK(linked >= int(full.nodes.size()) - 1, "linked=%d nodes=%zu", linked, full.nodes.size());
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

    // ---- 8. fog: scan finds the RenderSettings block, tick writes it, disabling restores it -------
    {
        const size_t size = 2 * 1024 * 1024;
        uint8_t* region = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        CHECK(region != MAP_FAILED, "fog mmap");
        auto fill = [&](size_t at, float start, float end, bool tail) {
            worldfog::Block b{};
            b.fog = 0; b.color[0] = .5f; b.color[1] = .5f; b.color[2] = .5f; b.color[3] = 1.f;
            b.mode = 3; b.density = .01f; b.start = start; b.end = end;
            if (tail) for (int c = 0; c < 3; ++c) { b.ambient[c * 4] = .2f + .1f * c; b.ambient[c * 4 + 1] = .3f; b.ambient[c * 4 + 2] = .4f; b.ambient[c * 4 + 3] = 1.f; }
            std::memcpy(region + at, &b, sizeof(b));
        };
        fill(0x1000, 1.f, 50.f, false);       // look-alike: no ambient tail
        fill(0x80010, 0.f, 300.f, true);      // the real one, straddling a 1 MiB chunk edge is covered separately
        fill((1024 * 1024) - 0x20, 2.f, 80.f, false);
        worldfog::forget(); worldfog::restart_scan(); worldfog::g_pid = proc::pid;
        g_scan_maps.clear();
        refresh_scan_maps();
        int steps = 0;
        while (!worldfog::g_address && steps++ < 4000) worldfog::scan_step(8 * 1024 * 1024);
        CHECK(worldfog::g_address == reinterpret_cast<uint64_t>(region + 0x80010), "fog found at %llx want %llx",
              (unsigned long long)worldfog::g_address, (unsigned long long)(reinterpret_cast<uint64_t>(region) + 0x80010));

        cfg::fog::enabled = true; cfg::fog::auto_color = true; cfg::fog::min_distance = 25.f; cfg::fog::max_distance = 90.f;
        worldfog::g_last_write = 0;
        worldfog::tick();
        worldfog::Block live{};
        std::memcpy(&live, region + 0x80010, sizeof(live));
        CHECK(live.fog == 1 && live.mode == 1, "fog on linear");
        CHECK(live.start == 25.f && live.end == 90.f, "fog distances %.1f %.1f", live.start, live.end);
        CHECK(std::fabs(live.color[0] - .3f) < 1e-4f && std::fabs(live.color[2] - .4f) < 1e-4f, "estimated color from ambient equator %.2f %.2f", live.color[0], live.color[2]);
        CHECK(live.ambient[3] == 1.f, "tail untouched");

        cfg::fog::auto_color = false; cfg::fog::color = ImVec4(.9f, .1f, .2f, 1.f);
        worldfog::g_last_write = 0;
        worldfog::tick();
        std::memcpy(&live, region + 0x80010, sizeof(live));
        CHECK(std::fabs(live.color[0] - .9f) < 1e-4f && std::fabs(live.color[1] - .1f) < 1e-4f, "manual color");

        cfg::fog::enabled = false;
        worldfog::tick();
        std::memcpy(&live, region + 0x80010, sizeof(live));
        CHECK(live.fog == 0 && live.mode == 3 && live.start == 0.f && live.end == 300.f && std::fabs(live.color[0] - .5f) < 1e-4f,
              "restored fog=%d mode=%d start=%.1f end=%.1f", live.fog, live.mode, live.start, live.end);
        cfg::fog::enabled = false;
    }

    std::printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
