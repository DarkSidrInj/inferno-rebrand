#pragma once
// Distance fog for the ESP overlay. An element closer than fog::min_distance keeps its
// configured color, one at or beyond fog::max_distance is fully blended into the fog
// color, and in between the blend is linear in distance. Alpha is left alone so the
// element stays visible, only its color sinks into the fog. Health bars are not fogged
// so HP always reads.
//
// Color estimate: distfog::col() returns the color an element takes at the distance
// set in g_factor, so the caller only sets g_factor per target and draws as usual.

#include "imgui.h"
#include "../CConfigs/CConfig.hpp"
#include <algorithm>

namespace distfog {
    // 0 = no fog, 1 = fully fogged. Set per target before its box, name and distance draw.
    inline float g_factor = 0.f;

    inline float factor_for(float distance) {
        if (!cfg::fog::enabled) return 0.f;
        const float lo = std::max(cfg::fog::min_distance, 0.f);
        const float hi = std::max(cfg::fog::max_distance, lo + 0.1f);
        return std::clamp((distance - lo) / (hi - lo), 0.f, 1.f);
    }

    inline ImU32 col(const ImVec4& base, float a) {
        const float t = g_factor;
        const ImVec4& f = cfg::fog::color;
        return IM_COL32(
            static_cast<int>((base.x + (f.x - base.x) * t) * 255),
            static_cast<int>((base.y + (f.y - base.y) * t) * 255),
            static_cast<int>((base.z + (f.z - base.z) * t) * 255),
            static_cast<int>(base.w * 255 * a)
        );
    }
}
