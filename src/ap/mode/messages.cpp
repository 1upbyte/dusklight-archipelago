// The Messages tab: the room log, chat, the emoji picker and the emoji font.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static std::string log_rml_all();
static void send_chat(ModContext*, void*);
static ModResult build_emoji_picker(ModContext* ctx, UiElementHandle pane, void*, ModError*);

static std::string log_rml_all() {
    if (g_log.empty()) {
        return R"(<div class="ap-msg ap-quiet">Nothing yet. Messages from the room show up here.</div>)";
    }
    std::string out;
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it) {  // newest first
        out += R"(<div class="ap-msg">)" + *it + "</div>";
    }
    return out;
}


// The game's UI (RmlUi) has no emoji glyphs, and the mod API has no way to add fonts. RmlUi's
// own LoadFontFace is in the game's symbol manifest though, so resolve it by name (the way
// other mods reach host functions) and register Twemoji as a fallback face. It's a COLR font,
// which the game's FreeType renders in color. If anything here fails, emoji stay as images in
// the message log and as :codes: in the picker.
bool load_emoji_font() {
    static int state = 0;  // 0 untried, 1 loaded, 2 failed
    if (state != 0) {
        return state == 1;
    }
    state = 2;
    using LoadFontFaceFn = bool (*)(const std::string& path, bool fallback, uint16_t weight, int face);
    static constexpr const char* kNames[] = {
        // bool Rml::LoadFontFace(const String&, bool fallback_face, Style::FontWeight, int)
        "?LoadFontFace@Rml@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
        "_NW4FontWeight@Style@1@H@Z",
        "_ZN3Rml12LoadFontFaceERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEbNS_5Style1"
        "0FontWeightEi",
        "_ZN3Rml12LoadFontFaceERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE"
        "bNS_5Style10FontWeightEi",
    };
    void* address = nullptr;
    for (const char* name : kNames) {
        if (svc_mng.hook->resolve(svc_mng.mod_ctx, name, &address, nullptr) == MOD_OK &&
            address != nullptr) {
            break;
        }
        address = nullptr;
    }
    if (address == nullptr) {
        ap_log("emoji font: Rml::LoadFontFace not found in the symbol manifest");
        return false;
    }
    ResourceBuffer font = RESOURCE_BUFFER_INIT;
    if (svc_mng.resource->load(svc_mng.mod_ctx, "fonts/Twemoji.Mozilla.ttf", &font) != MOD_OK ||
        font.data == nullptr) {
        ap_log("emoji font: res/fonts/Twemoji.Mozilla.ttf missing from the bundle");
        return false;
    }
    // LoadFontFace reads from a file, so keep a copy next to the mod's other data.
    namespace fs = std::filesystem;
    const fs::path path = randomizer::paths::GetRandomizerPath() / "archipelago" / "Twemoji.Mozilla.ttf";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (!fs::exists(path, ec) || fs::file_size(path, ec) != font.size) {
        std::ofstream(path, std::ios::binary)
            .write(static_cast<const char*>(font.data), static_cast<std::streamsize>(font.size));
    }
    svc_mng.resource->free(svc_mng.mod_ctx, &font);
    const bool ok = reinterpret_cast<LoadFontFaceFn>(address)(path.generic_string(), true, 0, 0);
    ap_log(ok ? "emoji font: loaded" : "emoji font: LoadFontFace refused the file");
    state = ok ? 1 : 2;
    return ok;
}

bool not_connected(ModContext*, void*) {
    return g_client.state() != State::Connected;
}


// Chat and server commands. The field only edits the draft; Send sends it, so moving focus
// away (to the emoji picker, say) never sends half a message.
static void send_chat(ModContext*, void*) {
    std::string text = message_safe(g_chatDraft, 400);
    std::erase(text, '\n');
    const auto first = text.find_first_not_of(' ');
    if (first != std::string::npos && g_client.state() == State::Connected) {
        g_client.say(emoji::shortcodes_to_unicode(text.substr(first)));
        g_chatDraft.clear();
    }
}

static ModResult build_emoji_picker(ModContext* ctx, UiElementHandle pane, void*, ModError*) {
    svc_mng.ui->pane_add_section(ctx, pane, "Emoji");
    UiRowDesc rowDesc = UI_ROW_DESC_INIT;
    rowDesc.wrap = true;
    UiElementHandle row = 0;
    svc_mng.ui->pane_add_row(ctx, pane, &rowDesc, &row);
    static std::vector<std::string> labels;
    labels.clear();
    labels.reserve(emoji::picker().size());
    for (const auto& e : emoji::picker()) {
        // With the emoji font the button is just the emoji; without it, an image behind the name.
        labels.push_back(g_emojiFont ? emoji::glyph(e.file) : std::string{e.name});
        UiControlDesc b = UI_CONTROL_DESC_INIT;
        b.kind = UI_CONTROL_BUTTON;
        b.label = labels.back().c_str();
        b.tooltip = e.name;
        b.user_data = const_cast<emoji::PickerEmoji*>(&e);
        b.on_pressed = [](ModContext*, void* ud) {
            const auto* pick = static_cast<const emoji::PickerEmoji*>(ud);
            if (!g_chatDraft.empty() && g_chatDraft.back() != ' ') {
                g_chatDraft += ' ';
            }
            g_chatDraft += g_emojiFont ? emoji::glyph(pick->file) + " "
                                       : fmt::format(":{}: ", pick->name);
        };
        UiElementHandle elem = 0;
        svc_mng.ui->pane_add_control(ctx, row, &b, &elem);
        if (elem != 0) {
            svc_mng.ui->elem_set_class(ctx, elem, g_emojiFont ? "ap-emoji-glyph" : "ap-emoji-btn", true);
            if (!g_emojiFont) {
                svc_mng.ui->elem_set_class(ctx, elem, fmt::format("ap-e-{}", e.file).c_str(), true);
            }
        }
    }
    return MOD_OK;
}

ModResult build_messages_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    svc_mng.ui->pane_add_section(ctx, left, "Chat");
    UiControlDesc say = UI_CONTROL_DESC_INIT;
    say.kind = UI_CONTROL_STRING;
    say.label = "Message";
    say.max_length = 400;
    say.get = get_str;
    say.set = set_str;
    say.user_data = &g_chatDraft;
    say.string_set_mode = UI_STRING_SET_ON_CHANGE;
    say.is_disabled = not_connected;
    svc_mng.ui->pane_add_control(ctx, left, &say, nullptr);

    UiControlDesc send = UI_CONTROL_DESC_INIT;
    send.kind = UI_CONTROL_BUTTON;
    send.label = "Send";
    send.on_pressed = send_chat;
    send.is_disabled = not_connected;
    svc_mng.ui->pane_add_control(ctx, left, &send, nullptr);
    // A group button has to sit directly in the tab's left pane; the host rejects one in a row.
    UiGroupDesc picker = UI_GROUP_DESC_INIT;
    picker.label = "Emoji";
    picker.build = build_emoji_picker;
    svc_mng.ui->pane_add_group(ctx, left, right, &picker, nullptr);
    // Guidance as text rather than help_rml: a control's help would replace the picker.
    svc_mng.ui->pane_add_rml(ctx, left,
        R"(<div class="ap-quiet">Chat with the room, or send a command such as !remaining or )"
        R"(!help. Codes like :joy: go out as the emoji.</div>)",
        nullptr);

    svc_mng.ui->pane_add_section(ctx, left, "Room");
    g_logElem = 0;
    svc_mng.ui->pane_add_rml(ctx, left, log_rml_all().c_str(), &g_logElem);
    g_logShown = g_logVersion;
    return MOD_OK;
}

ModResult update_messages_tab(ModContext* ctx, void*, ModError*) {
    if (g_logElem != 0 && g_logShown != g_logVersion) {
        g_logShown = g_logVersion;
        svc_mng.ui->elem_set_rml(ctx, g_logElem, log_rml_all().c_str());
    }
    return MOD_OK;
}

}  // namespace ap::internal
