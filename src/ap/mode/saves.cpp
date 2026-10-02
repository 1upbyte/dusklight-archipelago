// The new-save connection window and the save lifecycle (new save, load, reset).

#include "internal.hpp"

namespace ap::internal {
namespace {

void* g_fileSelect = nullptr;

}  // namespace

// Used only in this file; defined below.
static void press_connect(ModContext*, void*);
static bool connect_disabled(ModContext*, void*);
static ModResult build_new_save_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle, void*, ModError*);
static ModResult update_new_save_tab(ModContext* ctx, void*, ModError*);
static void new_save_window_closed(ModContext*, UiWindowHandle, void*);

static void press_connect(ModContext*, void*) {
    if (g_phase != Phase::NewSave) {
        return;
    }
    if (g_inSlot.empty()) {
        g_status = "Enter your slot (player) name.";
        return;
    }
    mDoAud_seStartMenu(Z2SE_SY_MENU_NEXT);
    g_conn = {g_inServer, g_inSlot, g_inPassword};
    if (g_cfgServer != 0) {
        svc_mng.config->set_string(svc_mng.mod_ctx, g_cfgServer, g_inServer.c_str());
        svc_mng.config->set_string(svc_mng.mod_ctx, g_cfgSlot, g_inSlot.c_str());
    }
    g_status = "Connecting...";
    g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
}

static bool connect_disabled(ModContext*, void*) {
    return g_phase != Phase::NewSave || g_client.state() == State::Connecting ||
           g_client.state() == State::Handshaking;
}

static ModResult build_new_save_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    auto string_row = [&](const char* label, std::string* target, const char* help) {
        UiControlDesc d = UI_CONTROL_DESC_INIT;
        d.kind = UI_CONTROL_STRING;
        d.label = label;
        d.help_rml = help;
        d.get = get_str;
        d.set = set_str;
        d.user_data = target;
        d.string_set_mode = UI_STRING_SET_ON_CHANGE;
        svc_mng.ui->pane_add_control(ctx, left, &d, nullptr);
    };
    svc_mng.ui->pane_add_section(ctx, left, "Archipelago");
    string_row("Server", &g_inServer, "Host and port, e.g. archipelago.gg:38281 or localhost:38281.");
    string_row("Slot name", &g_inSlot, "Your player name from your YAML.");
    string_row("Password", &g_inPassword, "Leave empty if the room has no password.");
    UiControlDesc b = UI_CONTROL_DESC_INIT;
    b.kind = UI_CONTROL_BUTTON;
    b.label = "Connect and start";
    b.on_pressed = press_connect;
    b.is_disabled = connect_disabled;
    svc_mng.ui->pane_add_control(ctx, left, &b, nullptr);
    g_statusText = 0;
    svc_mng.ui->pane_add_text(ctx, left, g_status.c_str(), &g_statusText);
    return MOD_OK;
}

static ModResult update_new_save_tab(ModContext* ctx, void*, ModError*) {
    if (g_statusText != 0) {
        static std::string shown;
        if (shown != g_status) {
            shown = g_status;
            svc_mng.ui->elem_set_text(ctx, g_statusText, g_status.c_str());
        }
    }
    return MOD_OK;
}

static void new_save_window_closed(ModContext*, UiWindowHandle, void*) {
    g_window = 0;
    g_statusText = 0;
    randomizer::ui::g_dialogSelectModeState = randomizer::ui::SelectReady;
    if (randomizer::ui::g_file_select_window_ctx.is_proceed) {
        return;
    }
    // Backed out: return the file-select menu to the data list, and drop the connection.
    if (auto* fs = static_cast<dFile_select_c*>(g_fileSelect)) {
        fs->headerTxtSet(0x43, 1, 0);
        fs->fileRecScaleAnmInitSet2(0.0f, 1.0f);
        fs->nameMoveAnmInitSet(0xd29, 0xd1f);
        fs->modoruTxtDispAnmInit(0);
        fs->mDataSelProc = dFile_select_c::DATASELPROC_NAME_TO_DATA_SELECT_MOVE;
    }
    g_client.disconnect();
    g_phase = Phase::Idle;
}

ModResult open_gate_window(void* fileSelect) {
    g_fileSelect = fileSelect;
    randomizer::ui::g_file_select_window_ctx.is_proceed = false;
    g_phase = Phase::NewSave;
    g_haveSlot = false;
    g_serverItems.clear();
    g_status = "Connect to your Archipelago room. Your seed is built from the server's data.";
    g_inServer = config_string(g_cfgServer);
    if (g_inServer.empty()) {
        g_inServer = "archipelago.gg:38281";
    }
    g_inSlot = config_string(g_cfgSlot);
    g_inPassword.clear();

    static UiTabDesc tab = UI_TAB_DESC_INIT;
    tab.title = "Archipelago";
    tab.build = build_new_save_tab;
    tab.update = update_new_save_tab;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.on_closed = new_save_window_closed;
    return svc_mng.ui->window_push(svc_mng.mod_ctx, &desc, &g_window);
}


// ---------------------------------------------------------------------------------------
// Save lifecycle

ModResult on_new_save(void* ud, ModError* err) {
    const ModResult r = randomizer::session::onNewSave(ud, err);
    if (r != MOD_OK) {
        return r;
    }
    g_state = {};
    g_state.seed = g_slotSeed;
    g_state.slot = g_conn.slot;
    g_state.transformAnywhere = g_transformAnywhereSlot;
    write_state();
    write_conn();
    g_loadedHash = randomizer_GetContext().mHash;
    g_needsRegen = false;
    g_outstanding = -1;
    g_redeemPending = 0;
    g_redeemedLocations.clear();
    g_phase = Phase::Playing;
    ap_log(fmt::format("new save created, hash {}, scan list {}", g_loadedHash, g_scan.size()));
    after_seed_activated();
    return MOD_OK;
}

ModResult on_save_loaded(void* ud, ModError* err) {
    g_outstanding = -1;
    g_redeemPending = 0;
    g_toSend.clear();
    Conn conn;
    SaveState state;
    const bool haveConn = read_blob(kConnBlob, conn);
    read_blob(kStateBlob, state);

    std::string hash;
    {
        size_t size = 0;
        if (svc_mng.save->get_blob(svc_mng.mod_ctx, kSeedHashBlob, nullptr, &size) == MOD_OK && size) {
            hash.resize(size);
            svc_mng.save->get_blob(svc_mng.mod_ctx, kSeedHashBlob, hash.data(), &size);
        }
    }
    g_loadedHash = hash;
    g_needsRegen = !seed_files_exist(hash);
    if (!g_needsRegen) {
        const ModResult r = randomizer::session::onSaveLoaded(ud, err);
        if (r != MOD_OK) {
            return r;
        }
        after_seed_activated();
    } else {
        randomizer::session::deactivateSeed();
    }

    const bool sameSession = g_client.state() == State::Connected && g_haveSlot &&
                             g_state.seed == state.seed && g_conn.slot == conn.slot;
    g_state = state;
    g_redeemedLocations = {g_state.redeemedLocations.begin(), g_state.redeemedLocations.end()};
    g_phase = Phase::Playing;
    if (!haveConn) {
        toast("Archipelago", "This save has no Archipelago connection info.", "warning");
        return MOD_OK;
    }
    g_conn = conn;
    if (!sameSession) {
        g_serverItems.clear();
        g_haveSlot = false;
        g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
    } else if (g_needsRegen && !g_lastSlotData.is_null()) {
        g_phase = Phase::Generating;
        start_generation(g_lastSlotData, g_conn.slot);
    }
    return MOD_OK;
}

ModResult on_game_reset(void*, ModError*) {
    g_phase = Phase::Idle;
    g_outstanding = -1;
    g_redeemPending = 0;
    g_redeemedLocations.clear();
    reset_tracker();
    return MOD_OK;
}

}  // namespace ap::internal
