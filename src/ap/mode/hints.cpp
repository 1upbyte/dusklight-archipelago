// The Hints tab: hints involving this slot, asking for hints, setting their status.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static uint64_t hint_key(const Hint& h);
static const Hint* selected_hint();
static const char* hint_status_name(const Hint& h);
static std::string hint_label(const Hint& h);
static std::string hints_summary();
static std::string hint_details_rml();
static void refresh_hints_list(ModContext* ctx);
static bool status_locked(ModContext*, void*);
static void add_status_button(ModContext* ctx, UiElementHandle row, const char* label, int status);
static void ask_for_hint(const std::string& command, std::string& draft);

static uint64_t hint_key(const Hint& h) {
    return (static_cast<uint64_t>(h.findingPlayer) << 48) ^ static_cast<uint64_t>(h.location);
}

static const Hint* selected_hint() {
    for (const auto& h : g_hints) {
        if (hint_key(h) == g_selectedHint) {
            return &h;
        }
    }
    return nullptr;
}

static const char* hint_status_name(const Hint& h) {
    if (h.found) {
        return "found";
    }
    switch (h.status) {
    case 30: return "priority";
    case 20: return "avoid";
    case 10: return "no priority";
    default: return "unspecified";
    }
}

std::string player_label(int slot) {
    if (slot == g_client.slot()) {
        return "you";
    }
    const auto& name = g_client.playerName(slot);
    return name.empty() ? fmt::format("player {}", slot) : name;
}

static std::string hint_label(const Hint& h) {
    const int me = g_client.slot();
    const std::string item = g_client.itemName(h.item, h.receivingPlayer);
    const std::string where = g_client.locationName(h.location, h.findingPlayer);
    std::string label = h.receivingPlayer == me
        ? fmt::format("{}: {} ({})", item, where,
              h.findingPlayer == me ? "your world" : player_label(h.findingPlayer) + "'s world")
        : fmt::format("{}'s {}: {} (your world)", player_label(h.receivingPlayer), item, where);
    label += fmt::format("  [{}]", hint_status_name(h));
    return message_safe(label, 200);
}

static std::string hints_summary() {
    if (g_client.state() != State::Connected) {
        return "Connect to your room to see and ask for hints.";
    }
    const int cost = g_client.hintCost();
    std::string s = cost == 0 ? fmt::format("Hint points: {}. Hints are free in this room.",
                                    g_client.hintPoints())
                              : fmt::format("Hint points: {}. A hint costs {}.",
                                    g_client.hintPoints(), cost);
    const size_t open = std::ranges::count_if(g_hints, [](const Hint& h) { return !h.found; });
    s += fmt::format("\n{} hint{} still open.", open, open == 1 ? "" : "s");
    return s;
}

static std::string hint_details_rml() {
    const Hint* h = selected_hint();
    if (h == nullptr) {
        return R"(<div class="ap-quiet">Pick a hint to see it here. For hints on your own items you can set a priority, which shows for the player who has to find it.</div>)";
    }
    const int flags = h->itemFlags;
    const char* itemClass = (flags & 1) ? "ap-prog" : (flags & 2) ? "ap-useful" : (flags & 4) ? "ap-trap" : "ap-item";
    auto player = [](int slot) {
        return fmt::format(R"(<span class="{}">{}</span>)",
            slot == g_client.slot() ? "ap-me" : "ap-player",
            emoji::emojify(rml_escape(message_safe(player_label(slot), 60))));
    };
    std::string out = fmt::format(R"(<div class="ap-head">{}</div>)",
        rml_escape(h->found ? "Found" : "Hint"));
    out += fmt::format(R"(<div class="ap-msg"><span class="{}">{}</span> for {}</div>)", itemClass,
        rml_escape(message_safe(g_client.itemName(h->item, h->receivingPlayer), 120)),
        player(h->receivingPlayer));
    out += fmt::format(R"(<div class="ap-msg">at <span class="ap-loc">{}</span> in {} world</div>)",
        rml_escape(message_safe(g_client.locationName(h->location, h->findingPlayer), 120)),
        h->findingPlayer == g_client.slot() ? std::string{"your"} : player(h->findingPlayer) + "'s");
    if (!h->entrance.empty()) {
        out += fmt::format(R"(<div class="ap-msg">through <span class="ap-ent">{}</span></div>)",
            rml_escape(message_safe(h->entrance, 120)));
    }
    out += fmt::format(R"(<div class="ap-msg">Status: {}</div>)", hint_status_name(*h));
    return out;
}

static void refresh_hints_list(ModContext* ctx) {
    g_hintLabels.clear();
    g_hintLabels.reserve(g_hints.size());
    std::vector<UiListItem> items;
    items.reserve(g_hints.size());
    for (const auto& h : g_hints) {
        g_hintLabels.push_back(hint_label(h));
        UiListItem item = UI_LIST_ITEM_INIT;
        item.key = hint_key(h);
        item.label = g_hintLabels.back().c_str();
        items.push_back(item);
    }
    if (g_hintsList != 0) {
        svc_mng.ui->list_set_items(ctx, g_hintsList, items.data(), items.size());
    }
}


// Setting a priority is only for hints on our own items that nobody has found yet.
static bool status_locked(ModContext*, void*) {
    const Hint* h = selected_hint();
    return h == nullptr || h->found || h->receivingPlayer != g_client.slot() ||
           g_client.state() != State::Connected;
}

static void add_status_button(ModContext* ctx, UiElementHandle row, const char* label, int status) {
    UiControlDesc b = UI_CONTROL_DESC_INIT;
    b.kind = UI_CONTROL_BUTTON;
    b.label = label;
    b.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(status));
    b.on_pressed = [](ModContext*, void* ud) {
        if (const Hint* h = selected_hint(); h != nullptr) {
            g_client.updateHint(h->findingPlayer, h->location,
                static_cast<int>(reinterpret_cast<intptr_t>(ud)));
        }
    };
    b.is_disabled = status_locked;
    b.is_selected = [](ModContext*, void* ud) {
        const Hint* h = selected_hint();
        return h != nullptr && !h->found && h->status == static_cast<int>(reinterpret_cast<intptr_t>(ud));
    };
    svc_mng.ui->pane_add_control(ctx, row, &b, nullptr);
}

static void ask_for_hint(const std::string& command, std::string& draft) {
    std::string text = message_safe(draft, 200);
    std::erase(text, '\n');
    const auto first = text.find_first_not_of(' ');
    if (first != std::string::npos && g_client.state() == State::Connected) {
        g_client.say(command + text.substr(first));
        draft.clear();
    }
}

ModResult build_hints_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    g_hintsSummary = g_hintDetails = 0;
    g_hintsList = 0;
    svc_mng.ui->pane_add_text(ctx, left, hints_summary().c_str(), &g_hintsSummary);

    // No help_rml on this tab's controls: a control's help replaces the right pane, and that's
    // where the hint details and priority buttons live.
    svc_mng.ui->pane_add_section(ctx, left, "Ask for a hint");
    svc_mng.ui->pane_add_rml(ctx, left,
        R"(<div class="ap-quiet">Where one of your items is, or what's at one of your )"
        R"(locations. Each hint costs hint points.</div>)",
        nullptr);
    struct Ask {
        const char* label;
        const char* button;
        const char* command;
        std::string* draft;
    };
    static const Ask asks[] = {
        {"Item", "Hint this item", "!hint ", &g_hintItemDraft},
        {"Location", "Hint this location", "!hint_location ", &g_hintLocationDraft},
    };
    for (const auto& ask : asks) {
        UiControlDesc field = UI_CONTROL_DESC_INIT;
        field.kind = UI_CONTROL_STRING;
        field.label = ask.label;
        field.max_length = 200;
        field.get = get_str;
        field.set = set_str;
        field.user_data = ask.draft;
        field.string_set_mode = UI_STRING_SET_ON_CHANGE;
        field.is_disabled = not_connected;
        svc_mng.ui->pane_add_control(ctx, left, &field, nullptr);
        UiControlDesc go = UI_CONTROL_DESC_INIT;
        go.kind = UI_CONTROL_BUTTON;
        go.label = ask.button;
        go.user_data = const_cast<Ask*>(&ask);
        go.on_pressed = [](ModContext*, void* ud) {
            const auto* a = static_cast<const Ask*>(ud);
            ask_for_hint(a->command, *a->draft);
        };
        go.is_disabled = not_connected;
        svc_mng.ui->pane_add_control(ctx, left, &go, nullptr);
    }

    svc_mng.ui->pane_add_section(ctx, left, "Your hints");
    UiListDesc list = UI_LIST_DESC_INIT;
    list.on_pressed = [](ModContext* c, UiListHandle, uint64_t key, void*) {
        g_selectedHint = key;
        if (g_hintDetails != 0) {
            svc_mng.ui->elem_set_rml(c, g_hintDetails, hint_details_rml().c_str());
        }
    };
    list.is_selected = [](ModContext*, UiListHandle, uint64_t key, void*) {
        return key == g_selectedHint;
    };
    svc_mng.ui->pane_add_list(ctx, left, &list, &g_hintsList);
    refresh_hints_list(ctx);

    svc_mng.ui->pane_add_rml(ctx, right, hint_details_rml().c_str(), &g_hintDetails);
    UiRowDesc rowDesc = UI_ROW_DESC_INIT;
    rowDesc.wrap = true;
    UiElementHandle row = 0;
    svc_mng.ui->pane_add_row(ctx, right, &rowDesc, &row);
    add_status_button(ctx, row, "Priority", 30);
    add_status_button(ctx, row, "No priority", 10);
    add_status_button(ctx, row, "Avoid", 20);
    g_hintsShown = {g_hintsVersion, g_selectedHint, g_client.hintPoints()};
    return MOD_OK;
}

ModResult update_hints_tab(ModContext* ctx, void*, ModError*) {
    const std::tuple<uint64_t, uint64_t, int> now{g_hintsVersion, g_selectedHint,
        g_client.state() == State::Connected ? g_client.hintPoints() : -1};
    if (now == g_hintsShown) {
        return MOD_OK;
    }
    const bool listChanged = std::get<0>(now) != std::get<0>(g_hintsShown);
    g_hintsShown = now;
    if (g_hintsSummary != 0) {
        svc_mng.ui->elem_set_text(ctx, g_hintsSummary, hints_summary().c_str());
    }
    if (listChanged) {
        refresh_hints_list(ctx);
    }
    if (g_hintDetails != 0) {
        svc_mng.ui->elem_set_rml(ctx, g_hintDetails, hint_details_rml().c_str());
    }
    return MOD_OK;
}

}  // namespace ap::internal
