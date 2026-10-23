#include "Android_draw/draw.h"
#include "CMenu/Theme/CTheme.hpp"
#include "CMenu/CMenu.hpp"
#include "CMenu/Widgets/CStatusBar.hpp"
#include "CUtils/CMemory.hpp"
#include "CGame/CGameContext.hpp"
#include "CGame/CFog.hpp"
#include "CGame/CThirdPerson.hpp"
#include "CGame/CAntiAim.hpp"
#include "CGame/CLocalView.hpp"
#include "CVisual/CVisualManager.hpp"
#include "CProtection/COxorany.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

static void print_status(const char* status) {
    diag::log("STATUS", "%s", status);
}

// Функция для запуска Standoff
static void launch_standoff() {
    int rc = system(oxorany("monkey -p com.axlebolt.standoff2 -c android.intent.category.LAUNCHER 1"));
    diag::log(rc == 0 ? "INFO" : "WARN", "game launch command exit=%d package=com.axlebolt.standoff2", rc);
}

int main() {
    const int stdout_flags = ::fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (stdout_flags >= 0) ::fcntl(STDOUT_FILENO, F_SETFL, stdout_flags | O_NONBLOCK);
    const int instance_lock = ::open("/data/local/tmp/bycmd-1.0.0.lock", O_CREAT | O_RDWR, 0600);
    if (instance_lock < 0 || flock(instance_lock, LOCK_EX | LOCK_NB) != 0) {
        diag::log("ERROR", "another bycmd instance already owns the overlay; refusing duplicate menu");
        if (instance_lock >= 0) ::close(instance_lock);
        return 2;
    }
    diag::log("INFO", "bycmd Standoff2 1.0.0 external ESP starting");
    screen_config();

    int max_size = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    int min_size = (displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width);

    g_sw = static_cast<float>(max_size);
    g_sh = static_cast<float>(min_size);

    native_window_screen_x = max_size;
    native_window_screen_y = max_size;

    diag::log("INFO", "display width=%d height=%d orientation=%d overlay=%dx%d",
              displayInfo.width, displayInfo.height, displayInfo.orientation,
              native_window_screen_x, native_window_screen_y);
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
        diag::log("ERROR", "initGUI_draw failed");
        return -1;
    }
    diag::log("INFO", "GUI initialized");

    touch::init(max_size, min_size, (uint8_t)displayInfo.orientation);

    // Автоматический запуск Standoff
    print_status(oxorany("Game detect ✅"));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    print_status(oxorany("start cheat...."));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    launch_standoff();
    
    game::init();

    static float alpha = 0.f;
    static bool prev = false;
    static bool game_started = false;

    while (true) {
        drawBegin();

        bool run = game::valid();

#if defined(__x86_64__)
        bool is_landscape = (displayInfo.orientation == 0 || displayInfo.orientation == 2);
#else
        bool is_landscape = (displayInfo.orientation == 1 || displayInfo.orientation == 3);
#endif

        if (run && !prev) {
            if (!game_started) {
                print_status(oxorany("Game detect ✅"));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                print_status(oxorany("start cheat...."));
                game_started = true;
            } else {
                print_status(oxorany("Game detect ✅"));
            }
            prev = true;
        } else if (!run && prev) {
            print_status(oxorany("game closed"));
            prev = false;
            game_started = false;
        }

        if (is_landscape) {
            ImGuiIO& io = ImGui::GetIO();
            float dt = io.DeltaTime;
            if (dt <= 0.f || dt > 0.1f) dt = 0.016f;

            float target = run ? 1.f : 0.f;
            float spd = run ? 4.f : 6.f;

            if (alpha < target) {
                alpha += dt * spd;
                if (alpha > target) alpha = target;
            } else if (alpha > target) {
                alpha -= dt * spd;
                if (alpha < target) alpha = target;
            }

            ui::bar::set_game_alpha(alpha);

            if (alpha > 0.001f) {
                ui::menu::render();
            }

            if (run && proc::lib != 0) {
                // Library health is independent from asynchronous registry discovery.
                game::check_lib(proc::lib);
                visuals::draw();
                worldfog::tick();
            }
        }

        bool vis = ui::bar::g_open;
        drawEnd();
        usleep(vis ? 1500 : 4000);
    }

    // Never leave the game with a frozen camera matrix or rotated pivots.
    thirdperson::release();
    localview::wait();
    localview::restore_all();
    antiaim::release();
    shutdown();
    return 0;
}
