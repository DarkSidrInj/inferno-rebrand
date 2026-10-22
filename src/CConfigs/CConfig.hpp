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
    inline float skeleton_thickness = 1.5f;
    inline ImVec4 skeleton_col = ImVec4(1.f, 1.f, 1.f, 1.f);

    inline bool grenade = true;
    inline ImVec4 grenade_col = ImVec4(1.f, 0.6f, 0.2f, 1.f);
    inline bool dropped = false;
    inline ImVec4 dropped_col = ImVec4(0.85f, 0.85f, 0.95f, 1.f);
    inline float world_distance = 60.f;  // meters, grenades and dropped weapons
}

namespace fog {
    inline bool enabled = false;
    inline float min_distance = 20.f;    // meters: closer than this keeps its own color
    inline float max_distance = 150.f;   // meters: at or beyond this it is fully fogged
    inline ImVec4 color = ImVec4(0.12f, 0.13f, 0.16f, 1.f);  // estimated fog color
}

}


