#pragma once
// x64 offsets for the skeed-derived camera and ESP, Standoff 2 1.0.0 ARM64.
// Every value comes from the ARM64 dump.cs (field offsets are x64 there). The skeed
// ARM32 value is in the comment; the two agree under the usual shift: object header
// 8 -> 16 bytes, pointers 4 -> 8 bytes. Marked (verify) = not confirmed in game.

#include <cstdint>

namespace v100 {
    // --- NetObject (mfc) -> parts container (wmz) -----------------------------
    inline constexpr uint64_t net_systems = 0x20;           // mfc.kcej : wmz           (skeed 0x14)
    inline constexpr uint64_t wmz_first_array = 0x10;       // chx[], ftv[], nqv[], ggk[], som[], chx[]
    inline constexpr uint64_t wmz_last_array = 0x38;        // one pointer every 8 bytes
    inline constexpr uint64_t wmz_dictionary = 0x40;        // Dictionary<Type, chx>

    // --- System.Collections.Generic.Dictionary<K,V>, il2cpp x64 ---------------
    inline constexpr uint64_t dict_entries = 0x18;          // Entry[]                  (skeed 0x0C)
    inline constexpr uint64_t dict_count = 0x20;            // int                      (skeed 0x10)
    inline constexpr uint64_t dict_entry_stride = 0x18;     // {int hash, int next, K key, V value}
    inline constexpr uint64_t dict_entry_key = 0x08;
    inline constexpr uint64_t dict_entry_value = 0x10;

    // --- GameCamera: xmt : nrl --------------------------------------------------
    inline constexpr uint64_t gamecam_transform = 0x18;     // nrl.irub : Transform     (skeed 0x0C)
    inline constexpr uint64_t gamecam_camera = 0x20;        // nrl.hycu : Camera        (skeed 0x10)

    // --- Grenade (cim) and collision grenade (qak) -----------------------------
    inline constexpr uint64_t grenade_transform_part = 0x58;// cim.ioc                  (skeed 0x38)
    inline constexpr uint64_t grenade_info = 0x68;          // cim.jar                  (skeed 0x40)
    inline constexpr uint64_t grenade_info_weapon = 0x1C;   // jar.WeaponId             (skeed 0x14)
    inline constexpr uint64_t grenade_col_info = 0x58;      // qak.aih                  (skeed 0x38)
    inline constexpr uint64_t grenade_col_holder = 0x60;    // qak.uyb                  (skeed 0x3C)
    inline constexpr uint64_t grenade_col_weapon = 0x18;    // aih.WeaponId             (skeed 0x10)
    inline constexpr uint64_t grenade_col_transform = 0x18; // uyb.ppef : Transform     (skeed 0x0C)

    // --- Dropped weapon (njg) ----------------------------------------------------
    inline constexpr uint64_t drop_data = 0x68;             // njg.vsv                  (skeed 0x40)
    inline constexpr uint64_t drop_visibility = 0x70;       // njg.qdp                  (skeed 0x44)
    inline constexpr uint64_t drop_data_weapon = 0x48;      // vsv.kuze : WeaponId      (skeed 0x34)
    inline constexpr uint64_t bounds_center = 0x18;         // qdp.Bounds.center        (skeed 0x10)
    inline constexpr uint64_t bounds_extents = 0x24;        // qdp.Bounds.extents       (skeed 0x1C)

    // --- Player model -> bones -------------------------------------------------------
    inline constexpr uint64_t model_bindings = 0x30;        // jgi.CharacterBindings    (skeed 0x18)
    inline constexpr uint64_t bindings_bones = 0x20;        // CharacterBindings._bones (skeed 0x10)
    // BonesDictionary = SerializableDictionary<HumanBodyBones, Transform>. Its inner
    // Dictionary sits at 0x10 (skeed 0x08). Generic layout is not in the dump, so the
    // reader probes 0x10..0x30 and logs the offset it settles on (verify).
    inline constexpr uint64_t bones_dict_probe_first = 0x10;
    inline constexpr uint64_t bones_dict_probe_last = 0x30;

    // --- Optional TypeInfo RVAs (ARM64 libunity). 0 = unknown, class name is used. ---
    // FILL (optional): script.json -> cim_TypeInfo / qak_TypeInfo / njg_TypeInfo / xmt_TypeInfo
    inline constexpr uint64_t grenade_typeinfo = 0;
    inline constexpr uint64_t grenade_collision_typeinfo = 0;
    inline constexpr uint64_t drop_typeinfo = 0;
    inline constexpr uint64_t game_camera_typeinfo = 0;
}
