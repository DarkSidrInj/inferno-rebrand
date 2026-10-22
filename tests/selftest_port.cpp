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
                CHECK(pose.nodes[size_t(node.parent)].index == parents[node.index], "parent link index=%d", node.index);
            }
        }
        CHECK(matched == int(pose.nodes.size()), "rig matched %d of %zu", matched, pose.nodes.size());
        CHECK(linked >= int(pose.nodes.size()) - 1, "linked=%d nodes=%zu", linked, pose.nodes.size());

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

    // ---- 8. fog: field addresses read out of the game's accessor code -------------------------------
    {
        // AArch64 encoders for the fake accessor code.
        auto adrp = [](int rd, uint64_t pc, uint64_t target) {
            const int64_t imm = (int64_t(target & ~uint64_t(0xFFF)) - int64_t(pc & ~uint64_t(0xFFF))) >> 12;
            return uint32_t(0x90000000u | (uint32_t(imm & 3) << 29) | (uint32_t((imm >> 2) & 0x7FFFF) << 5) | uint32_t(rd));
        };
        auto add_imm = [](int rd, int rn, uint32_t imm) { return 0x91000000u | (imm << 10) | (uint32_t(rn) << 5) | uint32_t(rd); };
        auto ldr_x = [](int rt, int rn, uint32_t off) { return 0xF9400000u | ((off / 8) << 10) | (uint32_t(rn) << 5) | uint32_t(rt); };
        auto bl = [](uint64_t pc, uint64_t target) { return 0x94000000u | (uint32_t((target - pc) >> 2) & 0x3FFFFFFu); };
        auto cbz_x = [](int rt, int64_t off) { return 0xB4000000u | (uint32_t((off >> 2) & 0x7FFFF) << 5) | uint32_t(rt); };
        const uint32_t RET = 0xD65F03C0, BRK = 0xD4200000, NOP = 0xD503201F, PACIASP = 0xD503233F;
        const uint32_t STP_X19_X30 = 0xA9BF7BF3, LDP_X19_X30 = 0xA8C17BF3, STP_FP_LR = 0xA9BF7BFD, LDP_FP_LR = 0xA8C17BFD;
        const uint32_t MOV_FP_SP = 0x910003FD, MOV_X19_X0 = 0xAA0003F3;

        auto emit = [&](uint64_t at, std::vector<uint32_t> words) { std::memcpy(reinterpret_cast<void*>(at), words.data(), words.size() * 4); };
        // il2cpp wrapper: load the icall slot, call it if filled, trap otherwise.
        auto wrapper = [&](uint64_t at, uint64_t slot) {
            emit(at, {PACIASP, STP_X19_X30, adrp(19, at + 8, slot), ldr_x(8, 19, uint32_t(slot & 0xFFF)), cbz_x(8, 16),
                      0xD63F0100 /* blr x8 */, LDP_X19_X30, RET, BRK});
        };

        const uint64_t lib = proc::lib;
        uint8_t* rs = static_cast<uint8_t*>(calloc(1, 0x200));
        auto put_f = [](uint8_t* o, int off, float v) { std::memcpy(o + off, &v, 4); };
        auto init_rs = [&](uint8_t* o) {
            o[0x40] = 0;                                                  // m_Fog
            put_f(o, 0x44, .5f); put_f(o, 0x48, .5f); put_f(o, 0x4C, .5f); put_f(o, 0x50, 1.f);  // m_FogColor
            const int32_t mode = 3; std::memcpy(o + 0x54, &mode, 4);      // m_FogMode
            put_f(o, 0x58, .01f); put_f(o, 0x5C, 0.f); put_f(o, 0x60, 300.f);
            put_f(o, 0x70, .1f); put_f(o, 0x74, .2f); put_f(o, 0x78, .3f); put_f(o, 0x7C, 1.f);  // ambient sky
            put_f(o, 0x80, .3f); put_f(o, 0x84, .35f); put_f(o, 0x88, .4f); put_f(o, 0x8C, 1.f); // ambient equator
        };
        init_rs(rs);

        const uint64_t data = lib + 0xC480000;          // globals: manager pointer and icall slots
        const uint64_t manager = data;
        *reinterpret_cast<uint64_t*>(manager) = reinterpret_cast<uint64_t>(rs);
        auto slot = [&](int i) { return data + 0x100 + uint64_t(i) * 8; };
        const uint64_t code = lib + 0x200000;           // native engine code
        const uint64_t grs = code;                       // RenderSettings& GetRenderSettings()
        emit(grs, {adrp(8, grs, manager), ldr_x(0, 8, uint32_t(manager & 0xFFF)), RET});
        auto native = [&](uint64_t at, std::vector<uint32_t> body) {
            std::vector<uint32_t> w{STP_FP_LR, MOV_FP_SP, bl(at + 8, grs)};
            w.insert(w.end(), body.begin(), body.end());
            w.push_back(LDP_FP_LR); w.push_back(RET);
            emit(at, w);
        };
        const uint64_t n_start = code + 0x100, n_end_set = code + 0x200, n_fog = code + 0x300, n_mode = code + 0x400,
                       n_color = code + 0x500, n_obj = code + 0x600, n_equator = code + 0x700, n_sky = code + 0x800;
        native(n_start, {0xBD400000u | ((0x5C / 4) << 10)});                                   // ldr s0, [x0,#0x5C]
        emit(n_end_set, {STP_FP_LR, MOV_FP_SP, 0x1E204001 /* fmov s1,s0 */, bl(n_end_set + 12, grs),
                         0xBD000000u | ((0x60 / 4) << 10) | 1 /* str s1,[x0,#0x60] */, LDP_FP_LR, RET});
        native(n_fog, {0x39400000u | (0x40 << 10) | 8 /* ldrb w8,[x0,#0x40] */, 0x7100011F /* cmp w8,#0 */, 0x1A9F07E0 /* cset w0,ne */});
        native(n_mode, {0xB9400000u | ((0x54 / 4) << 10)});                                    // ldr w0, [x0,#0x54]
        native(n_obj, {});                                                                     // returns x0 = RenderSettings*
        auto color_getter = [&](uint64_t at, int off) {
            emit(at, {STP_X19_X30, MOV_X19_X0, bl(at + 8, grs),
                      0x3CC00000u | (uint32_t(off & 0x1FF) << 12) /* ldur q0,[x0,#off] */, 0x3D800260 /* str q0,[x19] */,
                      LDP_X19_X30, RET});
        };
        color_getter(n_color, 0x44);
        color_getter(n_equator, 0x80);
        color_getter(n_sky, 0x70);

        // Wrappers at the real RVAs. Filled slots: start getter, end setter, fog getter, mode getter,
        // color getter, GetRenderSettings. Everything else is left unresolved on purpose.
        const uint64_t all[] = {v100::rs_get_fog, v100::rs_set_fog, v100::rs_get_fog_start, v100::rs_set_fog_start,
                                v100::rs_get_fog_end, v100::rs_set_fog_end, v100::rs_get_fog_mode, v100::rs_set_fog_mode,
                                v100::rs_get_fog_density, v100::rs_set_fog_density, v100::rs_get_fog_color_injected,
                                v100::rs_set_fog_color_injected, v100::rs_get_ambient_equator_injected,
                                v100::rs_set_ambient_equator_injected, v100::rs_get_ambient_sky_injected,
                                v100::rs_set_ambient_sky_injected, v100::rs_get_render_settings_injected};
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wrapper(lib + all[i], slot(int(i)));
        auto fill = [&](uint64_t rva, uint64_t fn) {
            for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
                if (all[i] == rva) *reinterpret_cast<uint64_t*>(slot(int(i))) = fn;
        };
        fill(v100::rs_get_fog_start, n_start);
        fill(v100::rs_set_fog_end, n_end_set);
        fill(v100::rs_get_fog, n_fog);
        fill(v100::rs_get_fog_mode, n_mode);
        fill(v100::rs_get_fog_color_injected, n_color);
        fill(v100::rs_get_render_settings_injected, n_obj);

        // A file-backed "libunity.so": icall names, a {name, fn} table (equator) and a
        // registration call site in an executable page (sky).
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
        *reinterpret_cast<uint64_t*>(unity + 0x408) = n_equator;
        const uint64_t site = reinterpret_cast<uint64_t>(unity + 0x1000);
        const int64_t reach = int64_t(n_sky) - int64_t(site);
        const bool sky_reachable = reach > -(int64_t(1) << 32) && reach < (int64_t(1) << 32);
        if (sky_reachable) {
            emit(site, {NOP, adrp(0, site + 4, reinterpret_cast<uint64_t>(unity + 0x200)),
                        add_imm(0, 0, uint32_t(reinterpret_cast<uint64_t>(unity + 0x200) & 0xFFF)),
                        adrp(1, site + 12, n_sky), add_imm(1, 1, uint32_t(n_sky & 0xFFF)), bl(site + 20, site), RET});
        }
        const bool exec_ok = mprotect(unity + 0x1000, 0x1000, PROT_READ | PROT_EXEC) == 0;

        cfg::fog::enabled = true; cfg::fog::auto_color = true; cfg::fog::min_distance = 25.f; cfg::fog::max_distance = 90.f;
        worldfog::g_pid = -1;
        worldfog::tick();
        const uint64_t base = reinterpret_cast<uint64_t>(rs);
        CHECK(worldfog::g_object == base, "GetRenderSettings traced");
        CHECK(worldfog::g_fields[worldfog::Start].address == base + 0x5C, "start via getter %s", worldfog::g_fields[worldfog::Start].how.c_str());
        CHECK(worldfog::g_fields[worldfog::End].address == base + 0x60 && worldfog::g_fields[worldfog::End].setter, "end via setter %s", worldfog::g_fields[worldfog::End].how.c_str());
        CHECK(worldfog::g_fields[worldfog::Fog].address == base + 0x40, "fog via cset getter %s", worldfog::g_fields[worldfog::Fog].how.c_str());
        CHECK(worldfog::g_fields[worldfog::Mode].address == base + 0x54, "mode via getter");
        CHECK(worldfog::g_fields[worldfog::FogColor].address == base + 0x44, "color via q copy %s", worldfog::g_fields[worldfog::FogColor].how.c_str());
        CHECK(worldfog::g_fields[worldfog::Density].address == base + 0x58 && worldfog::g_fields[worldfog::Density].how == "inferred from fog start",
              "density inferred: %s", worldfog::g_fields[worldfog::Density].how.c_str());
        CHECK(worldfog::g_fields[worldfog::Equator].address == base + 0x80, "equator via icall table: %s", worldfog::g_fields[worldfog::Equator].how.c_str());
        if (sky_reachable && exec_ok)
            CHECK(worldfog::g_fields[worldfog::Sky].address == base + 0x70, "sky via registration site: %s", worldfog::g_fields[worldfog::Sky].how.c_str());
        else std::printf("note: registration call-site case skipped (exec=%d reach=%d)\n", exec_ok, sky_reachable);

        float f = 0.f;
        CHECK(rs[0x40] == 1, "fog on");
        int32_t mode = 0; std::memcpy(&mode, rs + 0x54, 4); CHECK(mode == 1, "linear");
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
        *reinterpret_cast<uint64_t*>(manager) = reinterpret_cast<uint64_t>(rs2);
        worldfog::g_last_check = 0;
        worldfog::tick();                       // notices the move
        worldfog::g_last_write = 0;
        worldfog::tick();                       // resolves again and applies
        CHECK(worldfog::g_fields[worldfog::Start].address == reinterpret_cast<uint64_t>(rs2) + 0x5C, "followed the move");
        CHECK(rs2[0x40] == 1, "fog on after the move");

        cfg::fog::enabled = false;
        worldfog::tick();
        std::memcpy(&f, rs2 + 0x60, 4);
        std::memcpy(&mode, rs2 + 0x54, 4);
        CHECK(rs2[0x40] == 0 && mode == 3 && f == 300.f, "restored fog=%d mode=%d end=%.1f", rs2[0x40], mode, f);
        munmap(unity, 0x2000);
        close(fd);
        unlink(so.c_str());
        rmdir(unity_path);
    }

    std::printf(failures ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
