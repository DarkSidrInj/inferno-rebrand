#pragma once

#include "../CGame/CGameContext.hpp"
#include "CVector3.hpp"
#include "../CMenu/Theme/CTheme.hpp"
#include "imgui.h"
#include <cmath>

inline Vector4 matrix_multiply(const matrix& m, const Vector4& v) noexcept {
    const float* a = reinterpret_cast<const float*>(&m);
    return Vector4(
        a[0] * v.x + a[4] * v.y + a[8] * v.z + a[12] * v.w,
        a[1] * v.x + a[5] * v.y + a[9] * v.z + a[13] * v.w,
        a[2] * v.x + a[6] * v.y + a[10] * v.z + a[14] * v.w,
        a[3] * v.x + a[7] * v.y + a[11] * v.z + a[15] * v.w);
}

inline bool world_to_screen(const Vector3& pos, const matrix& view, const matrix& projection, ImVec2& out) noexcept {
    Vector4 camera = matrix_multiply(view, Vector4(pos.x, pos.y, pos.z, 1.f));
    Vector4 clip = matrix_multiply(projection, camera);
    if (-camera.z <= 0.05f || !std::isfinite(clip.w) || std::fabs(clip.w) <= 0.0001f) {
        out = ImVec2(-10000.f, -10000.f);
        return false;
    }

    float nx = clip.x / clip.w;
    float ny = clip.y / clip.w;

    out.x = (nx + 1.f) * 0.5f * g_sw;
    out.y = (1.f - ny) * 0.5f * g_sh;

    return out.x > -g_sw && out.x < g_sw * 2.f && out.y > -g_sh && out.y < g_sh * 2.f;
}

inline float calculate_distance(const Vector3& a, const Vector3& b) noexcept {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
