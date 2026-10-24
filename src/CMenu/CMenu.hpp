#pragma once

namespace ui::menu {
    void render();
    // The exit button was confirmed: the main loop should leave and clean up.
    bool exit_requested();
    // Night mode and fog as overlays on the screen (nothing of the game is touched): once per frame, before the ESP.
    void world_overlay();
}
