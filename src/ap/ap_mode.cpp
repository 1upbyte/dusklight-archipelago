#include "ap_mode.hpp"

#include "ap_client.hpp"
#include "ap_overlay.hpp"
#include "ap_tracker.hpp"
#include "emoji.hpp"
#include "data_version.hpp"
#include "text_safe.hpp"

#include "../../generator/randomizer.hpp"
#include "../../generator/utility/yaml.hpp"
#include "../item_ids.h"
#include "../paths.hpp"
#include "../randomizer_context.hpp"
#include "../session.hpp"
#include "../stages.h"
#include "../tools.h"
#include "../ui/rando_config.hpp"
#include "../verify_item_functions.h"

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_gnd.h"
#include "d/actor/d_a_demo_item.h"
#include "d/actor/d_a_obj_item.h"
#include "d/d_com_inf_game.h"
#include "d/d_file_select.h"
#include "d/d_kankyo.h"
#include "d/d_meter2_draw.h"
#include "d/d_msg_flow.h"
#include "d/d_s_play.h"
#include "d/d_stage.h"
#include "m_Do/m_Do_audio.h"

#include <mods/items.h>
#include <mods/svc/flow.hpp>
#include <mods/svc/hook.hpp>
#include <mods/svc/log.hpp>

#include <yaml-cpp/yaml.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

RandomizerContext WriteSeedData(randomizer::logic::world::World* world);

namespace ap {
namespace {

using randomizer::session::svc_mng;

constexpr const char* kGameModeId = "archipelago";
constexpr const char* kConnBlob = "ap_conn";
constexpr const char* kStateBlob = "ap_state";
constexpr const char* kSeedHashBlob = "seed_hash";  // written by the randomizer session
constexpr int kSlotDataVersion = 1;
constexpr uint8_t kApItem = dItemNo_Randomizer_NOENTRY_220_e;  // "Archipelago Item" (0xDC)
constexpr uint16_t kApItemDonorMessage = 120;  // Foolish Item get text; overridden while armed
constexpr int64_t kItemIdBase = 0x54500000;

// ---------------------------------------------------------------------------------------
// State

Client g_client;

void after_seed_activated();
void apply_death_link_tags();

struct Conn {
    std::string server = "archipelago.gg:38281";
    std::string slot;
    std::string password;
};

struct SaveState {
    int received = 0;     // server item index delivered into this save
    bool goal = false;
    std::string seed;     // AP seed name this save belongs to
    std::string slot;
    int deathLink = -1;   // -1 follow the YAML, 0 off, 1 on (toggled from the Archipelago tab)
    bool transformAnywhere = false;  // the slot's Logic Transform Anywhere, kept for offline play
};

enum class Phase {
    Idle,           // title / no AP save loaded
    NewSave,        // new-save window open, not yet generated
    Generating,     // seed generation thread running
    AwaitNewSave,   // generated; waiting for the host to create the save
    Playing,        // an AP save is loaded
};

Phase g_phase = Phase::Idle;
Conn g_conn;
SaveState g_state;
bool g_needsRegen = false;       // save loaded but its seed files are missing on this machine
std::string g_loadedHash;

// Slot data
bool g_haveSlot = false;
std::string g_slotSeed;
std::unordered_map<std::string, int64_t> g_locationIds;       // location name -> AP id
std::unordered_map<std::string, std::string> g_apItemText;    // location name -> get text
std::unordered_map<std::string, std::string> g_expected;      // location name -> item AP expects
std::unordered_map<std::string, std::vector<std::string>> g_checkToLocations;

struct ScanLoc {
    std::string name;
    int64_t id;
    YAML::Node meta;
};
std::vector<ScanLoc> g_scan;
size_t g_scanPos = 0;
std::unordered_set<int64_t> g_checked;  // known to the server (sent or reported back)
std::vector<int64_t> g_toSend;

// Items
std::vector<NetworkItem> g_serverItems;
int g_outstanding = -1;
std::string g_outstandingDesc;

// New-save UI
UiWindowHandle g_window = 0;
UiElementHandle g_statusText = 0;
std::string g_status = "Enter your Archipelago connection, then press Connect.";

// Generation thread
std::atomic<int> g_genStatus{0};  // 0 idle, 1 running, 2 ok, 3 failed
std::mutex g_genMutex;
std::string g_genHash;
std::string g_genError;

// AP item text
std::string g_armedText;
int g_armedFrames = 0;
std::string g_lastResolvedApLocation;
std::vector<mods::flow::MessageOverride> g_textOverrides;
ItemCheckHandle g_resolver = 0;
ItemGiveHandle g_observer = 0;

// Menu tab
UiMenuTabHandle g_menuTab = 0;
UiWindowHandle g_statusWindow = 0;
UiElementHandle g_statusWindowText = 0;

// Death link (see "Death link" below)
bool g_deathLinkSlot = false;   // the YAML's choice, from slot_data
bool g_transformAnywhereSlot = false;  // the slot's Logic Transform Anywhere, from slot_data
bool g_deathSent = false;       // this death is handled; cleared once Link is alive again
double g_lastDeathTime = 0.0;   // time on the death we sent, to recognise its echo
int g_killFrames = 0;           // > 0: a death now is the one we were sent, not a new one
std::optional<std::string> g_pendingDeath;

// Tracker (see "Tracker" below; the logic itself is in ap_tracker.cpp)
struct TrackerCheck {
    std::string name;
    int64_t id = 0;
    int region = 0;  // index into tracker_regions()
};
std::vector<TrackerCheck> g_trackerChecks;  // this slot's checks, in locations.yaml order
std::vector<std::string> g_unshuffled;      // slot_data "unshuffled_dungeons"
bool g_unshuffledKnown = false;             // slot data from before 1.6.0 doesn't say
std::shared_ptr<tracker::Logic> g_logic;
std::string g_logicKey;                     // seed-slot the logic is for (or being built for)
bool g_logicBuilding = false;
std::string g_logicError;
std::unordered_set<std::string> g_inLogic;  // reachable checks, done or not
size_t g_reachInputs = SIZE_MAX;            // the inputs g_inLogic (or the search running) is for
bool g_reachBusy = false;
uint64_t g_trackerEpoch = 0;                // bumped on reset: late worker results are dropped
uint64_t g_trackerVersion = 1;              // bumped whenever what the tracker shows changes
void reset_tracker();

// Message log (the Messages tab)
constexpr size_t kLogLines = 200;
std::deque<std::string> g_log;              // RML, oldest first
uint64_t g_logVersion = 1;
std::string g_chatDraft;

// Hints (the Hints tab): the server's list for our slot, kept current by SetNotify.
std::vector<Hint> g_hints;          // sorted: open first, by status; found last
uint64_t g_hintsVersion = 1;
uint64_t g_selectedHint = 0;        // hint_key() of the hint shown in the details pane
std::string g_hintItemDraft;
std::string g_hintLocationDraft;

// Overlay (see "Overlay" below): settings are config vars, so they persist and bind to the tab
struct Recent {
    std::string text;
    std::chrono::steady_clock::time_point at;
};
std::deque<Recent> g_recentItems;
std::deque<Recent> g_recentChat;
ConfigVarHandle g_ovEnabled = 0, g_ovCorner = 0, g_ovX = 0, g_ovY = 0, g_ovScale = 0,
                g_ovOpacity = 0, g_ovStatus = 0, g_ovChecks = 0, g_ovItems = 0, g_ovHints = 0,
                g_ovChat = 0, g_ovDeathLink = 0, g_ovItemCount = 0, g_ovKeep = 0;

// Config
ConfigVarHandle g_cfgServer = 0;
ConfigVarHandle g_cfgSlot = 0;
ConfigVarHandle g_cfgModelScale = 0;
ConfigVarHandle g_cfgDebugLog = 0;

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

std::unordered_map<int, std::string> g_itemNames;

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

void toast(const std::string& title, const std::string& body, const char* type = nullptr,
    uint32_t ms = 0) {
    // Server chat and refusal messages land here, so bound what we are willing to render.
    const std::string t = emoji::emojify(rml_escape(message_safe(title, 80)));
    const std::string b = emoji::emojify(rml_escape(message_safe(body, 400)));
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

template <typename T>
bool read_blob(const char* name, T& out) {
    size_t size = 0;
    if (svc_mng.save->get_blob(svc_mng.mod_ctx, name, nullptr, &size) != MOD_OK || size == 0) {
        return false;
    }
    std::string buf(size, '\0');
    if (svc_mng.save->get_blob(svc_mng.mod_ctx, name, buf.data(), &size) != MOD_OK) {
        return false;
    }
    json j = json::parse(buf, nullptr, false);
    if (j.is_discarded()) {
        return false;
    }
    out = T{};
    if constexpr (std::is_same_v<T, Conn>) {
        out.server = j.value("server", out.server);
        out.slot = j.value("slot", "");
        out.password = j.value("password", "");
    } else {
        out.received = j.value("received", 0);
        out.goal = j.value("goal", false);
        out.seed = j.value("seed", "");
        out.slot = j.value("slot", "");
        out.deathLink = j.value("death_link", -1);
        out.transformAnywhere = j.value("transform_anywhere", false);
    }
    return true;
}

void write_conn() {
    const std::string s =
        json{{"server", g_conn.server}, {"slot", g_conn.slot}, {"password", g_conn.password}}.dump();
    svc_mng.save->set_blob(svc_mng.mod_ctx, kConnBlob, s.data(), s.size());
}

void write_state() {
    const std::string s = json{{"received", g_state.received}, {"goal", g_state.goal},
        {"seed", g_state.seed}, {"slot", g_state.slot}, {"death_link", g_state.deathLink},
        {"transform_anywhere", g_state.transformAnywhere}}
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

// ---------------------------------------------------------------------------------------
// Slot data -> lookup tables

struct TrackerRegion {
    const char* label;
    std::vector<std::string> categories;
    bool dungeon;
};

// The randomizer tracker's own groups (src/ui/rando_config.cpp), plus "Other".
const std::vector<TrackerRegion>& tracker_regions() {
    static const std::vector<TrackerRegion> regions = {
        {"Ordon", {"Ordona Province"}, false},
        {"Faron", {"Faron Province", "Faron Woods", "Hyrule Field - Faron Province", "Sacred Grove"},
            false},
        {"Eldin", {"Eldin Province", "Hyrule Field - Eldin", "Hyrule Field - Eldin Province",
                      "Eldin Lantern Cave", "Eldin Stockcave", "Death Mountain", "Kakariko Village",
                      "Kakariko Graveyard"},
            false},
        {"Lanayru", {"Lanayru Province", "Hyrule Field - Lanayru", "Hyrule Field - Lanayru Province",
                        "Castle Town", "Fishing Hole", "Lake Hylia", "Lake Lantern Cave",
                        "Upper Zoras River", "Zoras Domain"},
            false},
        {"Gerudo Desert", {"Gerudo Desert", "Bulblin Camp", "Mirror Chamber", "Cave Of Ordeals"}, false},
        {"Snowpeak", {"Snowpeak Province", "Snowpeak"}, false},
        {"Forest Temple", {"Forest Temple"}, true},
        {"Goron Mines", {"Goron Mines"}, true},
        {"Lakebed Temple", {"Lakebed Temple"}, true},
        {"Arbiter's Grounds", {"Arbiters Grounds"}, true},
        {"Snowpeak Ruins", {"Snowpeak Ruins"}, true},
        {"Temple of Time", {"Temple of Time"}, true},
        {"City in the Sky", {"City in the Sky"}, true},
        {"Palace of Twilight", {"Palace of Twilight"}, true},
        {"Hyrule Castle", {"Hyrule Castle"}, true},
        {"Other", {}, false},
    };
    return regions;
}

// Dungeons first, so a dungeon's checks never land in the province around it.
int region_of(const YAML::Node& categories) {
    const auto& regions = tracker_regions();
    auto has = [&](const std::string& c) {
        for (const auto& n : categories) {
            if (n.as<std::string>() == c) {
                return true;
            }
        }
        return false;
    };
    for (const bool dungeons : {true, false}) {
        for (size_t i = 0; i < regions.size(); ++i) {
            if (regions[i].dungeon == dungeons &&
                std::ranges::any_of(regions[i].categories, has)) {
                return static_cast<int>(i);
            }
        }
    }
    return static_cast<int>(regions.size()) - 1;
}

void build_check_map() {
    g_checkToLocations.clear();
    g_scan.clear();
    g_scanPos = 0;
    g_trackerChecks.clear();
    const auto locations = LOAD_EMBED_YAML(RANDO_DATA_PATH "locations.yaml");
    auto add = [](const std::string& check, const std::string& loc) {
        g_checkToLocations[check].push_back(loc);
    };
    for (const auto& node : locations) {
        const std::string name = node["Name"].as<std::string>();
        const auto idIt = g_locationIds.find(name);
        if (idIt == g_locationIds.end()) {
            continue;  // not an AP location in this slot (vanilla-locked or removed)
        }
        const YAML::Node meta = node["Metadata"];
        g_scan.push_back({name, idIt->second, meta});
        g_trackerChecks.push_back({name, idIt->second, region_of(node["Categories"])});
        if (!meta.IsMap()) {
            continue;
        }
        auto stage = [](const YAML::Node& n) -> std::string {
            const int s = n["Stage"].as<int>();
            return (s >= 0 && s < static_cast<int>(std::size(allStages))) ? allStages[s] : "";
        };
        for (const auto& c : meta["Chest"]) {
            add(fmt::format("chest:{}:{}", stage(c), c["Tbox Id"].as<int>()), name);
        }
        for (const auto& c : meta["Poe"]) {
            add(fmt::format("poe:{}:{}", stage(c), c["Flag"].as<int>()), name);
        }
        for (const auto& c : meta["Freestanding Item"]) {
            const int flag = c["Flag"].as<int>();
            add(fmt::format("freestanding:{}:{}", stage(c), flag), name);
            if (flag == 0x9F) {
                add(fmt::format("boss:{}", stage(c)), name);
            }
        }
        for (const auto& c : meta["Sky Character"]) {
            add(fmt::format("sky:{}:{}", stage(c), c["Room"].as<int>()), name);
        }
        for (const auto& c : meta["Golden Wolf"]) {
            add(fmt::format("golden_wolf:{}", c["Flag"].as<int>()), name);
        }
        for (const auto& c : meta["Bug Reward"]) {
            add(fmt::format("bug:{}", c["Item Id"].as<int>()), name);
        }
        for (const auto& c : meta["Shop"]) {
            add(fmt::format("shop:{}:{}:{}", stage(c), c["Room"].as<int>(), c["Item"].as<int>()), name);
        }
        for (const auto& c : meta["Name Lookup"]) {
            add(nameLookupOverride(c.as<std::string>()), name);
        }
    }
}

std::vector<std::string> locations_for_check(const char* check) {
    if (check == nullptr) {
        return {};
    }
    if (const auto it = g_checkToLocations.find(check); it != g_checkToLocations.end()) {
        return it->second;
    }
    // Ook drops the Gale Boomerang as a freestanding item in his own stage
    const std::string ook = fmt::format("freestanding:{}:", allStages[Ook]);
    if (std::strncmp(check, ook.c_str(), ook.size()) == 0) {
        return {"Forest Temple Gale Boomerang"};
    }
    return {};
}

bool load_slot_data(const json& slotData, std::string& err) {
    if (slotData.value("version", 0) != kSlotDataVersion) {
        err = "This slot was generated with an incompatible apworld version.";
        return false;
    }
    // The apworld fingerprints the logic data it generated from. A different fingerprint here
    // means the mod would rebuild this seed with different logic: items could end up behind
    // requirements Archipelago never thought they were behind. Refuse it loudly instead.
    const json theirs = slotData.value("data_version", json());
    if (theirs.is_number_integer() && theirs.get<uint32_t>() != data_version()) {
        err = "This multiworld was generated with a different version of the Twilight Princess "
              "(Dusklight) apworld than this mod. Update both to the same release.";
        ap_log(fmt::format("data version mismatch: seed {} vs mod {}", theirs.get<uint32_t>(),
            data_version()));
        return false;
    }
    g_locationIds.clear();
    g_apItemText.clear();
    for (const auto& [name, id] : slotData.value("location_ids", json::object()).items()) {
        g_locationIds[name] = id.get<int64_t>();
    }
    g_expected.clear();
    for (const auto& [loc, v] : slotData.value("placements", json::object()).items()) {
        g_expected[loc] = v.is_object() ? fmt::format("{} ({})", v.value("name", "?"),
                                              v.value("player", "?"))
                                        : v.get<std::string>();
        if (v.is_object()) {
            std::string who = message_safe(v.value("player", "someone"), 40);
            std::string what = message_safe(v.value("name", "item"), 60);
            if (who.empty()) {
                who = "someone";
            }
            if (what.empty()) {
                what = "an item";
            }
            g_apItemText[loc] = fmt::format("You found {}'s\n{}!", who, what);
        }
    }
    g_slotSeed = slotData.value("seed", "");
    build_check_map();
    g_unshuffled.clear();
    g_unshuffledKnown = slotData.contains("unshuffled_dungeons");
    for (const auto& d : slotData.value("unshuffled_dungeons", json::array())) {
        if (d.is_string()) {
            g_unshuffled.push_back(d.get<std::string>());
        }
    }
    g_haveSlot = true;
    g_reachInputs = SIZE_MAX;
    ++g_trackerVersion;
    return true;
}

// ---------------------------------------------------------------------------------------
// Seed generation from slot data (runs on a worker thread)

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            c = '_';
        }
    }
    return s;
}

// Where a seed's settings and plando live; the tracker rebuilds its logic from the same files.
std::filesystem::path seed_base(const std::string& seedName, const std::string& slot) {
    return randomizer::paths::GetRandomizerPath() / "archipelago" / sanitize(seedName + "-" + slot);
}

bool generate_seed(const json& slotData, const std::string& slot, std::string& outHash,
    std::string& outError) {
    namespace fs = std::filesystem;
    std::lock_guard lock{tracker::g_generatorMutex};
    try {
        const std::string seedName = slotData.value("seed", "");
        const fs::path base = seed_base(seedName, slot);
        fs::create_directories(base);

        YAML::Node settings;
        settings["Seed"] = "AP-" + seedName + "-" + slot;
        settings["Plandomizer"] = true;
        settings["Generate Spoiler Log"] = false;
        settings["Starting Inventory"] = YAML::Node(YAML::NodeType::Map);
        settings["Excluded Locations"] = YAML::Node(YAML::NodeType::Sequence);
        settings["Mixed Entrance Pools"] = YAML::Node(YAML::NodeType::Sequence);
        for (const auto& [k, v] : slotData.value("settings", json::object()).items()) {
            settings[k] = v.get<std::string>();
        }
        std::ofstream(base / "settings.yaml") << YAML::Dump(settings);

        YAML::Node plando;
        YAML::Node locs(YAML::NodeType::Map);
        for (const auto& [loc, v] : slotData.value("placements", json::object()).items()) {
            locs[loc] = v.is_string() ? v.get<std::string>() : v.value("item", "Archipelago Item");
        }
        plando["World 1"]["Locations"] = locs;
        const fs::path plandoPath = base / "plando.yaml";
        std::ofstream(plandoPath) << YAML::Dump(plando);

        YAML::Node prefs;
        prefs["Plandomizer Path"] = plandoPath.generic_string();
        std::ofstream(base / "preferences.yaml") << YAML::Dump(prefs);

        randomizer::g_archipelagoMode = true;
        randomizer::Randomizer rando{base};
        auto err = rando.Generate();
        randomizer::g_archipelagoMode = false;
        if (err.has_value()) {
            outError = *err;
            return false;
        }
        RandomizerContext ctx = WriteSeedData(rando.GetWorld());
        ctx.mHash = rando.GetConfig().GetHash();
        fs::create_directories(ctx.GetSeedDataPath().parent_path());
        if (auto werr = ctx.WriteToFile(); werr.has_value()) {
            outError = *werr;
            return false;
        }
        outHash = ctx.mHash;
        return true;
    } catch (const std::exception& e) {
        randomizer::g_archipelagoMode = false;
        outError = e.what();
        return false;
    }
}

void start_generation(const json& slotData, const std::string& slot) {
    g_genStatus = 1;
    std::thread([slotData, slot] {
        std::string hash, error;
        const bool ok = generate_seed(slotData, slot, hash, error);
        {
            std::lock_guard lock{g_genMutex};
            g_genHash = hash;
            g_genError = error;
        }
        g_genStatus = ok ? 2 : 3;
    }).detach();
}

bool seed_files_exist(const std::string& hash) {
    std::error_code ec;
    return !hash.empty() &&
           std::filesystem::exists(randomizer::paths::GetRandomizerSeedsPath() / hash / "seed.dat", ec);
}

// ---------------------------------------------------------------------------------------
// Client callbacks

json g_lastSlotData;

void on_connected(const json& p) {
    const json slotData = p.value("slot_data", json::object());
    std::string err;
    if (!load_slot_data(slotData, err)) {
        g_status = err;
        toast("Archipelago", err, "warning");
        g_client.disconnect();
        return;
    }
    g_lastSlotData = slotData;
    const json deathLink = slotData.value("death_link", json(false));
    g_deathLinkSlot = deathLink.is_boolean() ? deathLink.get<bool>()
                                             : deathLink.is_number() && deathLink.get<int>() != 0;
    apply_death_link_tags();
    const json settings = slotData.value("settings", json::object());
    g_transformAnywhereSlot = settings.value("Logic Transform Anywhere", std::string{}) == "On";
    if (g_phase == Phase::Playing && g_state.transformAnywhere != g_transformAnywhereSlot) {
        g_state.transformAnywhere = g_transformAnywhereSlot;
        write_state();
    }
    g_checked.clear();
    for (const auto& id : p.value("checked_locations", json::array())) {
        g_checked.insert(id.get<int64_t>());
    }
    g_reachInputs = SIZE_MAX;
    ++g_trackerVersion;

    ap_log(fmt::format("connected in phase {}: {} locations, {} ap placements",
        static_cast<int>(g_phase), g_locationIds.size(), g_apItemText.size()));
    if (g_phase == Phase::NewSave) {
        g_status = "Connected. Building your seed...";
        g_phase = Phase::Generating;
        start_generation(slotData, g_client.info().slot);
        return;
    }
    if (g_phase == Phase::Playing) {
        if (!g_state.seed.empty() && g_state.seed != g_slotSeed) {
            toast("Archipelago", "This save belongs to a different multiworld seed. Disconnected.",
                "warning", 8000);
            g_client.disconnect();
            g_haveSlot = false;
            return;
        }
        if (g_needsRegen) {
            toast("Archipelago", "Rebuilding this save's seed from the server...");
            g_phase = Phase::Generating;
            start_generation(slotData, g_client.info().slot);
            return;
        }
        g_client.sendSync();  // also proves the transport can send after the handshake
        toast("Archipelago", status_line(), "success", 3000);
        if (g_state.goal) {
            g_client.sendGoal();
        }
    }
}

void on_items(int index, const std::vector<NetworkItem>& items) {
    if (index == 0) {
        g_serverItems.clear();
    }
    if (static_cast<size_t>(index) != g_serverItems.size()) {
        g_client.sendSync();  // gap: ask for the full list again
        return;
    }
    g_serverItems.insert(g_serverItems.end(), items.begin(), items.end());
}

int hint_rank(const Hint& h) {
    if (h.found) {
        return 4;
    }
    switch (h.status) {
    case 30: return 0;  // priority
    case 0: return 1;   // unspecified
    case 10: return 2;  // no priority
    default: return 3;  // avoid
    }
}

void on_hints(const std::vector<Hint>& hints) {
    g_hints = hints;
    std::stable_sort(g_hints.begin(), g_hints.end(),
        [](const Hint& a, const Hint& b) { return hint_rank(a) < hint_rank(b); });
    ++g_hintsVersion;
}

void on_print(const std::string& text, const json& msg) {
    log_rml(print_rml(msg.value("data", json::array()), g_client));
    const std::string type = msg.value("type", "");
    const int me = g_client.slot();
    if (type == "Hint" && !msg.value("found", false)) {
        const int receiver = msg.value("receiving", -1);
        const int finder = msg.contains("item") ? msg["item"].value("player", -1) : -1;
        if (receiver == me || finder == me) {
            toast("Hint", text, nullptr, 6000);
        }
        return;
    }
    if (type == "ItemSend" || type == "ItemCheat") {
        const int receiver = msg.value("receiving", -1);
        const int sender = msg.contains("item") ? msg["item"].value("player", -1) : -1;
        if (receiver == me || sender == me) {
            if (receiver == me && sender == me) {
                return;  // our own item at our own location: the game already showed it
            }
            toast("Archipelago", text, nullptr, 4000);
        }
    } else if (type == "Goal" || type == "Release" || type == "Collect" || type == "Countdown") {
        toast("Archipelago", text, nullptr, 4000);
    } else if (type == "Chat" || type == "ServerChat") {
        toast("Archipelago", text, nullptr, 5000);
        g_recentChat.push_back({text, std::chrono::steady_clock::now()});
        while (g_recentChat.size() > 8) {
            g_recentChat.pop_front();
        }
    }
}

void on_disconnected(const std::string& reason) {
    log_rml("<span class=\"ap-quiet\">" + rml_escape(message_safe("Disconnected: " + reason, 300)) +
            "</span>");
    if (g_phase == Phase::NewSave || g_phase == Phase::Generating) {
        g_status = "Could not connect: " + reason;
    } else if (g_phase == Phase::Playing) {
        toast("Archipelago", "Disconnected: " + reason, "warning", 4000);
    }
}

// ---------------------------------------------------------------------------------------
// ItemService callbacks

bool resolve_check(ModContext*, const ItemCheckInfo* info, ItemCheckResolution*, void*) {
    // Runs after the randomizer's resolver; only remembers where the last AP item came from.
    if (info->current_item == kApItem) {
        const auto locs = locations_for_check(info->name);
        if (!locs.empty()) {
            g_lastResolvedApLocation = locs.front();
        }
    }
    return false;
}

void report_locations(const std::vector<std::string>& locs) {
    for (const auto& loc : locs) {
        const auto it = g_locationIds.find(loc);
        ap_log(fmt::format("report_location '{}' known={}", loc, it != g_locationIds.end()));
        {
            if (!g_checked.contains(it->second)) {
                g_toSend.push_back(it->second);
                g_checked.insert(it->second);
            }
        }
    }
}

void observe_give(ModContext*, const ItemGiveInfo* info, void*) {
    std::string where = "(none)";
    std::string expected = "-";
    if (info->check_name != nullptr) {
        where = info->check_name;
        const auto locs = locations_for_check(info->check_name);
        if (!locs.empty()) {
            where += " -> " + locs.front();
            if (const auto e = g_expected.find(locs.front()); e != g_expected.end()) {
                expected = e->second;
            }
        }
    }
    ap_log(fmt::format("give item=0x{:02X} ({}) origin={} check='{}' ap_expects='{}'", info->item,
        item_name(info->item), info->origin, where, expected));
    if (info->check_name != nullptr) {
        const auto locs = locations_for_check(info->check_name);
        report_locations(locs);
        if (info->item == kApItem && !locs.empty()) {
            g_lastResolvedApLocation = locs.front();
        }
        return;
    }
    if ((info->origin == ITEM_GIVE_ORIGIN_QUEUE || info->origin == ITEM_GIVE_ORIGIN_QUEUE_SILENT) &&
        g_outstanding >= 0)
    {
        g_outstanding = -1;
        ++g_state.received;
        write_state();
    }
}

bool ap_item_text(ModContext*, const MessageOverrideContext*, MessageTextData* out, void*) {
    if (g_armedFrames <= 0 || g_armedText.empty()) {
        return false;
    }
    // Sanitized again at the sink: whatever the source, the renderer only ever sees printable
    // text of a sane length.
    const std::string text = message_safe(g_armedText, 160);
    thread_local std::vector<uint8_t> buf;
    buf.assign(text.begin(), text.end());
    buf.push_back(0);
    out->text = buf.data();
    out->text_size = buf.size();
    // Consume it: the next Archipelago item re-arms with its own text, and a real Foolish
    // Item (which shares this message) must not inherit it.
    g_armedFrames = 0;
    return true;
}

// ---------------------------------------------------------------------------------------
// Hooks: AP item model scale + goal detection

DEFINE_HOOK(&daDitem_c::set_mtx, ApDitemSetMtx);
DEFINE_HOOK(&daItem_c::setBaseMtx, ApItemSetBaseMtx);
DEFINE_HOOK(dStage_changeScene, ApChangeScene);
DEFINE_HOOK(&daAlink_c::procCoDeadInit, ApLinkDeadInit);
DEFINE_HOOK(&daAlink_c::procCoFogDeadInit, ApLinkFogDeadInit);
// Ganondorf's execute is file-local; hook it by translation unit alias.
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_b_gnd.cpp#daB_GND_Execute", int(b_gnd_class*), ApGanondorf);
// Midna's "is an NPC watching?" search is file-local too (see Transform Anywhere below).
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_midna.cpp#daMidna_searchNpc", void*(fopAc_ac_c*, void*),
    ApMidnaSearchNpc);
DEFINE_HOOK(&dMsgFlow_c::query042, ApMsgQuery042);
DEFINE_HOOK(&dMeter2Draw_c::draw, ApMeter2Draw);

float model_scale() {
    double s = 0.6;
    if (g_cfgModelScale != 0) {
        svc_mng.config->get_float(svc_mng.mod_ctx, g_cfgModelScale, &s);
    }
    return static_cast<float>(s);
}

void scale_model(J3DModel* model) {
    if (model == nullptr) {
        return;
    }
    Mtx m;
    std::memcpy(m, model->getBaseTRMtx(), sizeof(Mtx));
    const float s = model_scale();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r][c] *= s;
        }
    }
    model->setBaseTRMtx(m);
}

void post_ditem_set_mtx(ModContext*, void* args, void*, void*) {
    auto* item = mods::arg<daDitem_c*>(args, 0);
    if (item->getDisplayItemNo() == kApItem) {
        scale_model(item->mpModel);
    }
}

void post_item_set_base_mtx(ModContext*, void* args, void*, void*) {
    auto* item = mods::arg<daItem_c*>(args, 0);
    if (item->getDisplayItemNo() == kApItem) {
        scale_model(item->mpModel);
    }
}

void complete_goal(const char* how) {
    if (g_state.goal) {
        return;
    }
    g_state.goal = true;
    write_state();
    g_client.sendGoal();
    ap_log(fmt::format("goal complete ({})", how));
    toast("Archipelago", "Goal complete!", "success", 6000);
}

void post_ganondorf_execute(ModContext*, void* args, void*, void*) {
    // The ending plays out inside the arena (no stage or scene change), so watch Ganondorf
    // himself: the finishing blow puts him in the end action with the ending camera running.
    constexpr s16 kActionEnd = 22;   // ACTION_END in d_a_b_gnd.cpp
    constexpr s16 kEndingCamera = 60;
    auto* gnd = mods::arg<b_gnd_class*>(args, 0);
    if (gnd != nullptr && gnd->mActionMode == kActionEnd && gnd->mDemoCamMode >= kEndingCamera) {
        complete_goal("Ganondorf defeated");
    }
}

HookAction pre_change_scene(ModContext*, void*, void*, void*) {
    const char* stage = dComIfGp_getStartStageName();
    ap_log(fmt::format("changeScene from stage '{}'", stage != nullptr ? stage : "?"));
    // Leaving Dark Lord Ganondorf's arena only happens through the ending.
    if (g_phase == Phase::Playing && stage != nullptr && std::strcmp(stage, "D_MN09C") == 0) {
        complete_goal("left D_MN09C");
    }
    return HOOK_CONTINUE;
}

// ---------------------------------------------------------------------------------------
// Per-frame work

bool in_gameplay() {
    return g_phase == Phase::Playing && !g_needsRegen && randomizer_IsActive() &&
           !playerIsOnTitleScreen() && dComIfGp_getPlayer(0) != nullptr;
}

// ---------------------------------------------------------------------------------------
// Transform Anywhere
//
// With the slot's Logic Transform Anywhere on, logic may expect Link to transform where an NPC
// can see him, which the game refuses unless Dusklight's Can Transform Anywhere cheat is on.
// Mods can't reach host settings, so do what that setting does, for this save only: the host
// checks it at both calls of daMidna_searchNpc, and in one Castle Town branch of query042 (the
// flow query behind talking to Midna about transforming).

bool transform_anywhere_on() {
    return g_phase == Phase::Playing && g_state.transformAnywhere && randomizer_IsActive();
}

void post_midna_search_npc(ModContext*, void*, void* retval, void*) {
    if (transform_anywhere_on()) {
        *static_cast<void**>(retval) = nullptr;  // nobody is watching
    }
}

void post_msg_query042(ModContext*, void*, void* retval, void*) {
    // 4 is the Castle Town branch the host skips under Can Transform Anywhere. Past it the query
    // checks for nearby NPCs (never flagged now) and then for twilight (3).
    auto* ret = static_cast<u16*>(retval);
    if (transform_anywhere_on() && *ret == 4) {
        *ret = (g_env_light.mEvilInitialized & 0x80) ? 3 : 0;
    }
}

// ---------------------------------------------------------------------------------------
// Death link
//
// Every real death goes through daAlink_c::procCoDeadInit (procCoFogDeadInit for the fog),
// which checkDeadAction() calls once life is 0 and no bottled fairy can step in. A fairy
// save takes a different branch and never gets here, so it isn't a death. Killing Link is
// the same thing in reverse: set life to 0 and let the game's own check do the rest,
// fairies included.

bool death_link_on() {
    return g_state.deathLink >= 0 ? g_state.deathLink == 1 : g_deathLinkSlot;
}

void apply_death_link_tags() {
    g_client.setTags(death_link_on() ? std::vector<std::string>{"DeathLink"}
                                     : std::vector<std::string>{});
}

bool link_is_dead(const daAlink_c* link) {
    return link->mProcID == daAlink_c::PROC_DEAD || link->mProcID == daAlink_c::PROC_FOG_DEAD;
}

void on_link_died(daAlink_c* link, const char* how) {
    // The init returns early if Link was already in the death proc, so check he got there.
    if (link == nullptr || !link_is_dead(link) || g_deathSent) {
        return;
    }
    g_deathSent = true;
    if (g_killFrames > 0) {
        g_killFrames = 0;  // the death we were sent landing: don't bounce it back
        ap_log("death link: received death landed");
        return;
    }
    if (!death_link_on() || !in_gameplay() || g_client.state() != State::Connected) {
        return;
    }
    const std::string who = g_client.info().slot;
    g_lastDeathTime = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    g_client.sendBounce({
        {"tags", json::array({"DeathLink"})},
        {"data", {{"time", g_lastDeathTime}, {"source", who}, {"cause", who + " " + how}}},
    });
    ap_log(fmt::format("death link: sent ({})", how));
}

void post_link_dead_init(ModContext*, void* args, void*, void*) {
    const bool drowned = dComIfGp_getOxygenShowFlag() && dComIfGp_getNowOxygen() == 0;
    on_link_died(mods::arg<daAlink_c*>(args, 0), drowned ? "drowned." : "was defeated.");
}

void post_link_fog_dead_init(ModContext*, void* args, void*, void*) {
    on_link_died(mods::arg<daAlink_c*>(args, 0), "was lost in the fog.");
}

void on_bounced(const json& p) {
    const json tags = p.value("tags", json::array());
    const bool isDeath = std::any_of(tags.begin(), tags.end(),
        [](const json& t) { return t.is_string() && t.get<std::string>() == "DeathLink"; });
    if (!isDeath || !death_link_on()) {
        return;
    }
    const json data = p.value("data", json::object());
    const json time = data.value("time", json());
    if (time.is_number() && std::abs(time.get<double>() - g_lastDeathTime) < 1e-3) {
        return;  // our own death, echoed back by the server
    }
    const json source = data.value("source", json());
    const json cause = data.value("cause", json());
    const std::string who = source.is_string() ? source.get<std::string>() : "Someone";
    g_pendingDeath = cause.is_string() && !cause.get<std::string>().empty() ?
                         cause.get<std::string>() : who + " died.";
    ap_log("death link: received from " + who);
}

void tick_death_link() {
    if (!in_gameplay()) {
        return;
    }
    auto* link = daAlink_getAlinkActorClass();
    if (link == nullptr) {
        return;
    }
    const bool dead = link_is_dead(link) || dComIfGs_getLife() == 0;
    if (!dead) {
        g_deathSent = false;  // alive again, so the next death is a new one
    }
    if (g_killFrames > 0) {
        // Alive a few frames after we zeroed his life means a fairy saved him. Stop treating
        // the next death as ours, or a real one moments later would be swallowed.
        if (!dead && g_killFrames < 598) {
            g_killFrames = 0;
            ap_log("death link: a fairy saved Link from the received death");
        } else {
            --g_killFrames;
        }
    }
    if (!g_pendingDeath) {
        return;
    }
    if (dead) {
        g_pendingDeath.reset();  // already dying; there's nothing more to take
        return;
    }
    if (dComIfGp_event_runCheck()) {
        return;  // wait out the cutscene or conversation
    }
    toast("Death link", *g_pendingDeath, "warning", 5000);
    g_pendingDeath.reset();
    g_killFrames = 600;
    dComIfGs_setLife(0);
}

void log_stage_changes() {
    static std::string last;
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && last != stage) {
        last = stage;
        ap_log(fmt::format("stage -> {}", last));
    }
}

void scan_locations() {
    if (!g_haveSlot || g_scan.empty() || !in_gameplay()) {
        return;
    }
    for (int n = 0; n < 24; ++n) {
        const auto& loc = g_scan[g_scanPos];
        g_scanPos = (g_scanPos + 1) % g_scan.size();
        if (g_checked.contains(loc.id)) {
            continue;
        }
        if (loc.meta.IsMap() && isLocationMetadataObtained(loc.meta, loc.name)) {
            g_toSend.push_back(loc.id);
            g_checked.insert(loc.id);
        }
    }
}

void flush_checks() {
    if (!g_toSend.empty() && g_client.state() == State::Connected) {
        ap_log(fmt::format("sending {} location check(s)", g_toSend.size()));
        g_client.sendLocations(g_toSend);
        g_toSend.clear();
    }
}

void deliver_items() {
    if (!in_gameplay() || g_outstanding >= 0 ||
        g_state.received >= static_cast<int>(g_serverItems.size()))
    {
        return;
    }
    const auto& it = g_serverItems[g_state.received];
    const int64_t id = it.item - kItemIdBase;
    if (id < 0 || id > 0xFE) {
        ++g_state.received;  // not a giveable item for this game; skip
        write_state();
        return;
    }
    const auto give = static_cast<uint8_t>(verifyProgressiveItem(static_cast<u32>(id)));
    const bool progression = (it.flags & 1) != 0;
    const bool fromOther = it.player != g_client.slot() && it.location >= 0;
    const uint32_t flags = (progression && fromOther) ? 0u : ITEM_GIVE_SILENT;
    if (svc_mng.item->give_item(svc_mng.mod_ctx, nullptr, give, flags) != MOD_OK) {
        return;
    }
    g_outstanding = give;
    {
        const std::string sender = g_client.playerName(it.player);
        g_recentItems.push_back({fmt::format("+ {}{}", item_name(give),
                                     fromOther && !sender.empty() ? " (" + sender + ")" : ""),
            std::chrono::steady_clock::now()});
        while (g_recentItems.size() > 8) {
            g_recentItems.pop_front();
        }
    }
    if (flags & ITEM_GIVE_SILENT) {
        const std::string from = g_client.playerName(it.player);
        toast("Received item", fmt::format("{}{}", item_name(give),
                                   fromOther && !from.empty() ? " from " + from : ""),
            nullptr, 3000);
    }
}

void tick_generation() {
    const int st = g_genStatus.load();
    if (st < 2) {
        return;
    }
    g_genStatus = 0;
    std::string hash, error;
    {
        std::lock_guard lock{g_genMutex};
        hash = g_genHash;
        error = g_genError;
    }
    if (st == 3) {
        mods::log::error("archipelago: seed generation failed: {}", error);
        g_status = "Seed generation failed: " + error;
        if (g_needsRegen) {
            toast("Archipelago", g_status, "warning", 10000);
        }
        g_phase = g_needsRegen ? Phase::Playing : Phase::NewSave;
        return;
    }
    if (g_needsRegen) {
        // Loaded save on a machine without its seed files: activate the rebuilt seed now.
        if (hash != g_loadedHash) {
            toast("Archipelago", "Rebuilt seed doesn't match this save.", "warning", 10000);
        } else {
            randomizer::session::deactivateSeed();
            randomizer::session::activateSeed(hash.c_str());
            loadAncientDocumentNum();
            g_needsRegen = false;
            after_seed_activated();
        }
        g_phase = Phase::Playing;
        return;
    }
    randomizer::session::g_pending_seed_hash = hash;
    g_status = "Seed ready!";
    g_phase = Phase::AwaitNewSave;
    randomizer::ui::g_file_select_window_ctx.is_proceed = true;  // continue to name entry
    mDoAud_seStartMenu(Z2SE_SY_NEW_FILE);
    if (g_window != 0) {
        const auto w = g_window;
        g_window = 0;
        svc_mng.ui->window_close(svc_mng.mod_ctx, w);
    }
}

void after_seed_activated() {
    // Our resolver must run after the randomizer's (registered on seed activation), so it sees
    // the resolved "Archipelago Item"; message overrides likewise stack on top of its text.
    if (g_resolver != 0) {
        svc_mng.item->clear_check_resolver(svc_mng.mod_ctx, g_resolver);
        g_resolver = 0;
    }
    svc_mng.item->set_check_resolver(svc_mng.mod_ctx, nullptr, resolve_check, nullptr, &g_resolver);
    g_textOverrides.clear();
    for (auto lang : {MESSAGE_LANGUAGE_ENGLISH, MESSAGE_LANGUAGE_GERMAN, MESSAGE_LANGUAGE_FRENCH,
             MESSAGE_LANGUAGE_SPANISH, MESSAGE_LANGUAGE_ITALIAN, MESSAGE_LANGUAGE_JAPANESE})
    {
        g_textOverrides.push_back(
            mods::flow::override_message_fn(0, kApItemDonorMessage, lang, ap_item_text));
    }
}

// ---------------------------------------------------------------------------------------
// New-save window

std::string g_inServer, g_inSlot, g_inPassword;

void get_str(ModContext*, void* ud, UiControlValue* out) {
    out->string_value = static_cast<std::string*>(ud)->c_str();
}
void set_str(ModContext*, void* ud, const UiControlValue* v) {
    *static_cast<std::string*>(ud) = v->string_value != nullptr ? v->string_value : "";
}

void press_connect(ModContext*, void*) {
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

bool connect_disabled(ModContext*, void*) {
    return g_phase != Phase::NewSave || g_client.state() == State::Connecting ||
           g_client.state() == State::Handshaking;
}

ModResult build_new_save_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle,
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

ModResult update_new_save_tab(ModContext* ctx, void*, ModError*) {
    if (g_statusText != 0) {
        static std::string shown;
        if (shown != g_status) {
            shown = g_status;
            svc_mng.ui->elem_set_text(ctx, g_statusText, g_status.c_str());
        }
    }
    return MOD_OK;
}

void* g_fileSelect = nullptr;

void new_save_window_closed(ModContext*, UiWindowHandle, void*) {
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
    g_phase = Phase::Playing;
    ap_log(fmt::format("new save created, hash {}, scan list {}", g_loadedHash, g_scan.size()));
    after_seed_activated();
    return MOD_OK;
}

ModResult on_save_loaded(void* ud, ModError* err) {
    g_outstanding = -1;
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
    reset_tracker();
    return MOD_OK;
}

// ---------------------------------------------------------------------------------------
// Tracker
//
// The seed's world is rebuilt once per save on a worker thread (ap_tracker.cpp), from the same
// files the seed was generated from, then asked "which checks can I reach?" on another worker
// whenever items or checks change. The game thread only hands inputs over and takes results.

struct WorkerResults {
    std::mutex mutex;
    uint64_t epoch = 0;  // results are kept only while this matches the worker's
    bool logicDone = false;
    std::shared_ptr<tracker::Logic> logic;
    std::string logicError;
    bool reachDone = false;
    std::unordered_set<std::string> reach;
};
WorkerResults g_workers;

void reset_tracker() {
    ++g_trackerEpoch;
    {
        std::lock_guard lock{g_workers.mutex};
        g_workers.epoch = g_trackerEpoch;
        g_workers.logicDone = g_workers.reachDone = false;
        g_workers.logic.reset();
        g_workers.reach.clear();
    }
    g_logic.reset();
    g_logicKey.clear();
    g_logicBuilding = false;
    g_logicError.clear();
    g_inLogic.clear();
    g_reachInputs = SIZE_MAX;
    g_reachBusy = false;
    ++g_trackerVersion;
}

void start_logic_build(const std::filesystem::path& base) {
    g_logicBuilding = true;
    const uint64_t epoch = g_trackerEpoch;
    std::thread([base, epoch] {
        std::string error;
        std::shared_ptr<tracker::Logic> logic = tracker::Logic::build(base, error);
        std::lock_guard lock{g_workers.mutex};
        if (epoch == g_workers.epoch) {
            g_workers.logic = std::move(logic);
            g_workers.logicError = error.empty() ? "the seed could not be rebuilt" : error;
            g_workers.logicDone = true;
        }
    }).detach();
}

void start_reach_search() {
    std::vector<uint16_t> received;
    for (const auto& it : g_serverItems) {
        const int64_t id = it.item - kItemIdBase;
        if (id >= 0 && id <= 0xFFFF) {
            received.push_back(static_cast<uint16_t>(id));
        }
    }
    std::unordered_set<std::string> checks, checked;
    for (const auto& c : g_trackerChecks) {
        checks.insert(c.name);
        if (g_checked.contains(c.id)) {
            checked.insert(c.name);
        }
    }
    g_reachBusy = true;
    std::thread([logic = g_logic, epoch = g_trackerEpoch, received = std::move(received),
                    checks = std::move(checks), checked = std::move(checked),
                    unshuffled = g_unshuffled, known = g_unshuffledKnown] {
        auto reach = logic->reachable(
            received, checks, checked, known ? unshuffled : logic->guess_unshuffled(checks));
        std::lock_guard lock{g_workers.mutex};
        if (epoch == g_workers.epoch) {
            g_workers.reach = std::move(reach);
            g_workers.reachDone = true;
        }
    }).detach();
}

void tick_tracker() {
    {
        std::lock_guard lock{g_workers.mutex};
        if (g_workers.logicDone) {
            g_workers.logicDone = false;
            g_logic = std::move(g_workers.logic);
            g_logicError = g_logic ? "" : g_workers.logicError;
            g_logicBuilding = false;
            g_reachInputs = SIZE_MAX;
            ++g_trackerVersion;
            ap_log(g_logic ? "tracker: logic ready" : "tracker: logic failed: " + g_logicError);
        }
        if (g_workers.reachDone) {
            g_workers.reachDone = false;
            g_inLogic = std::move(g_workers.reach);
            g_reachBusy = false;
            ++g_trackerVersion;
        }
    }
    if (g_phase != Phase::Playing || g_needsRegen || g_state.seed.empty()) {
        return;
    }
    const std::string key = g_state.seed + "-" + g_state.slot;
    if (key != g_logicKey) {
        reset_tracker();
        g_logicKey = key;
        const auto base = seed_base(g_state.seed, g_state.slot);
        std::error_code ec;
        if (std::filesystem::exists(base / "plando.yaml", ec)) {
            start_logic_build(base);
        } else {
            g_logicError = "this save's seed files aren't on this computer";
        }
        return;
    }
    if (!g_logic || !g_haveSlot || g_reachBusy) {
        return;
    }
    // Items only ever arrive and checks only ever get done, so the two counts say when the
    // answer can have changed.
    const size_t inputs = g_serverItems.size() * 100003u + g_checked.size();
    if (inputs != g_reachInputs) {
        g_reachInputs = inputs;
        start_reach_search();
    }
}

enum class CheckState { Done, InLogic, NotYet, Unknown };

CheckState check_state(const TrackerCheck& c) {
    if (g_checked.contains(c.id)) {
        return CheckState::Done;
    }
    if (!g_logic || g_reachInputs == SIZE_MAX) {
        return CheckState::Unknown;
    }
    return g_inLogic.contains(c.name) ? CheckState::InLogic : CheckState::NotYet;
}

struct RegionCounts {
    size_t total = 0, done = 0, inLogic = 0;
};

RegionCounts region_counts(int region) {
    RegionCounts n;
    for (const auto& c : g_trackerChecks) {
        if (region >= 0 && c.region != region) {
            continue;
        }
        ++n.total;
        const auto st = check_state(c);
        n.done += st == CheckState::Done;
        n.inLogic += st == CheckState::InLogic;
    }
    return n;
}

std::string tracker_summary() {
    if (!g_haveSlot) {
        return "Connect to your room to see your checks.";
    }
    const auto n = region_counts(-1);
    std::string s = fmt::format("{} of {} checks done", n.done, n.total);
    if (g_logic) {
        s += fmt::format(", {} in logic now", n.inLogic);
    } else if (g_logicBuilding) {
        s += ". Working out logic...";
    } else if (!g_logicError.empty()) {
        s += ". Logic unavailable: " + g_logicError + ".";
    }
    return s;
}

std::string region_label(int region) {
    const auto n = region_counts(region);
    std::string label = fmt::format("{}   {}/{}", tracker_regions()[region].label, n.done, n.total);
    if (g_logic && n.inLogic > 0) {
        label += fmt::format("   ({} in logic)", n.inLogic);
    }
    return label;
}

std::string check_row(const TrackerCheck& c, CheckState st) {
    const char* cls = st == CheckState::Done      ? "ap-done"
                      : st == CheckState::InLogic ? "ap-in"
                      : st == CheckState::NotYet  ? "ap-out"
                                                  : "ap-unknown";
    return fmt::format(R"(<div class="ap-row {}"><span class="ap-dot"></span>{}</div>)", cls,
        rml_escape(c.name));
}

// kInLogicNow lists every check in logic under region headings; otherwise one region's checks,
// in logic first and done last.
constexpr int kInLogicNow = -2;

std::string region_rml(int region) {
    if (!g_haveSlot) {
        return "";
    }
    std::string out;
    if (region == kInLogicNow) {
        if (!g_logic) {
            return R"(<div class="ap-quiet">)" + rml_escape(tracker_summary()) + "</div>";
        }
        for (int r = 0; r < static_cast<int>(tracker_regions().size()); ++r) {
            std::string rows;
            size_t n = 0;
            for (const auto& c : g_trackerChecks) {
                if (c.region == r && check_state(c) == CheckState::InLogic) {
                    rows += check_row(c, CheckState::InLogic);
                    ++n;
                }
            }
            if (n > 0) {
                out += fmt::format(R"(<div class="ap-head">{} ({})</div>)",
                    rml_escape(tracker_regions()[r].label), n);
                out += rows;
            }
        }
        return out.empty() ? R"(<div class="ap-quiet">Nothing in logic right now.</div>)" : out;
    }
    const auto& regions = tracker_regions();
    if (region < 0 || region >= static_cast<int>(regions.size())) {
        return "";
    }
    out += fmt::format(R"(<div class="ap-head">{}</div>)", rml_escape(region_label(region)));
    for (const auto want :
        {CheckState::InLogic, CheckState::Unknown, CheckState::NotYet, CheckState::Done}) {
        for (const auto& c : g_trackerChecks) {
            if (c.region == region && check_state(c) == want) {
                out += check_row(c, want);
            }
        }
    }
    return out;
}

constexpr const char* kLegendRml =
    R"(<div class="ap-legend"><span class="ap-in"><span class="ap-dot"></span>in logic</span>)"
    R"(<span class="ap-out"><span class="ap-dot"></span>not yet</span>)"
    R"(<span class="ap-done"><span class="ap-dot"></span>done</span></div>)";

// The window's styles: tracker rows, and Archipelago's usual colors for the message log.
constexpr const char* kWindowRcss = R"(
.ap-row { display: block; padding: 3dp 0dp; }
.ap-dot { display: inline-block; width: 10dp; height: 10dp; border-radius: 5dp; margin-right: 10dp; }
.ap-in { color: #ffffff; }
.ap-in .ap-dot { background-color: #6fd08c; }
.ap-out { color: #a6a6a6; }
.ap-out .ap-dot { border: 2dp #a6a6a6; }
.ap-unknown { color: #d0d0d0; }
.ap-unknown .ap-dot { border: 2dp #d0d0d0; }
.ap-done { color: #6c6c6c; text-decoration: line-through; }
.ap-done .ap-dot { background-color: #6c6c6c; }
.ap-head { display: block; margin-top: 10dp; margin-bottom: 2dp; font-weight: bold; color: #e8c867; }
.ap-legend > span { margin-right: 18dp; }
.ap-quiet { color: #a6a6a6; }
.ap-msg { display: block; padding: 3dp 0dp; }
.ap-me { color: #ee00ee; }
.ap-player { color: #fafad2; }
.ap-prog { color: #af99ef; }
.ap-useful { color: #6d8be8; }
.ap-trap { color: #fa8072; }
.ap-item { color: #00eeee; }
.ap-loc { color: #00ff7f; }
.ap-ent { color: #6495ed; }
)";

// Tracker tab
UiElementHandle g_trackerSummary = 0;
UiElementHandle g_trackerProgress = 0;
UiElementHandle g_trackerList = 0;
int g_trackerShownRegion = kInLogicNow;
std::pair<uint64_t, size_t> g_trackerShown{0, 0};
std::vector<std::pair<UiElementHandle, int>> g_trackerGroups;  // group control, region

float done_fraction() {
    const auto n = region_counts(-1);
    return n.total == 0 ? 0.0f : static_cast<float>(n.done) / static_cast<float>(n.total);
}

ModResult build_region_pane(ModContext* ctx, UiElementHandle pane, void* ud, ModError*) {
    g_trackerShownRegion = static_cast<int>(reinterpret_cast<intptr_t>(ud));
    g_trackerList = 0;
    return svc_mng.ui->pane_add_rml(
        ctx, pane, region_rml(g_trackerShownRegion).c_str(), &g_trackerList);
}

ModResult build_tracker_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    g_trackerSummary = g_trackerProgress = g_trackerList = 0;
    g_trackerGroups.clear();
    svc_mng.ui->pane_add_text(ctx, left, tracker_summary().c_str(), &g_trackerSummary);
    svc_mng.ui->pane_add_progress(ctx, left, done_fraction(), &g_trackerProgress);
    svc_mng.ui->pane_add_rml(ctx, left, kLegendRml, nullptr);

    UiGroupDesc now = UI_GROUP_DESC_INIT;
    now.label = "In logic now";
    now.build = build_region_pane;
    now.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(kInLogicNow));
    svc_mng.ui->pane_add_group(ctx, left, right, &now, nullptr);

    svc_mng.ui->pane_add_section(ctx, left, "Regions");
    const auto& regions = tracker_regions();
    for (int r = 0; r < static_cast<int>(regions.size()); ++r) {
        if (region_counts(r).total == 0) {
            continue;
        }
        const std::string label = region_label(r);
        UiGroupDesc g = UI_GROUP_DESC_INIT;
        g.label = label.c_str();
        g.build = build_region_pane;
        g.user_data = reinterpret_cast<void*>(static_cast<intptr_t>(r));
        UiElementHandle elem = 0;
        svc_mng.ui->pane_add_group(ctx, left, right, &g, &elem);
        g_trackerGroups.emplace_back(elem, r);
    }

    // Open on what's in logic: the question mid-run is usually "what can I do now?".
    g_trackerShownRegion = kInLogicNow;
    svc_mng.ui->pane_add_rml(ctx, right, region_rml(kInLogicNow).c_str(), &g_trackerList);
    g_trackerShown = {g_trackerVersion, g_checked.size()};
    return MOD_OK;
}

ModResult update_tracker_tab(ModContext* ctx, void*, ModError*) {
    const std::pair<uint64_t, size_t> now{g_trackerVersion, g_checked.size()};
    if (now == g_trackerShown) {
        return MOD_OK;
    }
    g_trackerShown = now;
    if (g_trackerSummary != 0) {
        svc_mng.ui->elem_set_text(ctx, g_trackerSummary, tracker_summary().c_str());
    }
    if (g_trackerProgress != 0) {
        svc_mng.ui->elem_set_progress(ctx, g_trackerProgress, done_fraction());
    }
    for (const auto& [elem, region] : g_trackerGroups) {
        if (elem != 0) {
            svc_mng.ui->control_set_label(ctx, elem, region_label(region).c_str());
        }
    }
    if (g_trackerList != 0) {
        svc_mng.ui->elem_set_rml(ctx, g_trackerList, region_rml(g_trackerShownRegion).c_str());
    }
    return MOD_OK;
}

// Messages tab
UiElementHandle g_logElem = 0;
uint64_t g_logShown = 0;

std::string log_rml_all() {
    if (g_log.empty()) {
        return R"(<div class="ap-msg ap-quiet">Nothing yet. Messages from the room show up here.</div>)";
    }
    std::string out;
    for (auto it = g_log.rbegin(); it != g_log.rend(); ++it) {  // newest first
        out += R"(<div class="ap-msg">)" + *it + "</div>";
    }
    return out;
}

bool not_connected(ModContext*, void*) {
    return g_client.state() != State::Connected;
}

// Chat and server commands. The field only edits the draft; Send sends it, so moving focus
// away (to the emoji picker, say) never sends half a message.
void send_chat(ModContext*, void*) {
    std::string text = message_safe(g_chatDraft, 400);
    std::erase(text, '\n');
    const auto first = text.find_first_not_of(' ');
    if (first != std::string::npos && g_client.state() == State::Connected) {
        g_client.say(emoji::shortcodes_to_unicode(text.substr(first)));
        g_chatDraft.clear();
    }
}

ModResult build_emoji_picker(ModContext* ctx, UiElementHandle pane, void*, ModError*) {
    svc_mng.ui->pane_add_text(ctx, pane, "Pick one to add it to your message.", nullptr);
    UiRowDesc rowDesc = UI_ROW_DESC_INIT;
    rowDesc.wrap = true;
    UiElementHandle row = 0;
    svc_mng.ui->pane_add_row(ctx, pane, &rowDesc, &row);
    for (const auto& e : emoji::picker()) {
        UiControlDesc b = UI_CONTROL_DESC_INIT;
        b.kind = UI_CONTROL_BUTTON;
        b.label = e.name;
        b.user_data = const_cast<emoji::PickerEmoji*>(&e);
        b.on_pressed = [](ModContext*, void* ud) {
            const auto* pick = static_cast<const emoji::PickerEmoji*>(ud);
            if (!g_chatDraft.empty() && g_chatDraft.back() != ' ') {
                g_chatDraft += ' ';
            }
            g_chatDraft += fmt::format(":{}: ", pick->name);
        };
        UiElementHandle elem = 0;
        svc_mng.ui->pane_add_control(ctx, row, &b, &elem);
        if (elem != 0) {
            svc_mng.ui->elem_set_class(ctx, elem, "ap-emoji-btn", true);
            svc_mng.ui->elem_set_class(ctx, elem, fmt::format("ap-e-{}", e.file).c_str(), true);
        }
    }
    return MOD_OK;
}

ModResult build_messages_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left,
    UiElementHandle right, void*, ModError*) {
    UiControlDesc say = UI_CONTROL_DESC_INIT;
    say.kind = UI_CONTROL_STRING;
    say.label = "Message";
    say.help_rml = "Chat with everyone in the room, or send a server command such as "
                   "<b>!hint</b> <i>item name</i>, <b>!remaining</b> or <b>!help</b>. "
                   "Emoji codes like <b>:joy:</b> are sent as the emoji.";
    say.max_length = 400;
    say.get = get_str;
    say.set = set_str;
    say.user_data = &g_chatDraft;
    say.string_set_mode = UI_STRING_SET_ON_CHANGE;
    say.is_disabled = not_connected;
    svc_mng.ui->pane_add_control(ctx, left, &say, nullptr);

    UiRowDesc rowDesc = UI_ROW_DESC_INIT;
    UiElementHandle row = 0;
    svc_mng.ui->pane_add_row(ctx, left, &rowDesc, &row);
    UiControlDesc send = UI_CONTROL_DESC_INIT;
    send.kind = UI_CONTROL_BUTTON;
    send.label = "Send";
    send.on_pressed = send_chat;
    send.is_disabled = not_connected;
    svc_mng.ui->pane_add_control(ctx, row, &send, nullptr);
    UiGroupDesc picker = UI_GROUP_DESC_INIT;
    picker.label = "Emoji";
    picker.build = build_emoji_picker;
    svc_mng.ui->pane_add_group(ctx, row, right, &picker, nullptr);

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

// Hints tab
UiElementHandle g_hintsSummary = 0;
UiListHandle g_hintsList = 0;
UiElementHandle g_hintDetails = 0;
std::tuple<uint64_t, uint64_t, int> g_hintsShown{0, 0, -1};
std::vector<std::string> g_hintLabels;  // backing store for the list's labels

uint64_t hint_key(const Hint& h) {
    return (static_cast<uint64_t>(h.findingPlayer) << 48) ^ static_cast<uint64_t>(h.location);
}

const Hint* selected_hint() {
    for (const auto& h : g_hints) {
        if (hint_key(h) == g_selectedHint) {
            return &h;
        }
    }
    return nullptr;
}

const char* hint_status_name(const Hint& h) {
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

std::string hint_label(const Hint& h) {
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

std::string hints_summary() {
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

std::string hint_details_rml() {
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

void refresh_hints_list(ModContext* ctx) {
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
bool status_locked(ModContext*, void*) {
    const Hint* h = selected_hint();
    return h == nullptr || h->found || h->receivingPlayer != g_client.slot() ||
           g_client.state() != State::Connected;
}

void add_status_button(ModContext* ctx, UiElementHandle row, const char* label, int status) {
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

void ask_for_hint(const std::string& command, std::string& draft) {
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

    struct Ask {
        const char* label;
        const char* help;
        const char* button;
        const char* command;
        std::string* draft;
    };
    static const Ask asks[] = {
        {"Item", "The name of one of your items. The server says where it is, for the hint cost.",
            "Hint this item", "!hint ", &g_hintItemDraft},
        {"Location", "The name of one of your locations. The server says what's there.",
            "Hint this location", "!hint_location ", &g_hintLocationDraft},
    };
    for (const auto& ask : asks) {
        UiControlDesc field = UI_CONTROL_DESC_INIT;
        field.kind = UI_CONTROL_STRING;
        field.label = ask.label;
        field.help_rml = ask.help;
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

    svc_mng.ui->pane_add_section(ctx, left, "Hints");
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

std::vector<overlay::Line> overlay_lines() {
    using overlay::ascii_only;
    constexpr uint32_t kWhite = 0xFFFFFFFF, kGreen = 0x6FD08CFF, kYellow = 0xE8C867FF,
                       kRed = 0xFA8072FF, kGray = 0xC8C8C8FF, kBlue = 0x9DB8FFFF;
    constexpr size_t kChars = 44;
    std::vector<overlay::Line> out;
    const auto now = std::chrono::steady_clock::now();
    const int64_t keep = cfg_int(g_ovKeep, 30);
    auto fresh = [&](const Recent& r) {
        return keep <= 0 || now - r.at < std::chrono::seconds(keep);
    };

    if (cfg_on(g_ovStatus)) {
        switch (g_client.state()) {
        case State::Connected: out.push_back({"Archipelago: connected", kGreen}); break;
        case State::Connecting:
        case State::Handshaking: out.push_back({"Archipelago: connecting...", kYellow}); break;
        default: out.push_back({"Archipelago: offline", kRed}); break;
        }
    }
    if (cfg_on(g_ovChecks) && g_haveSlot) {
        const auto n = region_counts(-1);
        std::string text = fmt::format("Checks {}/{}", n.done, n.total);
        if (g_logic && g_reachInputs != SIZE_MAX) {
            text += fmt::format("   {} in logic", n.inLogic);
        }
        out.push_back({text, kWhite});
        out.push_back({"", kGreen, n.total == 0 ? 0.0f : static_cast<float>(n.done) / n.total});
    }
    if (cfg_on(g_ovItems)) {
        int64_t left = std::clamp<int64_t>(cfg_int(g_ovItemCount, 3), 1, 8);
        for (auto it = g_recentItems.rbegin(); it != g_recentItems.rend() && left > 0; ++it) {
            if (fresh(*it)) {
                out.push_back({ascii_only(it->text, kChars), kBlue});
                --left;
            }
        }
    }
    if (cfg_on(g_ovHints) && g_client.state() == State::Connected) {
        const int me = g_client.slot();
        const Hint* top = nullptr;
        size_t open = 0;
        for (const auto& h : g_hints) {
            if (h.receivingPlayer == me && !h.found) {
                ++open;
                top = top == nullptr ? &h : top;  // g_hints is sorted, priority first
            }
        }
        if (open > 0) {
            out.push_back({fmt::format("Hints: {} open", open), kYellow});
            out.push_back({ascii_only(fmt::format("{}{} - {}", top->status == 30 ? "! " : "",
                                          g_client.itemName(top->item, top->receivingPlayer),
                                          g_client.locationName(top->location, top->findingPlayer)),
                               kChars),
                kGray});
        }
    }
    if (cfg_on(g_ovChat)) {
        std::vector<const Recent*> lines;
        for (auto it = g_recentChat.rbegin(); it != g_recentChat.rend() && lines.size() < 3; ++it) {
            if (fresh(*it)) {
                lines.push_back(&*it);
            }
        }
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {  // oldest of them first
            out.push_back({ascii_only((*it)->text, kChars), kGray});
        }
    }
    if (cfg_on(g_ovDeathLink) && death_link_on()) {
        out.push_back({"Death link on", kRed});
    }
    return out;
}

void post_meter2_draw(ModContext*, void*, void*, void*) {
    if (!cfg_on(g_ovEnabled) || !in_gameplay() || dComIfGp_isPauseFlag() || dScnPly_c::isPause()) {
        return;
    }
    overlay::Layout layout;
    layout.corner = static_cast<overlay::Corner>(std::clamp<int64_t>(cfg_int(g_ovCorner, 3), 0, 5));
    layout.offsetX = static_cast<float>(cfg_int(g_ovX, 12));
    layout.offsetY = static_cast<float>(cfg_int(g_ovY, 0));
    layout.scale = static_cast<float>(cfg_int(g_ovScale, 100)) / 100.0f;
    layout.backgroundAlpha =
        static_cast<uint8_t>(std::clamp<int64_t>(cfg_int(g_ovOpacity, 60), 0, 100) * 255 / 100);
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
    reg_int("overlayKeepSeconds", 30, g_ovKeep);
}

// Overlay tab: every control is bound straight to its config var.
void add_bound(ModContext* ctx, UiElementHandle pane, UiControlKind kind, const char* label,
    ConfigVarHandle var, const char* help = nullptr, int64_t min = 0, int64_t max = 0,
    int64_t step = 1, const char* suffix = nullptr) {
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
    add_bound(ctx, left, UI_CONTROL_SELECT, "Position", g_ovCorner);
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Distance from the side", g_ovX, nullptr, 0, 600, 5);
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Distance from top or bottom", g_ovY,
        "For the middle positions this moves the panel down (or up, below zero).", -400, 400, 5);
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
        "How many hints for your items are still open, and the one to look at first.");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Chat", g_ovChat, "The last few chat messages.");
    add_bound(ctx, left, UI_CONTROL_TOGGLE, "Death link", g_ovDeathLink,
        "A reminder while death link is on.");
    add_bound(ctx, left, UI_CONTROL_NUMBER, "Keep items and chat for", g_ovKeep,
        "0 keeps them until newer ones push them out.", 0, 300, 5, " s");
    return MOD_OK;
}

// ---------------------------------------------------------------------------------------
// Status window (menu bar tab)

std::string g_editServer;

void press_reconnect(ModContext*, void*) {
    if (!g_editServer.empty() && g_editServer != g_conn.server) {
        g_conn.server = g_editServer;
        if (g_phase == Phase::Playing) {
            write_conn();
        }
    }
    if (!g_conn.slot.empty()) {
        g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
    }
}

void press_disconnect(ModContext*, void*) {
    g_client.disconnect();
}

std::string status_text() {
    std::string s = status_line();
    if (g_phase == Phase::Playing) {
        s += fmt::format("\nItems received: {} / {}", g_state.received, g_serverItems.size());
        if (g_haveSlot) {
            s += fmt::format("\nChecks sent: {} / {}", g_checked.size(), g_locationIds.size());
        }
        if (g_state.transformAnywhere) {
            s += "\nTransform anywhere: on (from your YAML)";
        }
        if (g_state.goal) {
            s += "\nGoal complete!";
        }
        if (g_needsRegen) {
            s += "\nWaiting to rebuild this save's seed from the server.";
        }
    }
    return s;
}

ModResult build_status_tab(ModContext* ctx, UiWindowHandle, UiElementHandle left, UiElementHandle,
    void*, ModError*) {
    g_editServer = g_conn.server;
    svc_mng.ui->pane_add_section(ctx, left, "Connection");
    g_statusWindowText = 0;
    svc_mng.ui->pane_add_text(ctx, left, status_text().c_str(), &g_statusWindowText);
    UiControlDesc d = UI_CONTROL_DESC_INIT;
    d.kind = UI_CONTROL_STRING;
    d.label = "Server";
    d.help_rml = "Change this if the room moved to a new port, then press Reconnect.";
    d.get = get_str;
    d.set = set_str;
    d.user_data = &g_editServer;
    svc_mng.ui->pane_add_control(ctx, left, &d, nullptr);
    UiControlDesc b = UI_CONTROL_DESC_INIT;
    b.kind = UI_CONTROL_BUTTON;
    b.label = "Reconnect";
    b.on_pressed = press_reconnect;
    svc_mng.ui->pane_add_control(ctx, left, &b, nullptr);
    UiControlDesc goal = UI_CONTROL_DESC_INIT;
    goal.kind = UI_CONTROL_BUTTON;
    goal.label = "Send goal complete";
    goal.help_rml = "Tells the server you finished the game, in case it wasn't detected.";
    goal.on_pressed = [](ModContext*, void*) { complete_goal("manual"); };
    goal.is_disabled = [](ModContext*, void*) {
        return g_state.goal || g_client.state() != State::Connected;
    };
    svc_mng.ui->pane_add_control(ctx, left, &goal, nullptr);
    UiControlDesc dl = UI_CONTROL_DESC_INIT;
    dl.kind = UI_CONTROL_TOGGLE;
    dl.label = "Death link";
    dl.help_rml = "When anyone else with death link dies, so do you, and the other way round. "
                  "Starts from your YAML; changing it here sticks to this save.";
    dl.get = [](ModContext*, void*, UiControlValue* out) { out->bool_value = death_link_on(); };
    dl.set = [](ModContext*, void*, const UiControlValue* v) {
        g_state.deathLink = v->bool_value ? 1 : 0;
        write_state();
        apply_death_link_tags();
    };
    dl.is_disabled = [](ModContext*, void*) { return g_phase != Phase::Playing; };
    svc_mng.ui->pane_add_control(ctx, left, &dl, nullptr);
    UiControlDesc b2 = UI_CONTROL_DESC_INIT;
    b2.kind = UI_CONTROL_BUTTON;
    b2.label = "Disconnect";
    b2.on_pressed = press_disconnect;
    svc_mng.ui->pane_add_control(ctx, left, &b2, nullptr);
    return MOD_OK;
}

ModResult update_status_tab(ModContext* ctx, void*, ModError*) {
    if (g_statusWindowText != 0) {
        static std::string shown;
        const std::string now = status_text();
        if (now != shown) {
            shown = now;
            svc_mng.ui->elem_set_text(ctx, g_statusWindowText, now.c_str());
        }
    }
    return MOD_OK;
}

void open_status_window(ModContext*, void*) {
    static UiTabDesc tabs[5] = {UI_TAB_DESC_INIT, UI_TAB_DESC_INIT, UI_TAB_DESC_INIT,
        UI_TAB_DESC_INIT, UI_TAB_DESC_INIT};
    tabs[0].title = "Status";
    tabs[0].build = build_status_tab;
    tabs[0].update = update_status_tab;
    tabs[1].title = "Tracker";
    tabs[1].build = build_tracker_tab;
    tabs[1].update = update_tracker_tab;
    tabs[2].title = "Hints";
    tabs[2].build = build_hints_tab;
    tabs[2].update = update_hints_tab;
    tabs[3].title = "Messages";
    tabs[3].build = build_messages_tab;
    tabs[3].update = update_messages_tab;
    tabs[4].title = "Overlay";
    tabs[4].build = build_overlay_tab;
    // The picker's buttons show their emoji as a background image.
    static const std::string rcss = [] {
        std::string out = kWindowRcss;
        out += ".ap-emoji-btn { padding-left: 42dp; font-size: 15dp; text-align: left; }\n";
        for (const auto& e : emoji::picker()) {
            out += fmt::format(".ap-e-{} {{ decorator: image({} contain left center); }}\n", e.file,
                emoji::image_source(e.file));
        }
        return out;
    }();
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = tabs;
    desc.tab_count = 5;
    desc.rcss = rcss.c_str();
    desc.on_closed = [](ModContext*, UiWindowHandle, void*) {
        g_statusWindow = 0;
        g_statusWindowText = 0;
        g_trackerSummary = g_trackerProgress = g_trackerList = 0;
        g_trackerGroups.clear();
        g_logElem = 0;
        g_hintsSummary = g_hintDetails = 0;
        g_hintsList = 0;
    };
    svc_mng.ui->window_push(svc_mng.mod_ctx, &desc, &g_statusWindow);
}

// Auto-reconnect
int g_reconnectFrames = 0;

}  // namespace

// =========================================================================================

GameModeDesc game_mode_desc() {
    return GameModeDesc{
        .struct_size = sizeof(GameModeDesc),
        .game_mode_id = kGameModeId,
        .full_name = "Archipelago",
        .save_name = "archipelago",
        .user_data = nullptr,
        .on_save_loaded = on_save_loaded,
        .on_new_save = on_new_save,
        .on_game_reset = on_game_reset,
    };
}

ModResult activate() {
    g_client.onConnected = on_connected;
    g_client.onItems = on_items;
    g_client.onHints = on_hints;
    g_client.onPrint = on_print;
    g_client.onDisconnected = on_disconnected;
    g_client.onBounced = on_bounced;

    auto reg_string = [](const char* name, ConfigVarHandle& out) {
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = name;
        d.type = CONFIG_VAR_STRING;
        d.default_string = "";
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &out);
    };
    if (g_cfgServer == 0) {
        reg_string("lastServer", g_cfgServer);
        reg_string("lastSlot", g_cfgSlot);
        ConfigVarDesc d = CONFIG_VAR_DESC_INIT;
        d.name = "apItemModelScale";
        d.type = CONFIG_VAR_FLOAT;
        d.default_float = 0.6;
        svc_mng.config->register_var(svc_mng.mod_ctx, &d, &g_cfgModelScale);
        ConfigVarDesc debug = CONFIG_VAR_DESC_INIT;
        debug.name = "debugLog";
        debug.type = CONFIG_VAR_BOOL;
        debug.default_bool = false;
        svc_mng.config->register_var(svc_mng.mod_ctx, &debug, &g_cfgDebugLog);
        register_overlay_vars();
    }

    svc_mng.item->observe_gives(svc_mng.mod_ctx, observe_give, nullptr, &g_observer);

    if (mods::hook::add_post<ApDitemSetMtx>(post_ditem_set_mtx) != MOD_OK ||
        mods::hook::add_post<ApItemSetBaseMtx>(post_item_set_base_mtx) != MOD_OK ||
        mods::hook::add_post<ApGanondorf>(post_ganondorf_execute) != MOD_OK ||
        mods::hook::add_pre<ApChangeScene>(pre_change_scene) != MOD_OK)
    {
        mods::log::error("archipelago: failed to install hooks");
        return MOD_ERROR;
    }
    // Separate from the hooks above: if these ever fail to resolve on a future Dusklight,
    // lose death link rather than the whole mode.
    if (mods::hook::add_post<ApLinkDeadInit>(post_link_dead_init) != MOD_OK ||
        mods::hook::add_post<ApLinkFogDeadInit>(post_link_fog_dead_init) != MOD_OK)
    {
        mods::log::error("archipelago: death link hooks failed to install; deaths won't be sent");
    }
    if (mods::hook::add_post<ApMidnaSearchNpc>(post_midna_search_npc) != MOD_OK ||
        mods::hook::add_post<ApMsgQuery042>(post_msg_query042) != MOD_OK)
    {
        mods::log::error("archipelago: transform anywhere hooks failed to install; turn on "
                         "Dusklight's Can Transform Anywhere cheat instead");
    }
    if (mods::hook::add_post<ApMeter2Draw>(post_meter2_draw) != MOD_OK) {
        mods::log::error("archipelago: overlay hook failed to install; the overlay won't show");
    }

    UiMenuTabDesc tab = UI_MENU_TAB_DESC_INIT;
    tab.label = "Archipelago";
    tab.on_selected = open_status_window;
    svc_mng.ui->register_menu_tab(svc_mng.mod_ctx, &tab, &g_menuTab);
    return MOD_OK;
}

void deactivate() {
    g_client.disconnect();
    reset_tracker();
    g_log.clear();
    g_hints.clear();
    g_selectedHint = 0;
    ++g_hintsVersion;
    g_textOverrides.clear();
    if (g_resolver != 0) {
        svc_mng.item->clear_check_resolver(svc_mng.mod_ctx, g_resolver);
        g_resolver = 0;
    }
    if (g_observer != 0) {
        svc_mng.item->unobserve_gives(svc_mng.mod_ctx, g_observer);
        g_observer = 0;
    }
    mods::hook::uninstall<ApDitemSetMtx>();
    mods::hook::uninstall<ApItemSetBaseMtx>();
    mods::hook::uninstall<ApChangeScene>();
    mods::hook::uninstall<ApGanondorf>();
    mods::hook::uninstall<ApLinkDeadInit>();
    mods::hook::uninstall<ApLinkFogDeadInit>();
    mods::hook::uninstall<ApMidnaSearchNpc>();
    mods::hook::uninstall<ApMsgQuery042>();
    mods::hook::uninstall<ApMeter2Draw>();
    g_pendingDeath.reset();
    g_killFrames = 0;
    g_deathSent = false;
    g_deathLinkSlot = false;
    g_transformAnywhereSlot = false;
    if (g_menuTab != 0) {
        svc_mng.ui->unregister_menu_tab(svc_mng.mod_ctx, g_menuTab);
        g_menuTab = 0;
    }
    g_phase = Phase::Idle;
}

void update() {
    g_client.poll();
    tick_generation();
}

void tick() {
    if (g_armedFrames > 0) {
        --g_armedFrames;
    }

    if (g_phase == Phase::Playing) {
        // Keep an AP save connected: retry every ~10 s after a drop (not after a refusal).
        if (g_client.state() == State::Disconnected && !g_conn.slot.empty()) {
            if (++g_reconnectFrames > 600) {
                g_reconnectFrames = 0;
                g_client.connect({g_conn.server, g_conn.slot, g_conn.password});
            }
        } else {
            g_reconnectFrames = 0;
        }
        log_stage_changes();
        scan_locations();
        flush_checks();
        deliver_items();
        tick_death_link();
    }
    tick_tracker();
}

ModResult open_connect_gate(void* fileSelect) {
    return open_gate_window(fileSelect);
}

void on_get_item_demo(void* link) {
    auto* alink = static_cast<daAlink_c*>(link);
    if (alink->field_0x32cc != 0) {
        return;
    }
    if (alink->mProcVar2.field_0x300c != kApItem) {
        if (alink->mProcVar2.field_0x300c == dItemNo_Randomizer_FOOLISH_ITEM_e) {
            g_armedFrames = 0;  // a real Foolish Item uses the same message
        }
        return;
    }
    // Always re-arm from the check being collected right now; the placeholder item has no
    // message of its own, so it must never fall back to whatever was shown last.
    g_armedText.clear();
    if (!g_lastResolvedApLocation.empty()) {
        if (const auto it = g_apItemText.find(g_lastResolvedApLocation); it != g_apItemText.end()) {
            g_armedText = it->second;
        }
    }
    if (g_armedText.empty()) {
        g_armedText = "You found another player's item!";
    }
    g_armedFrames = 600;
    alink->field_0x32cc = kApItemDonorMessage;
}

}  // namespace ap
