#pragma once
#include "../Theme/CTheme.hpp"
#include <vector>
#include <functional>
#include <string>

namespace ui::widgets {

bool checkbox(const char* name, bool* v, float a);
bool slider(const char* name, float* v, float mn, float mx, float a, const char* fmt = "%.1f");
bool combo(const char* name, int* v, const std::vector<const char*>& items, float a);
// One value out of many, stepped with the arrows at either end (long lists: no popup).
bool stepper(const char* name, int* v, const std::vector<std::string>& items, float a);
void colorpick(const char* name, ImVec4* v, float a);
void separator(float a);
bool button(const char* name, float a, std::function<void()> on_click = nullptr);

}
