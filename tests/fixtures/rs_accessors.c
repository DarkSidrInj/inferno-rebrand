// Unity-6-style RenderSettings accessors, compiled for aarch64 and embedded in the selftest
// (tests/fixtures/rs_accessors.h) so the interpreter in src/CUtils/CArm64.hpp is checked
// against real compiler output: il2cpp icall wrappers with lazily resolved slots, native
// icalls with a main-thread check that reads TLS, an error reporter, and PLT calls into
// symbols that are not there (SetDirty, il2cpp_codegen_resolve_icall).
//
// Regenerate with tests/fixtures/build_rs_accessors.sh.

typedef struct { float r, g, b, a; } Color;
typedef struct RenderSettings {
    void* vtable;
    char pad[0x38];
    unsigned char fog;          // 0x40
    Color fog_color;            // 0x44
    int mode;                   // 0x54
    float density, start, end;  // 0x58 0x5C 0x60
    char pad2[0xC];
    Color sky, equator, ground; // 0x70 0x80 0x90
} RenderSettings;

void* g_managers[64];
__thread int t_is_main;
int g_error_count;

extern void SetDirty(void* object);
extern void* il2cpp_codegen_resolve_icall(const char* name);
extern void DebugStringToFile(const char* text, int line);

__attribute__((noinline)) int CurrentThreadIsMainThread(void) { return t_is_main != 0; }

__attribute__((noinline)) void ThreadAndSerializationSafeCheckReportError(const char* name) {
    char buffer[256];
    int n = 0;
    for (const char* p = "UnityException: "; *p && n < 255; ++p) buffer[n++] = *p;
    for (const char* p = name; *p && n < 255; ++p) buffer[n++] = *p;
    buffer[n] = 0;
    for (int i = 0; i < n; ++i) if (buffer[i] == '_') g_error_count += i;
    DebugStringToFile(buffer, n);
}

__attribute__((noinline)) RenderSettings* GetRenderSettings(void) { return (RenderSettings*)g_managers[7]; }

#define CHECK_THREAD(name) do { if (!CurrentThreadIsMainThread()) ThreadAndSerializationSafeCheckReportError(name); } while (0)

float RenderSettings_get_fogStartDistance(void) { CHECK_THREAD("get_fogStartDistance"); return GetRenderSettings()->start; }
float RenderSettings_get_fogEndDistance(void) { CHECK_THREAD("get_fogEndDistance"); return GetRenderSettings()->end; }
void RenderSettings_set_fogEndDistance(float v) {
    CHECK_THREAD("set_fogEndDistance");
    RenderSettings* rs = GetRenderSettings();
    if (rs->end != v) { rs->end = v; SetDirty(rs); }
}
unsigned char RenderSettings_get_fog(void) { CHECK_THREAD("get_fog"); return GetRenderSettings()->fog; }
int RenderSettings_get_fogMode(void) { CHECK_THREAD("get_fogMode"); return GetRenderSettings()->mode; }
void RenderSettings_get_fogColor_Injected(Color* ret) { CHECK_THREAD("get_fogColor"); *ret = GetRenderSettings()->fog_color; }
void RenderSettings_get_ambientEquatorColor_Injected(Color* ret) { CHECK_THREAD("get_ambientEquatorColor"); *ret = GetRenderSettings()->equator; }
void RenderSettings_get_ambientSkyColor_Injected(Color* ret) { CHECK_THREAD("get_ambientSkyColor"); *ret = GetRenderSettings()->sky; }
void* RenderSettings_GetRenderSettings_Injected(void) { return GetRenderSettings(); }

// il2cpp wrappers: one lazily resolved function pointer per icall.
typedef float (*f_float)(void);
typedef void (*f_set_float)(float);
typedef unsigned char (*f_bool)(void);
typedef int (*f_int)(void);
typedef void (*f_color)(Color*);
typedef void* (*f_ptr)(void);

f_float slot_get_start, slot_get_end;
f_set_float slot_set_end;
f_bool slot_get_fog;
f_int slot_get_mode;
f_color slot_get_color, slot_get_equator;
f_ptr slot_get_object;

#define RESOLVE(slot, type, name) type f = slot; if (!f) { f = (type)il2cpp_codegen_resolve_icall(name); slot = f; }

float UnityEngine_RenderSettings__get_fogStartDistance(const void* method) {
    RESOLVE(slot_get_start, f_float, "UnityEngine.RenderSettings::get_fogStartDistance()"); return f();
}
float UnityEngine_RenderSettings__get_fogEndDistance(const void* method) {
    RESOLVE(slot_get_end, f_float, "UnityEngine.RenderSettings::get_fogEndDistance()"); return f();
}
void UnityEngine_RenderSettings__set_fogEndDistance(float value, const void* method) {
    RESOLVE(slot_set_end, f_set_float, "UnityEngine.RenderSettings::set_fogEndDistance(System.Single)"); f(value);
}
unsigned char UnityEngine_RenderSettings__get_fog(const void* method) {
    RESOLVE(slot_get_fog, f_bool, "UnityEngine.RenderSettings::get_fog()"); return f();
}
int UnityEngine_RenderSettings__get_fogMode(const void* method) {
    RESOLVE(slot_get_mode, f_int, "UnityEngine.RenderSettings::get_fogMode()"); return f();
}
void UnityEngine_RenderSettings__get_fogColor_Injected(Color* ret, const void* method) {
    RESOLVE(slot_get_color, f_color, "UnityEngine.RenderSettings::get_fogColor_Injected(UnityEngine.Color&)"); f(ret);
}
void UnityEngine_RenderSettings__get_ambientEquatorColor_Injected(Color* ret, const void* method) {
    RESOLVE(slot_get_equator, f_color, "UnityEngine.RenderSettings::get_ambientEquatorColor_Injected(UnityEngine.Color&)"); f(ret);
}
void* UnityEngine_RenderSettings__GetRenderSettings_Injected(const void* method) {
    RESOLVE(slot_get_object, f_ptr, "UnityEngine.RenderSettings::GetRenderSettings_Injected()"); return f();
}
