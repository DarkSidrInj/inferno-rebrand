#pragma once

#include "imgui.h"
#include "../CGame/CGameContext.hpp"
#include "../CMath/CVector3.hpp"
#include <cstdint>
#include <vector>

namespace visuals {
    void draw();
    // Called from draw() with the camera that was picked for this frame.
    void draw_skeletons(const matrix& view, const matrix& projection, const std::vector<uint64_t>& enemies);
    // Third person: the own character drawn from its bones (the real body is not in the render scene).
    void draw_local_model(const matrix& view, const matrix& projection, uint64_t local);
    void draw_world_items(const matrix& view, const matrix& projection, const Vector3& local_position);
    void dbox(const ImVec2& t, const ImVec2& b, float a);
    void dhp(int hp, const ImVec2& t, const ImVec2& b, float box_h, float a);
    void dnick(const char* name, const ImVec2& box_max, float cx, float sz, float a);
    void ddist(float dist, float x, float y, float sz, float a);
    void draw_text_outlined(ImDrawList* dl, ImFont* font, float size, const ImVec2& pos, ImU32 color, const char* text);
}
