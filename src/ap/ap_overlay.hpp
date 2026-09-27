#pragma once

// The in-game overlay: one small panel of Archipelago info drawn in the game's own HUD pass
// (J2D boxes and the game's font), so it's there during play the way the HUD is, and hides with
// it. What goes in the panel is decided by ap_mode.cpp; this only lays it out and draws it.

#include <cstdint>
#include <string>
#include <vector>

namespace ap::overlay {

struct Line {
    std::string text;       // printable ASCII only: the game's font has little else
    uint32_t rgba = 0xFFFFFFFF;
    float bar = -1.0f;      // >= 0: a progress bar filled to this fraction instead of text
};

enum class Corner { TopLeft, TopRight, MiddleLeft, MiddleRight, BottomLeft, BottomRight };

struct Layout {
    Corner corner = Corner::MiddleRight;
    float offsetX = 0.0f;   // away from the chosen edge
    float offsetY = 0.0f;   // down (top/middle) or up (bottom)
    float scale = 1.0f;
    uint8_t backgroundAlpha = 160;
};

// Call from the HUD draw hook only (it needs the game's current graphics port).
void draw_panel(const std::vector<Line>& lines, const Layout& layout);

// Text made safe for the game's font: printable ASCII kept, anything else dropped, cut to
// maxChars with "..." if longer.
std::string ascii_only(const std::string& text, size_t maxChars);

}  // namespace ap::overlay
