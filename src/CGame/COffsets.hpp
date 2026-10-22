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
    // jgi is not always a direct parts entry: the chx part vds keeps it at +0x58 (vds.jyfd).
    inline constexpr uint64_t part_model = 0x58;            // vds.jyfd : jgi           (dump.cs)
    // BonesDictionary = SerializableDictionary<HumanBodyBones, Transform>. Its inner
    // Dictionary sits at 0x10 (skeed 0x08). Generic layout is not in the dump, so the
    // reader probes 0x10..0x30 and logs the offset it settles on (verify).
    inline constexpr uint64_t bones_dict_probe_first = 0x10;
    inline constexpr uint64_t bones_dict_probe_last = 0x30;

    // --- UnityEngine.RenderSettings il2cpp wrappers (script.json, same base as the TypeInfo RVAs) ---
    // Only read, never called: the interpreter in CUtils/CArm64.hpp walks them to the native
    // RenderSettings fields.
    inline constexpr uint64_t rs_get_fog = 0x7612954;                         // 123808084
    inline constexpr uint64_t rs_set_fog = 0x75F0BD4;                         // 123669460
    inline constexpr uint64_t rs_get_fog_start = 0x7616F70;                   // 123826032
    inline constexpr uint64_t rs_set_fog_start = 0x760D93C;                   // 123787580
    inline constexpr uint64_t rs_get_fog_end = 0x7601480;                     // 123737216
    inline constexpr uint64_t rs_set_fog_end = 0x76134EC;                     // 123811052
    inline constexpr uint64_t rs_get_fog_mode = 0x76145A0;                    // 123815328
    inline constexpr uint64_t rs_set_fog_mode = 0x75EA52C;                    // 123643180
    inline constexpr uint64_t rs_get_fog_density = 0x760B268;                 // 123777640
    inline constexpr uint64_t rs_set_fog_density = 0x76096D0;                 // 123770576
    inline constexpr uint64_t rs_get_fog_color_injected = 0x760C044;          // 123781188
    inline constexpr uint64_t rs_set_fog_color_injected = 0x75FDE94;          // 123723412
    inline constexpr uint64_t rs_get_ambient_equator_injected = 0x760993C;    // 123771196
    inline constexpr uint64_t rs_set_ambient_equator_injected = 0x76030B8;    // 123744440
    inline constexpr uint64_t rs_get_ambient_sky_injected = 0x7609EF4;        // 123772660
    inline constexpr uint64_t rs_set_ambient_sky_injected = 0x75E48DC;        // 123619548
    inline constexpr uint64_t rs_get_render_settings_injected = 0x75E7F30;    // 123633456

    // --- Animator culling (dump.cs RVAs; read by the interpreter, never called) -----------------
    inline constexpr uint64_t model_animator = 0x28;                          // jgi.<fjwo> : Animator
    inline constexpr uint64_t bindings_animator = 0x30;                       // CharacterBindings.<Animator>
    inline constexpr uint64_t animator_get_culling_injected = 0x88323C4;      // static get_cullingMode_Injected(IntPtr)
    inline constexpr uint64_t animator_set_culling_injected = 0x882E280;      // static set_cullingMode_Injected(IntPtr, mode)
    inline constexpr uint64_t animator_get_culling = 0x88342A0;               // instance get_cullingMode()
    inline constexpr uint64_t animator_set_culling = 0x882E200;               // instance set_cullingMode(mode)

    // --- Optional TypeInfo RVAs (ARM64 libunity). 0 = unknown, class name is used. ---
    // FILL (optional): script.json -> cim_TypeInfo / qak_TypeInfo / njg_TypeInfo / xmt_TypeInfo
    inline constexpr uint64_t grenade_typeinfo = 0;
    inline constexpr uint64_t grenade_collision_typeinfo = 0;
    inline constexpr uint64_t drop_typeinfo = 0;
    inline constexpr uint64_t game_camera_typeinfo = 0;
}
