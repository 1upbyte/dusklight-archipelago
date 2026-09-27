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

JUtility::TColor color(uint32_t rgba, uint8_t alphaScale = 255) {
    const auto a = static_cast<uint8_t>(((rgba & 0xFF) * alphaScale) / 255);
    return JUtility::TColor(static_cast<u8>(rgba >> 24), static_cast<u8>(rgba >> 16),
        static_cast<u8>(rgba >> 8), a);
}

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
        out += "...";
    }
    return out;
}

void draw_panel(const std::vector<Line>& lines, const Layout& layout) {
    if (lines.empty()) {
        return;
    }
    auto* port = dComIfGp_getCurrentGrafPort();
    JUTFont* font = mDoExt_getSubFont();
    if (font == nullptr) {
        font = mDoExt_getMesgFont();
    }
    if (port == nullptr || font == nullptr) {
        return;
    }

    const float s = std::clamp(layout.scale, 0.5f, 2.0f);
    const float lineH = 17.0f * s;
    const float barH = 6.0f * s;
    const float charH = 15.0f * s;
    const float charW = charH * static_cast<float>(font->getWidth()) /
                        static_cast<float>(std::max<s32>(1, font->getHeight()));
    const float pad = 7.0f * s;

    float width = 110.0f * s;
    float height = 0.0f;
    for (const auto& line : lines) {
        height += line.bar >= 0.0f ? barH + 4.0f * s : lineH;
        if (line.bar < 0.0f) {
            width = std::max(width, text_width(font, line.text, charW));
        }
    }
    const float panelW = width + 2.0f * pad;
    const float panelH = height + 2.0f * pad;
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
    J2DFillBox(x, y, panelW, panelH, JUtility::TColor(0, 0, 0, layout.backgroundAlpha));

    float cy = y + pad;
    for (const auto& line : lines) {
        if (line.bar >= 0.0f) {
            const float fill = std::clamp(line.bar, 0.0f, 1.0f);
            port->setup2D();
            J2DFillBox(x + pad, cy + 2.0f * s, width, barH, JUtility::TColor(255, 255, 255, 50));
            J2DFillBox(x + pad, cy + 2.0f * s, width * fill, barH, color(line.rgba));
            cy += barH + 4.0f * s;
            continue;
        }
        font->setGX();
        font->setCharColor(JUtility::TColor(0, 0, 0, 170));
        font->drawString_scale(x + pad + 1.0f, cy + 1.0f, charW, charH, line.text.c_str(), true);
        font->setCharColor(color(line.rgba));
        font->drawString_scale(x + pad, cy, charW, charH, line.text.c_str(), true);
        cy += lineH;
    }
}

}  // namespace ap::overlay
