// Shared helpers: the debug log, toasts, config vars, save blobs, status text.

#include "internal.hpp"

namespace ap::internal {
namespace {

std::unordered_map<int, std::string> g_itemNames;

}  // namespace


// ---------------------------------------------------------------------------------------
// Helpers

// Flushed debug trail next to the mod's data; the host log buffers too much to debug with.
void ap_log(const std::string& line) {
    bool enabled = false;
    if (g_cfgDebugLog != 0) {
        svc_mng.config->get_bool(svc_mng.mod_ctx, g_cfgDebugLog, &enabled);
    }
    if (!enabled) {
        return;
    }
    const char* dir = nullptr;
    if (svc_mng.host->data_dir(svc_mng.mod_ctx, &dir) != MOD_OK || dir == nullptr) {
        return;
    }
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    const std::string stamped = fmt::format("{:02}:{:02}:{:02} {}", tm.tm_hour, tm.tm_min,
        tm.tm_sec, line);
    if (std::FILE* f = std::fopen((std::filesystem::path(dir) / "ap_debug.log").string().c_str(), "a")) {
        std::fputs(stamped.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

const std::string& item_name(int id) {
    if (g_itemNames.empty()) {
        for (const auto& node : LOAD_EMBED_YAML(RANDO_DATA_PATH "items.yaml")) {
            g_itemNames[node["Id"].as<int>()] = node["Name"].as<std::string>();
        }
    }
    static const std::string unknown = "an item";
    const auto it = g_itemNames.find(id);
    return it != g_itemNames.end() ? it->second : unknown;
}

void toast(const std::string& title, const std::string& body, const char* type,
    uint32_t ms) {
    // Server chat and refusal messages land here, so bound what we are willing to render.
    // No emoji images here: toasts don't render <img>. With the emoji font loaded, emoji in
    // the text show by themselves.
    const std::string t = rml_escape(message_safe(title, 80));
    const std::string b = rml_escape(message_safe(body, 400));
    UiToastDesc desc{sizeof(UiToastDesc)};
    desc.type = type;
    desc.title_rml = t.c_str();
    desc.body_rml = b.c_str();
    desc.duration_ms = ms;
    svc_mng.ui->push_toast(svc_mng.mod_ctx, &desc);
}


// One line of the Messages tab; `rml` must already be escaped.
void log_rml(std::string rml) {
    g_log.push_back(std::move(rml));
    while (g_log.size() > kLogLines) {
        g_log.pop_front();
    }
    ++g_logVersion;
}

std::string config_string(ConfigVarHandle var) {
    if (var == 0) {
        return {};
    }
    size_t len = 0;
    if (svc_mng.config->get_string(svc_mng.mod_ctx, var, nullptr, 0, &len) != MOD_OK || len == 0) {
        return {};
    }
    std::string s(len, '\0');
    svc_mng.config->get_string(svc_mng.mod_ctx, var, s.data(), s.size() + 1, &len);
    s.resize(std::strlen(s.c_str()));
    return s;
}

void write_conn() {
    const std::string s =
        json{{"server", g_conn.server}, {"slot", g_conn.slot}, {"password", g_conn.password}}.dump();
    svc_mng.save->set_blob(svc_mng.mod_ctx, kConnBlob, s.data(), s.size());
}

void write_state() {
    const std::string s = json{{"received", g_state.received}, {"goal", g_state.goal},
        {"seed", g_state.seed}, {"slot", g_state.slot}, {"death_link", g_state.deathLink},
        {"transform_anywhere", g_state.transformAnywhere},
        {"redeemed_dungeons", g_state.redeemedDungeons},
        {"redeemed_locations", g_state.redeemedLocations}}
                              .dump();
    svc_mng.save->set_blob(svc_mng.mod_ctx, kStateBlob, s.data(), s.size());
}

std::string status_line() {
    switch (g_client.state()) {
    case State::Connected:
        return "Connected to " + g_client.info().server + " as " + g_client.info().slot;
    case State::Connecting:
    case State::Handshaking:
        return "Connecting to " + g_client.info().server + "...";
    case State::Refused:
        return "Connection refused: " + g_client.lastError();
    default:
        return g_client.lastError().empty() ? "Disconnected" :
                                              "Disconnected: " + g_client.lastError();
    }
}


// A pasted key often brings a space or newline along.
std::string trim_key(std::string key) {
    std::erase_if(key, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
    return key;
}


// ---------------------------------------------------------------------------------------
// Per-frame work

bool in_gameplay() {
    return g_phase == Phase::Playing && !g_needsRegen && randomizer_IsActive() &&
           !playerIsOnTitleScreen() && dComIfGp_getPlayer(0) != nullptr;
}

void get_str(ModContext*, void* ud, UiControlValue* out) {
    out->string_value = static_cast<std::string*>(ud)->c_str();
}

void set_str(ModContext*, void* ud, const UiControlValue* v) {
    *static_cast<std::string*>(ud) = v->string_value != nullptr ? v->string_value : "";
}


// ---------------------------------------------------------------------------------------
// Overlay
//
// A small panel drawn in the game's own HUD pass (ap_overlay.cpp), after the HUD itself, so it
// shows during play and hides whenever the HUD does. Each part can be switched off from the
// Overlay tab; it all starts switched off.

bool cfg_on(ConfigVarHandle h) {
    bool v = false;
    if (h != 0) {
        svc_mng.config->get_bool(svc_mng.mod_ctx, h, &v);
    }
    return v;
}

int64_t cfg_int(ConfigVarHandle h, int64_t fallback) {
    int64_t v = fallback;
    if (h != 0) {
        svc_mng.config->get_int(svc_mng.mod_ctx, h, &v);
    }
    return v;
}

}  // namespace ap::internal
