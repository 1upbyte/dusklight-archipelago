#pragma once

// The in-game overlay: one small panel of Archipelago info drawn in the game's own HUD pass
// (J2D boxes and the game's font), so it's there during play the way the HUD is, and hides with
// it. What goes in the panel is decided by ap_mode.cpp; this only lays it out and draws it.

#include <cstdint>
#include <string>
#include <vector>

namespace ap::overlay {

struct Line {
    enum class Kind { Text, Bar, Divider };
    Kind kind = Kind::Text;
    // Text: `left` from the left edge, `right` against the right edge (either may be empty).
    // Printable ASCII only: the game's font has little else.
    std::string left;
    std::string right;
    uint32_t leftColor = 0xF2EEE2FF;  // RGBA
    uint32_t rightColor = 0xA9A493FF;
    bool small = false;               // secondary line, drawn smaller
    // Bar: filled to `fill` (0..1) in `barColor`.
    float fill = 0.0f;
    uint32_t barColor = 0x6FD08CFF;

    static Line text(std::string l, uint32_t lc, std::string r = {}, uint32_t rc = 0xA9A493FF,
        bool small = false) {
        Line line;
        line.left = std::move(l);
        line.right = std::move(r);
        line.leftColor = lc;
        line.rightColor = rc;
        line.small = small;
        return line;
    }
    static Line bar(float fill, uint32_t color) {
        Line line;
        line.kind = Kind::Bar;
        line.fill = fill;
        line.barColor = color;
        return line;
    }
    static Line divider() {
        Line line;
        line.kind = Kind::Divider;
        return line;
    }
};

enum class Corner { TopLeft, TopRight, MiddleLeft, MiddleRight, BottomLeft, BottomRight };

struct Layout {
    Corner corner = Corner::MiddleRight;
    float offsetX = 0.0f;   // away from the chosen side
    float offsetY = 0.0f;   // down (top/middle) or up (bottom)
    float scale = 1.0f;
    uint8_t backgroundAlpha = 160;
    uint32_t accent = 0x6FD08CFF;  // the strip down the panel's left edge (connection status)
};

// Call from the HUD draw hook only (it needs the game's current graphics port).
void draw_panel(const std::vector<Line>& lines, const Layout& layout);

// Text made safe for the game's font: printable ASCII kept, anything else dropped, cut to
// maxChars with "..." if longer.
std::string ascii_only(const std::string& text, size_t maxChars);

}  // namespace ap::overlay
