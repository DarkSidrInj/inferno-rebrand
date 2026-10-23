#pragma once
#include "imgui.h"

namespace cfg {

namespace esp {
    inline bool box = true;
    inline bool name = false;
    inline bool health = true;
    inline bool distance = false;
    inline int box_type = 0;
    inline float fallback_fov = 60.f;  // vertical FOV of the synthetic camera (only when the game Camera is not found)
    inline float box_rounding = 0.f;
    inline ImVec4 box_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);
    inline ImVec4 name_col = ImVec4(1.f, 1.f, 1.f, 1.f);
    inline ImVec4 health_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);
    inline ImVec4 distance_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);

    inline bool skeleton = false;
    inline bool skeleton_all = true;   // every mapped bone (fingers, toes, jaw), never the weapon
    inline bool skeleton_occlusion = true;  // keep enemy animators running behind walls
    inline float skeleton_thickness = 1.5f;
    inline ImVec4 skeleton_col = ImVec4(1.f, 1.f, 1.f, 1.f);

    inline bool grenade = true;
    inline ImVec4 grenade_col = ImVec4(1.f, 0.6f, 0.2f, 1.f);
    inline bool dropped = false;
    inline ImVec4 dropped_col = ImVec4(0.85f, 0.85f, 0.95f, 1.f);
    inline float world_distance = 60.f;  // meters, grenades and dropped weapons
}

namespace chams {
    inline bool enabled = false;      // chams kind 1: the game's renderer gets a flat / untouchable / outline material
    inline bool enemies = true;
    inline bool teammates = false;
    inline bool local_body = false;
    inline bool local_arms = false;
    inline bool local_weapon = false;
    inline int material = 0;         // 0 flat (material id 0, as the legacy chams), 1 untouchable, 2 outline
    inline bool through_walls = false; // experimental (no menu): a material drawn without depth test; does not work on this build
    inline bool overlay = false;      // chams kind 2: filled limbs drawn on screen along the skeleton, through walls
    inline ImVec4 overlay_col = ImVec4(1.f, .25f, .35f, 1.f);
    inline bool overlay_shaded = true;    // darker base with a lit inner tube per part
    inline float overlay_outline = 1.5f;  // pixels, 0 = none
    inline ImVec4 overlay_outline_col = ImVec4(0.f, 0.f, 0.f, 1.f);
    inline bool no_occlusion = false; // experimental (no menu): camera occlusion culling off while through walls is on
    inline bool layer_pass = false;   // experimental: enemy bodies move to layer 30, drawn by the first-person camera (depth cleared)
    inline int wall_material = 2;     // 0 / 1 the game's outline mask materials (rdj +0x38 / +0x40), 2 Unity default UI, 3 untouchable, 4 own outline
}

namespace thirdperson {
    inline bool enabled = false;
    inline float distance = 2.5f;   // meters behind the eye
    inline float height = 0.3f;     // meters above the eye
    inline float side = 0.f;        // meters to the right (negative: left shoulder)
    inline bool show_body = true;   // draw the own body, hide first-person arms, gloves and weapon
    inline bool try_enable_body = false;  // replay the game's set_enabled on the body renderer once (fails on device)
    inline bool real_body = false;      // the game switches the real body on itself: its set_enabled, called on its main thread through ptrace
    inline bool real_model = false;     // hand the arms' scene node to the body renderer (the game's own mesh and skin)
    inline bool flat_body = false;     // flat white body material instead of the real skin
    inline bool model_overlay = true;  // draw the own character from its bones (its real renderer is not in the scene)
    inline ImVec4 model_col = ImVec4(.86f, .91f, 1.f, 1.f);
    inline ImVec4 model_outline_col = ImVec4(0.f, 0.f, 0.f, 1.f);
    inline float model_outline = 1.5f;
}

namespace skins {
    inline bool enabled = false;
    inline int chosen[256]{};       // WeaponId raw -> SkinDefinition id (0: the game's own)
    inline bool code_patch = false;  // knives load as the skin's weapon (writes one instruction of the game's code, opt-in)
    inline bool force_model = false; // also write the skin's model into the weapon's own tables (the earlier way)
}

namespace antiaim {
    inline bool enabled = false;
    inline int yaw_mode = 1;        // 0 legacy (degenerate quaternions), 1 spin, 2 jitter, 3 backwards, 4 none
    inline int pitch_mode = 0;      // 0 none, 1 down, 2 up
    inline float spin_speed = 720.f;  // degrees per second
    inline float jitter = 90.f;       // degrees each side
}

namespace fog {
    inline bool enabled = false;
    inline bool auto_color = true;       // estimate the color from the scene's ambient light
    inline float min_distance = 20.f;    // meters: fog starts here
    inline float max_distance = 150.f;   // meters: fully fogged from here
    inline ImVec4 color = ImVec4(0.12f, 0.13f, 0.16f, 1.f);  // manual color (auto_color off)
}

}


