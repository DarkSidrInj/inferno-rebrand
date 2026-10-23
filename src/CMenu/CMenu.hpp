#pragma once

namespace ui::menu {
    void render();
    // The exit button was confirmed: the main loop should leave and clean up.
    bool exit_requested();
}
