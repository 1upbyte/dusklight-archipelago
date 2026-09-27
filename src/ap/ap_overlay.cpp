#include "ap_overlay.hpp"

#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "JSystem/JUtility/JUTFont.h"
#include "JSystem/JUtility/TColor.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_ext.h"
#include "m_Do/m_Do_graphic.h"

#include <algorithm>

namespace ap::overlay {

namespace {

JUtility::TColor color(uint32_t rgba) {
    return JUtility::TColor(static_cast<u8>(rgba >> 24), static_cast<u8>(rgba >> 16),
        static_cast<u8>(rgba >> 8), static_cast<u8>(rgba));
}

struct Metrics {
    float charW;
    float charH;
};

// Width of `text` drawn with glyphs `charW` wide, from the font's own advance table.
float text_width(JUTFont* font, const std::string& text, float charW) {
    const float cell = static_cast<float>(std::max<s32>(1, font->getWidth()));
    float w = 0.0f;
    for (const char c : text) {
        const int advance = font->isFixed() ? font->getFixedWidth()
                                            : font->getWidth(static_cast<u8>(c));
        w += static_cast<float>(advance) * charW / cell;
    }
    return w;
}

// The font draws from its baseline, so text sits at `top + ascent` to stay inside its line.
void draw_text(JUTFont* font, float x, float top, const Metrics& m, const std::string& text,
    uint32_t rgba) {
    if (text.empty()) {
        return;
    }
    const float baseline = top + m.charH * 0.82f;
    font->setGX();
    font->setCharColor(JUtility::TColor(0, 0, 0, static_cast<u8>((rgba & 0xFF) * 3 / 4)));
    font->drawString_scale(x + 1.0f, baseline + 1.0f, m.charW, m.charH, text.c_str(), true);
    font->setCharColor(color(rgba));
    font->drawString_scale(x, baseline, m.charW, m.charH, text.c_str(), true);
}

}  // namespace

std::string ascii_only(const std::string& text, size_t maxChars) {
    std::string out;
    for (const char c : text) {
        if (c >= 0x20 && c < 0x7F) {
            out += c;
        }
    }
    if (out.size() > maxChars && maxChars > 3) {
        out.resize(maxChars - 3);
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out += "...";
    }
    return out;
}

void draw_panel(const std::vector<Line>& lines, const Layout& layout) {
    if (lines.empty()) {
        return;
    }
    auto* port = dComIfGp_getCurrentGrafPort();
    JUTFont* font = mDoExt_getMesgFont();  // the game's dialogue font reads best at this size
    if (font == nullptr) {
        font = mDoExt_getSubFont();
    }
    if (port == nullptr || font == nullptr) {
        return;
    }

    const float s = std::clamp(layout.scale, 0.5f, 2.0f);
    const float aspect =
        static_cast<float>(font->getWidth()) / static_cast<float>(std::max<s32>(1, font->getHeight()));
    const Metrics normal{16.0f * s * aspect, 16.0f * s};
    const Metrics small{13.5f * s * aspect, 13.5f * s};
    const float padX = 10.0f * s;
    const float padY = 8.0f * s;
    const float accentW = 3.0f * s;
    const float gap = 14.0f * s;

    auto line_height = [&](const Line& l) {
        switch (l.kind) {
        case Line::Kind::Bar: return 9.0f * s;
        case Line::Kind::Divider: return 9.0f * s;
        default: return (l.small ? small.charH : normal.charH) + 5.0f * s;
        }
    };

    float contentW = 180.0f * s;
    float contentH = 0.0f;
    for (const auto& l : lines) {
        contentH += line_height(l);
        if (l.kind == Line::Kind::Text) {
            const auto& m = l.small ? small : normal;
            float w = text_width(font, l.left, m.charW);
            if (!l.right.empty()) {
                w += gap + text_width(font, l.right, m.charW);
            }
            contentW = std::max(contentW, w);
        }
    }
    contentW = std::min(contentW, 420.0f * s);
    const float panelW = accentW + contentW + 2.0f * padX;
    const float panelH = contentH + 2.0f * padY;
    const float screenW = mDoGph_gInf_c::getWidthF();
    const float screenH = mDoGph_gInf_c::getHeightF();

    const bool right = layout.corner == Corner::TopRight || layout.corner == Corner::MiddleRight ||
                       layout.corner == Corner::BottomRight;
    float x = right ? screenW - panelW - layout.offsetX : layout.offsetX;
    float y = 0.0f;
    switch (layout.corner) {
    case Corner::TopLeft:
    case Corner::TopRight: y = layout.offsetY; break;
    case Corner::MiddleLeft:
    case Corner::MiddleRight: y = (screenH - panelH) * 0.5f + layout.offsetY; break;
    default: y = screenH - panelH - layout.offsetY; break;
    }
    x = std::clamp(x, 0.0f, std::max(0.0f, screenW - panelW));
    y = std::clamp(y, 0.0f, std::max(0.0f, screenH - panelH));

    port->setup2D();
    J2DFillBox(x, y, panelW, panelH, JUtility::TColor(12, 11, 9, layout.backgroundAlpha));
    J2DFillBox(x, y, accentW, panelH, color(layout.accent));

    const float left = x + accentW + padX;
    const float rightEdge = x + panelW - padX;
    float cy = y + padY;
    for (const auto& l : lines) {
        const float h = line_height(l);
        switch (l.kind) {
        case Line::Kind::Bar: {
            const float barH = 4.0f * s;
            const float by = cy + (h - barH) * 0.5f;
            port->setup2D();
            J2DFillBox(left, by, contentW, barH, JUtility::TColor(255, 255, 255, 40));
            J2DFillBox(left, by, contentW * std::clamp(l.fill, 0.0f, 1.0f), barH, color(l.barColor));
            break;
        }
        case Line::Kind::Divider:
            port->setup2D();
            J2DFillBox(left, cy + h * 0.5f, contentW, std::max(1.0f, s), JUtility::TColor(255, 255, 255, 28));
            break;
        default: {
            const auto& m = l.small ? small : normal;
            draw_text(font, left, cy, m, l.left, l.leftColor);
            if (!l.right.empty()) {
                draw_text(font, rightEdge - text_width(font, l.right, m.charW), cy, m, l.right,
                    l.rightColor);
            }
            break;
        }
        }
        cy += h;
    }
}

}  // namespace ap::overlay
