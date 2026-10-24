#include "Android_draw/draw.h"
#include "fonts/menu/pixeloperator.h"
#include "fonts/esp/esp.h"
#include "fonts/menu/neverlose_fonts.h"

EGLDisplay display = EGL_NO_DISPLAY;
EGLConfig config;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;

ANativeWindow *native_window;
ImFont* fontBold;
ImFont* fontMedium;
ImFont* fontDesc;
ImFont* espFont;
ImFont* nlText;
ImFont* nlBold;
ImFont* nlIcons;

int native_window_screen_x = 0;
int native_window_screen_y = 0;
android::ANativeWindowCreator::DisplayInfo displayInfo{0};
uint32_t orientation = 0;
bool g_Initialized = false;
ImGuiWindow *g_window = nullptr;

bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    orientation = displayInfo.orientation;

    if (!init_egl(_screen_x, _screen_y, log)) {
        return false;
    }

    if (!ImGui_init()) {
        return false;
    }

    return true;
}

bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    ::native_window = android::ANativeWindowCreator::Create("Overlay", _screen_x, _screen_y, false);

    ANativeWindow_acquire(native_window);
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        return false;
    }

    if (eglInitialize(display, 0, 0) != EGL_TRUE) {
        return false;
    }

    EGLint num_config = 0;
    const EGLint attribList[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 5,
        EGL_GREEN_SIZE, 6,
        EGL_RED_SIZE, 5,
        EGL_BUFFER_SIZE, 32,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint attrib_list[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        3,
        EGL_NONE
    };

    if (eglChooseConfig(display, attribList, &config, 1, &num_config) != EGL_TRUE) {
        return false;
    }

    EGLint egl_format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, egl_format);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);
    if (context == EGL_NO_CONTEXT) {
        return false;
    }

    surface = eglCreateWindowSurface(display, config, native_window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        return false;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        return false;
    }

    return true;
}

void screen_config() {
    displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
}

bool ImGui_init() {
    if (g_Initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplAndroid_Init(native_window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;

    // The font data is static: the atlas must not free it (it did, on exit: "Pointer tag ... truncated", abort).
    ImFontConfig static_font;
    static_font.FontDataOwnedByAtlas = false;
    fontBold = io.Fonts->AddFontFromMemoryTTF(pixeloperator, sizeof(pixeloperator), 32, &static_font, io.Fonts->GetGlyphRangesCyrillic());
    fontMedium = io.Fonts->AddFontFromMemoryTTF(pixeloperator, sizeof(pixeloperator), 28, &static_font, io.Fonts->GetGlyphRangesCyrillic());
    fontDesc = io.Fonts->AddFontFromMemoryTTF(pixeloperator, sizeof(pixeloperator), 22, &static_font, io.Fonts->GetGlyphRangesCyrillic());
    io.FontDefault = fontMedium;

    espFont = io.Fonts->AddFontFromMemoryTTF(esp, sizeof(esp), 18, &static_font, io.Fonts->GetGlyphRangesCyrillic());

    // The Neverlose-style menu: text (Medium, Bold) and the icon font. The data is static, the atlas must not free it.
    {
        static const ImWchar text_ranges[] = {0x0020, 0x00FF, 0x0400, 0x04FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0};
        static const ImWchar icon_ranges[] = {0xF000, 0xF8FF, 0};
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        nlText = io.Fonts->AddFontFromMemoryTTF(nl_font_medium, sizeof(nl_font_medium), 30, &config, text_ranges);
        nlBold = io.Fonts->AddFontFromMemoryTTF(nl_font_bold, sizeof(nl_font_bold), 34, &config, text_ranges);
        nlIcons = io.Fonts->AddFontFromMemoryTTF(nl_font_icons, sizeof(nl_font_icons), 28, &config, icon_ranges);
    }

    ImGui::GetStyle().ScaleAllSizes(3.0f);
    ::g_Initialized = true;
    return true;
}

void drawBegin() {
    screen_config();
    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        touch::update(displayInfo.width, displayInfo.height, displayInfo.orientation);
        if (g_window) {
            g_window->Pos.x = 100;
            g_window->Pos.y = 125;
        }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(native_window_screen_x, native_window_screen_y);
    ImGui::NewFrame();
}

void drawEnd() {
    ImGui::Render();
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(display, surface);
}

void shutdown() {
    if (!g_Initialized) {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
        eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;

    ANativeWindow_release(native_window);
    android::ANativeWindowCreator::Destroy(native_window);
    ::g_Initialized = false;
}
