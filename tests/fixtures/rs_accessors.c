// Unity-6-style RenderSettings accessors, compiled for aarch64 and embedded in the selftest
// (tests/fixtures/rs_accessors.h) so the interpreter in src/CUtils/CArm64.hpp is checked
// against real compiler output: il2cpp icall wrappers with lazily resolved slots, native
// icalls with a main-thread check that reads TLS, an error reporter, and PLT calls into
// symbols that are not there (SetDirty, il2cpp_codegen_resolve_icall).
//
// Regenerate with tests/fixtures/build_rs_accessors.sh.

typedef struct { float r, g, b, a; } Color;
// Field order as traced on device (Standoff 2, Unity 6): fog, mode, color, start, end, density.
typedef struct RenderSettings {
    void* vtable;
    char pad[0x38];
    unsigned char fog;          // 0x40
    int mode;                   // 0x44
    Color fog_color;            // 0x48
    float start, end, density;  // 0x58 0x5C 0x60
    char pad2[0xC];
    Color sky, equator, ground; // 0x70 0x80 0x90
} RenderSettings;

// What the renderer reads: derived from the fields only inside the setters (ApplyFog).
float g_fog_params[4];
__attribute__((noinline)) void ApplyFog(RenderSettings* rs) {
    const float range = rs->end - rs->start;
    g_fog_params[0] = rs->density * 1.2011224f;
    g_fog_params[1] = rs->density * 1.4426950f;
    g_fog_params[2] = range != 0.0f ? -1.0f / range : 0.0f;
    g_fog_params[3] = range != 0.0f ? rs->end / range : 0.0f;
}

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
    if (rs->end != v) { rs->end = v; ApplyFog(rs); SetDirty(rs); }
}
void RenderSettings_set_fogStartDistance(float v) {
    CHECK_THREAD("set_fogStartDistance");
    RenderSettings* rs = GetRenderSettings();
    if (rs->start != v) { rs->start = v; ApplyFog(rs); SetDirty(rs); }
}
unsigned char RenderSettings_get_fog(void) { CHECK_THREAD("get_fog"); return GetRenderSettings()->fog; }
int RenderSettings_get_fogMode(void) { CHECK_THREAD("get_fogMode"); return GetRenderSettings()->mode; }
void RenderSettings_get_fogColor_Injected(Color* ret) { CHECK_THREAD("get_fogColor"); *ret = GetRenderSettings()->fog_color; }
void RenderSettings_get_ambientEquatorColor_Injected(Color* ret) { CHECK_THREAD("get_ambientEquatorColor"); *ret = GetRenderSettings()->equator; }
void RenderSettings_get_ambientSkyColor_Injected(Color* ret) { CHECK_THREAD("get_ambientSkyColor"); *ret = GetRenderSettings()->sky; }
void* RenderSettings_GetRenderSettings_Injected(void) { return GetRenderSettings(); }

// Animator (Unity 6 binding style: the icall gets the native object).
typedef struct Animator { void* vtable; char pad[0x50]; int culling_mode; /* 0x58 */ } Animator;
int Animator_get_cullingMode_Injected(void* self) { CHECK_THREAD("get_cullingMode"); return ((Animator*)self)->culling_mode; }
int g_always_animate_count;  // stands for the animator manager's bookkeeping
void Animator_set_cullingMode_Injected(void* self, int mode) {
    CHECK_THREAD("set_cullingMode");
    Animator* a = (Animator*)self;
    if (a->culling_mode == mode) return;
    if (mode == 0) g_always_animate_count++; else if (a->culling_mode == 0) g_always_animate_count--;
    a->culling_mode = mode;
}

// il2cpp wrappers: one lazily resolved function pointer per icall.
typedef float (*f_float)(void);
typedef void (*f_set_float)(float);
typedef unsigned char (*f_bool)(void);
typedef int (*f_int)(void);
typedef void (*f_color)(Color*);
typedef void* (*f_ptr)(void);

f_float slot_get_start, slot_get_end;
f_set_float slot_set_end, slot_set_start;
f_bool slot_get_fog;
f_int slot_get_mode;
f_color slot_get_color, slot_get_equator;
f_ptr slot_get_object;
typedef int (*f_cull)(void*);
typedef void (*f_set_cull)(void*, int);
f_cull slot_get_culling;
f_set_cull slot_set_culling;

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
void UnityEngine_RenderSettings__set_fogStartDistance(float value, const void* method) {
    RESOLVE(slot_set_start, f_set_float, "UnityEngine.RenderSettings::set_fogStartDistance(System.Single)"); f(value);
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
int UnityEngine_Animator__get_cullingMode_Injected(void* self, const void* method) {
    RESOLVE(slot_get_culling, f_cull, "UnityEngine.Animator::get_cullingMode_Injected(System.IntPtr)"); return f(self);
}
void UnityEngine_Animator__set_cullingMode_Injected(void* self, int mode, const void* method) {
    RESOLVE(slot_set_culling, f_set_cull, "UnityEngine.Animator::set_cullingMode_Injected(System.IntPtr,UnityEngine.AnimatorCullingMode)"); f(self, mode);
}

// Renderer (Unity 6 binding style): m_Materials is a dynamic_array of material instance ids.
typedef struct DynArray { int* data; long label; unsigned long size; unsigned long capacity; } DynArray;
typedef struct Renderer { void* vtable; char pad[0x110]; DynArray materials; /* data 0x118, size 0x128 */ } Renderer;
int Renderer_GetMaterialCount_Injected(void* self) { CHECK_THREAD("GetMaterialCount"); return (int)((Renderer*)self)->materials.size; }
typedef int (*f_count)(void*);
f_count slot_get_material_count;
int UnityEngine_Renderer__GetMaterialCount_Injected(void* self, const void* method) {
    RESOLVE(slot_get_material_count, f_count, "UnityEngine.Renderer::GetMaterialCount_Injected(System.IntPtr)"); return f(self);
}

// Canvas: the default UI material, created once and handed out.
void* g_default_ui_material;
void* Canvas_GetDefaultCanvasMaterial_Injected(void) { return g_default_ui_material; }
f_ptr slot_get_default_ui_material;
void* UnityEngine_Canvas__GetDefaultCanvasMaterial_Injected(const void* method) {
    RESOLVE(slot_get_default_ui_material, f_ptr, "UnityEngine.Canvas::GetDefaultCanvasMaterial_Injected()"); return f();
}

// Camera (Unity 6 binding style). The view matrix is recomputed from the transform while
// m_ImplicitWorldToCameraMatrix is set; the setter stores the matrix and clears the flag.
typedef struct Matrix4 { float m[16]; } Matrix4;
typedef struct Camera {
    void* vtable;
    char pad[0x18];
    void* game_object;          // 0x20
    char pad2[0x48];
    Matrix4 world_to_camera;    // 0x70
    Matrix4 projection;         // 0xB0
    char pad3[0x130];
    int culling_mask;           // 0x220
    char pad4[0x1C];
    unsigned char implicit_view;  // 0x240
    unsigned char other_flags;    // 0x241
} Camera;
Matrix4 g_transform_view;  // stands for the camera transform's inverse
__attribute__((noinline)) void UpdateViewFromTransform(Camera* c) { c->world_to_camera = g_transform_view; }
void Camera_get_worldToCameraMatrix_Injected(Camera* c, Matrix4* out) {
    CHECK_THREAD("get_worldToCameraMatrix");
    if (c->implicit_view) UpdateViewFromTransform(c);
    *out = c->world_to_camera;
}
void Camera_set_worldToCameraMatrix_Injected(Camera* c, const Matrix4* m) {
    CHECK_THREAD("set_worldToCameraMatrix");
    c->implicit_view = 0;
    c->world_to_camera = *m;
    SetDirty(c);
}
int Camera_get_cullingMask_Injected(Camera* c) { CHECK_THREAD("get_cullingMask"); return c->culling_mask; }
void Camera_set_cullingMask_Injected(Camera* c, int mask) { CHECK_THREAD("set_cullingMask"); c->culling_mask = mask; }
typedef void (*f_get_view)(Camera*, Matrix4*);
typedef int (*f_get_mask)(Camera*);
f_get_view slot_get_world_to_camera;
f_get_mask slot_get_culling_mask;
void UnityEngine_Camera__get_worldToCameraMatrix_Injected(void* self, Matrix4* ret, const void* method) {
    RESOLVE(slot_get_world_to_camera, f_get_view, "UnityEngine.Camera::get_worldToCameraMatrix_Injected(System.IntPtr,UnityEngine.Matrix4x4&)"); f((Camera*)self, ret);
}
int UnityEngine_Camera__get_cullingMask_Injected(void* self, const void* method) {
    RESOLVE(slot_get_culling_mask, f_get_mask, "UnityEngine.Camera::get_cullingMask_Injected(System.IntPtr)"); return f((Camera*)self);
}
