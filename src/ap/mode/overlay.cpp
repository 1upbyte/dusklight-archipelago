// The in-game overlay: what it shows, its settings and its tab. Drawing is ap_overlay.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static uint32_t overlay_accent();
static std::vector<overlay::Line> overlay_lines();

static uint32_t overlay_accent() {
    switch (g_client.state()) {
    case State::Connected: return kOvGreen;
    case State::Connecting:
    case State::Handshaking: return kOvYellow;
    default: return kOvRed;
    }
}

static std::vector<overlay::Line> overlay_lines() {
    using overlay::ascii_only;
    using overlay::Line;
    constexpr size_t kChars = 34;
    std::vector<Line> out;
    auto section = [&] {
        if (!out.empty()) {
            out.push_back(Line::divider());
        }
    };
    const auto now = std::chrono::steady_clock::now();
    const int64_t keep = cfg_int(g_ovKeep, 30);
    auto fresh = [&](const Recent& r) {
        return keep <= 0 || now - r.at < std::chrono::seconds(keep);
    };

    if (cfg_on(g_ovStatus)) {
        const char* state = g_client.state() == State::Connected ? "connected"
                            : g_client.state() == State::Connecting ||
                                      g_client.state() == State::Handshaking
                                ? "connecting"
                                : "offline";
        out.push_back(Line::text("Archipelago", kOvText, state, overlay_accent()));
    }
    if (cfg_on(g_ovChecks) && g_haveSlot) {
        const auto n = region_counts(-1);
        section();
        out.push_back(Line::text("Checks", kOvText, fmt::format("{} / {}", n.done, n.total), kOvText));
        out.push_back(Line::bar(n.total == 0 ? 0.0f : static_cast<float>(n.done) / n.total, kOvGreen));
        if (g_logic && g_reachInputs != SIZE_MAX) {
            out.push_back(Line::text("In logic now", kOvDim, std::to_string(n.inLogic),
                n.inLogic > 0 ? kOvGreen : kOvDim, true));
        }
    }
    if (cfg_on(g_ovItems)) {
        int64_t left = std::clamp<int64_t>(cfg_int(g_ovItemCount, 3), 1, 8);
        bool first = true;
        for (auto it = g_recentItems.rbegin(); it != g_recentItems.rend() && left > 0; ++it) {
            if (!fresh(*it)) {
                continue;
            }
            if (first) {
                section();
                first = false;
            }
            out.push_back(Line::text(ascii_only(it->item, kChars), kOvBlue,
                ascii_only(it->from, 16), kOvDim));
            --left;
        }
    }
    if (cfg_on(g_ovHints) && g_client.state() == State::Connected) {
        const int me = g_client.slot();
        // Anything that concerns us: our items wherever they are, and anyone's items in our world.
        std::vector<const Hint*> open;
        for (const auto& h : g_hints) {  // sorted, priority first
            if ((h.receivingPlayer == me || h.findingPlayer == me) && !h.found) {
                open.push_back(&h);
            }
        }
        if (!open.empty()) {
            section();
            out.push_back(Line::text("Hints", kOvText, fmt::format("{} open", open.size()), kOvYellow));
            const auto shown = static_cast<size_t>(std::clamp<int64_t>(cfg_int(g_ovHintCount, 3), 1, 8));
            for (size_t k = 0; k < open.size() && k < shown; ++k) {
                const Hint* h = open[k];
                // Right column: whose world our item is in, or whose item it is.
                const std::string who = h->receivingPlayer == me
                    ? (h->findingPlayer == me ? "your world"
                                              : ascii_only(player_label(h->findingPlayer), 16))
                    : "for " + ascii_only(player_label(h->receivingPlayer), 12);
                out.push_back(Line::text(ascii_only(g_client.itemName(h->item, h->receivingPlayer), kChars),
                    h->status == 30 ? kOvYellow : kOvText, who, kOvDim, true));
                out.push_back(Line::text(
                    ascii_only(g_client.locationName(h->location, h->findingPlayer), kChars + 6),
                    kOvDim, {}, kOvDim, true));
            }
        }
    }
    if (cfg_on(g_ovChat)) {
        std::vector<const Recent*> lines;
        for (auto it = g_recentChat.rbegin(); it != g_recentChat.rend() && lines.size() < 3; ++it) {
            if (fresh(*it)) {
                lines.push_back(&*it);
            }
        }
        if (!lines.empty()) {
            section();
        }
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {  // oldest of them first
            out.push_back(Line::text(ascii_only((*it)->item, kChars + 6), kOvDim, {}, kOvDim, true));
        }
    }
    if (cfg_on(g_ovDeathLink) && death_link_on()) {
        section();
        out.push_back(Line::text("Death link", kOvRed, "on", kOvRed, true));
    }
    return out;
}

void post_meter2_draw(ModContext*, void*, void*, void*) {
    // Not dScnPly_c::isPause(): that's the hit-stop timer (set for a few frames when an attack
    // lands), and hiding the panel for it made it flicker. The panel only draws boxes and text,
    // so a frozen frame is fine; only the pause flag (menus) hides it.
    if (!cfg_on(g_ovEnabled) || !in_gameplay() || dComIfGp_isPauseFlag()) {
        return;
    }
    overlay::Layout layout;
    layout.corner = static_cast<overlay::Corner>(std::clamp<int64_t>(cfg_int(g_ovCorner, 3), 0, 5));
    layout.offsetX = static_cast<float>(cfg_int(g_ovX, 12));
    layout.offsetY = static_cast<float>(cfg_int(g_ovY, 0));
    layout.scale = static_cast<float>(cfg_int(g_ovScale, 100)) / 100.0f;
    layout.backgroundAlpha =
        static_cast<uint8_t>(std::clamp<int64_t>(cfg_int(g_ovOpacity, 60), 0, 100) * 255 / 100);
    layout.accent = overlay_accent();
    overlay::draw_panel(overlay_lines(), layout);
}

void register_overlay_vars() {
    auto reg_bool = [](const char* name, bool def, ConfigVarHandle& out) {
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = name;
        d.type = CONFIG_VAR_BOOL;
        d.default_bool = def;
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &out);
    };
    auto reg_int = [](const char* name, int64_t def, ConfigVarHandle& out) {
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = name;
        d.type = CONFIG_VAR_INT;
        d.default_int = def;
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &out);
    };
    reg_bool("overlay", false, g_ovEnabled);
    reg_int("overlayCorner", 3, g_ovCorner);
    reg_int("overlayX", 12, g_ovX);
    reg_int("overlayY", 0, g_ovY);
    reg_int("overlayScale", 100, g_ovScale);
    reg_int("overlayBackground", 60, g_ovOpacity);
    reg_bool("overlayStatus", true, g_ovStatus);
    reg_bool("overlayChecks", true, g_ovChecks);
    reg_bool("overlayItems", true, g_ovItems);
    reg_bool("overlayHints", true, g_ovHints);
    reg_bool("overlayChat", true, g_ovChat);
    reg_bool("overlayDeathLink", true, g_ovDeathLink);
    reg_int("overlayItemCount", 3, g_ovItemCount);
    reg_int("overlayHintCount", 3, g_ovHintCount);
    reg_int("overlayKeepSeconds", 30, g_ovKeep);
}


// Overlay tab: every control is bound straight to its config var.
void add_bound(ModContext* ctx, UiElementHandle pane, UiControlKind kind, const char* label,
    ConfigVarHandle var, const char* help, int64_t min, int64_t max,
    int64_t step, const char* suffix) {
    UiControlDesc d = UI_CONTROL_DESC_INIT;
    d.kind = kind;
    d.label = label;
    d.help_rml = help;
    d.binding = UI_BINDING_CONFIG_VAR;
    d.config_var = var;
    d.min = min;
    d.max = max;
    d.step = step;
    d.suffix = suffix;
    if (kind == UI_CONTROL_SELECT) {
        static const char* const corners[] = {"Top left", "Top right", "Middle left",
            "Middle right", "Bottom left", "Bottom right"};
        d.options = corners;
        d.option_count = std::size(corners);
    }
    svc_mng.ui->pane_add_control(ctx, pane, &d, nullptr);
}

ModResult build_overlay_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle, void*, ModError*) {
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Show overlay", g_ovEnabled,
        "A small panel of Archipelago info on screen while you play. It hides whenever the "
        "game's own HUD does.");
    svc_mng.ui->pane_add_section(ctx, left, "Placement");
    add_bound(ctx, left, UI_CONTROL_SELECT, "Position", g_ovCorner);
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Horizontal offset", g_ovX,
        "How far the panel sits from its side of the screen.", 0, 600, 5);
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Vertical offset", g_ovY,
        "How far it sits from the top or bottom. For the middle positions this moves it down, or "
        "up below zero.",
        -400, 400, 5);
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Size", g_ovScale, nullptr, 50, 200, 10, "%");
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Background", g_ovOpacity, nullptr, 0, 100, 10, "%");
    svc_mng.ui->pane_add_section(ctx, left, "Show");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Connection status", g_ovStatus);
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Checks and logic", g_ovChecks,
        "Checks done, how many are in logic now, and a progress bar.");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Recent items", g_ovItems,
        "Items you just received, and who sent them.");
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Recent items shown", g_ovItemCount, nullptr, 1, 8);
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Hints", g_ovHints,
        "Open hints that concern you: your items wherever they are, and anyone's items in your "
        "world. Priority hints come first.");
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Hints shown", g_ovHintCount, nullptr, 1, 8);
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Chat", g_ovChat, "The last few chat messages.");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Death link", g_ovDeathLink,
        "A reminder while death link is on.");
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Fade items and chat after", g_ovKeep,
        "0 keeps them until newer ones push them out.", 0, 300, 5, " s");

    return MOD_OK;
}

}  // namespace ap::internal
