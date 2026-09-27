#include "ap_covers.hpp"

#include "cover_art.hpp"

#include "../paths.hpp"

#include "JSystem/JUtility/JUTTexture.h"

#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXTexture.h>
#include <mods/svc/http.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>

namespace ap::covers {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// SteamGridDB found nothing for a game: ask again after this long, since art gets added.
constexpr int64_t kMissingRetrySeconds = 3 * 24 * 3600;
constexpr int kMaxAttempts = 3;  // network failures before a game is given up for the session
// Bumped when matching improves: "no art" answers from an older matcher are asked again.
constexpr int kMatcher = 2;

enum class State { Idle, Queued, Decoding, Ready, Missing, Failed };

struct Entry {
    State state = State::Idle;
    int attempts = 0;
    std::unique_ptr<Cover> cover;
};

std::unordered_map<std::string, Entry> g_entries;
std::deque<std::string> g_queue;  // games waiting for a download, in order
json g_index = json::object();    // game -> {"file", "id", "time"} or {"missing": true, "time"}
bool g_indexLoaded = false;
std::string g_key;
bool g_enabled = true;
bool g_keyRefused = false;        // until the key changes
std::string g_problem;
Clock::time_point g_retryAt{};
uint64_t g_generation = 1;        // bumped by refetch(): stale results are dropped
std::vector<std::unique_ptr<Cover>> g_retired;  // kept a few frames after refetch()
int g_retireFrames = 0;

// One download at a time, three steps per game: find the game, pick its art, fetch the image.
struct Fetch {
    enum class Step { Search, SearchAgain, Grids, GridsAny, Image };
    std::string game;
    Step step = Step::Search;
    int64_t gameId = 0;
    uint64_t token = 0;
    HttpRequestHandle handle = 0;
    bool done = false;
    HttpError error = HTTP_ERROR_NONE;
    int status = 0;
    std::string message;
    std::string body;
    std::string imageExt;
};
std::optional<Fetch> g_fetch;
uint64_t g_nextToken = 1;

struct Decode {
    std::string game;
    uint64_t generation = 0;
    std::future<std::optional<cover_art::Image>> result;
};
std::vector<Decode> g_decodes;

fs::path utf8_path(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

fs::path cache_dir() {
    return randomizer::paths::GetRandomizerPath() / "archipelago" / "covers";
}

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void load_index() {
    if (g_indexLoaded) {
        return;
    }
    g_indexLoaded = true;
    std::ifstream in(cache_dir() / "index.json");
    if (!in) {
        return;
    }
    try {
        json j = json::parse(in);
        if (j.is_object() && j.value("version", 0) == 1 && j.contains("games") &&
            j["games"].is_object()) {
            g_index = std::move(j["games"]);
            if (j.value("matcher", 1) < kMatcher) {
                for (auto it = g_index.begin(); it != g_index.end();) {
                    it = it->is_object() && it->value("missing", false) ? g_index.erase(it) : std::next(it);
                }
            }
        }
    } catch (const std::exception&) {
        g_index = json::object();  // a broken index just means downloading again
    }
}

void save_index() {
    std::error_code ec;
    fs::create_directories(cache_dir(), ec);
    const fs::path tmp = cache_dir() / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << json{{"version", 1}, {"matcher", kMatcher}, {"games", g_index}}.dump(2);
    }
    fs::rename(tmp, cache_dir() / "index.json", ec);
}

std::optional<std::vector<uint8_t>> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

void start_decode(const std::string& game, std::vector<uint8_t> bytes) {
    g_entries[game].state = State::Decoding;
    g_decodes.push_back({game, g_generation,
        std::async(std::launch::async, [bytes = std::move(bytes)]() {
            return cover_art::process(bytes.data(), bytes.size());
        })});
}

void start_decode_file(const std::string& game, fs::path path) {
    g_entries[game].state = State::Decoding;
    g_decodes.push_back({game, g_generation,
        std::async(std::launch::async, [path = std::move(path)]() -> std::optional<cover_art::Image> {
            const auto bytes = read_file(path);
            if (!bytes) {
                return std::nullopt;
            }
            return cover_art::process(bytes->data(), bytes->size());
        })});
}

// A cover the player put in covers/custom/ wins: "<exact game name>.png" or ".jpg".
std::optional<fs::path> custom_cover(const std::string& game) {
    std::error_code ec;
    for (const char* ext : {".png", ".jpg", ".jpeg"}) {
        for (const std::string& stem : {game, cover_art::file_stem(game)}) {
            const fs::path p = cache_dir() / "custom" / utf8_path(stem + ext);
            if (fs::is_regular_file(p, ec)) {
                return p;
            }
        }
    }
    return std::nullopt;
}

// Where a game's cover comes from, now that we want it.
void consider(const std::string& game) {
    load_index();
    Entry& e = g_entries[game];
    if (const auto custom = custom_cover(game)) {
        start_decode_file(game, *custom);
        return;
    }
    const json info = g_index.value(game, json::object());
    std::error_code ec;
    if (info.contains("file") && info["file"].is_string()) {
        const fs::path file = cache_dir() / utf8_path(info["file"].get<std::string>());
        if (fs::is_regular_file(file, ec)) {
            start_decode_file(game, file);
            return;
        }
    }
    if (info.value("missing", false) && unix_now() - info.value("time", int64_t{0}) < kMissingRetrySeconds) {
        e.state = State::Missing;
        return;
    }
    e.state = State::Queued;
    g_queue.push_back(game);
}

void on_http_done(ModContext*, HttpRequestHandle, const HttpResult* result, void* user) {
    const auto token = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user));
    if (!g_fetch || g_fetch->token != token) {
        return;  // cancelled by refetch()
    }
    g_fetch->done = true;
    g_fetch->error = result->error;
    g_fetch->status = result->status_code;
    g_fetch->message = result->error_message != nullptr ? result->error_message : "";
    if (result->body != nullptr && result->body_size > 0) {
        g_fetch->body.assign(static_cast<const char*>(result->body), result->body_size);
    } else {
        g_fetch->body.clear();
    }
}

bool request(const std::string& url, bool withKey) {
    if (svc_http == nullptr) {
        g_problem = "This version of Dusklight can't download covers.";
        return false;
    }
    const std::string bearer = "Bearer " + g_key;
    HttpHeader headers[] = {{"Authorization", bearer.c_str()}, {"Accept", "application/json"}};
    HttpRequestDesc d = HTTP_REQUEST_DESC_INIT;
    d.method = HTTP_METHOD_GET;
    d.url = url.c_str();
    d.headers = withKey ? headers : nullptr;
    d.header_count = withKey ? 2u : 0u;
    d.connect_timeout_ms = 10000;
    d.idle_timeout_ms = 15000;
    d.total_timeout_ms = 30000;
    d.max_body_bytes = 8u * 1024 * 1024;
    g_fetch->token = g_nextToken++;
    g_fetch->done = false;
    g_fetch->body.clear();
    const ModResult r = svc_http->request(mod_ctx, &d, on_http_done,
        reinterpret_cast<void*>(static_cast<uintptr_t>(g_fetch->token)), &g_fetch->handle);
    if (r != MOD_OK) {
        g_problem = "Couldn't start a download.";
        return false;
    }
    return true;
}

void mark_missing(const std::string& game) {
    g_index[game] = json{{"missing", true}, {"time", unix_now()}};
    save_index();
    g_entries[game].state = State::Missing;
}

void finish_fetch() {
    g_fetch.reset();
}

// A step's reply arrived: move on to the next step, or deal with the failure.
void advance() {
    Fetch& f = *g_fetch;
    const std::string game = f.game;
    Entry& e = g_entries[game];

    if (f.error != HTTP_ERROR_NONE || f.status == 0) {
        g_problem = "Couldn't reach SteamGridDB" + (f.message.empty() ? std::string{"."} : ": " + f.message);
        g_retryAt = Clock::now() + std::chrono::seconds(30);
        if (++e.attempts >= kMaxAttempts) {
            e.state = State::Failed;
        } else {
            g_queue.push_front(game);
        }
        finish_fetch();
        return;
    }
    if (f.status == 401 || f.status == 403) {
        if (f.step != Fetch::Step::Image) {
            g_keyRefused = true;
            g_problem = "SteamGridDB didn't accept the API key.";
            g_queue.push_front(game);
            finish_fetch();
            return;
        }
    }
    if (f.status == 429) {
        g_problem = "SteamGridDB asked us to slow down; trying again in a minute.";
        g_retryAt = Clock::now() + std::chrono::seconds(60);
        g_queue.push_front(game);
        finish_fetch();
        return;
    }
    if (f.status != 200) {
        if (f.status >= 500 && ++e.attempts < kMaxAttempts) {
            g_retryAt = Clock::now() + std::chrono::seconds(30);
            g_queue.push_front(game);
        } else if (f.status == 404) {
            mark_missing(game);
        } else {
            e.state = State::Failed;
        }
        finish_fetch();
        return;
    }
    g_problem.clear();

    json reply;
    if (f.step != Fetch::Step::Image) {
        try {
            reply = json::parse(f.body);
        } catch (const std::exception&) {
            e.state = State::Failed;
            finish_fetch();
            return;
        }
    }

    switch (f.step) {
    case Fetch::Step::Search:
    case Fetch::Step::SearchAgain: {
        const auto id = cover_art::pick_game(game, reply);
        if (!id) {
            const std::string shorter = cover_art::fallback_term(game);
            if (f.step == Fetch::Step::Search && !shorter.empty()) {
                // "Resident Evil 2 Remake" is listed as "Resident Evil 2": ask for that.
                f.step = Fetch::Step::SearchAgain;
                if (!request("https://www.steamgriddb.com/api/v2/search/autocomplete/" +
                                 cover_art::url_encode(shorter),
                        true)) {
                    e.state = State::Failed;
                    finish_fetch();
                }
                return;
            }
            mark_missing(game);
            finish_fetch();
            return;
        }
        f.gameId = *id;
        f.step = Fetch::Step::Grids;
        if (!request(fmt::format("https://www.steamgriddb.com/api/v2/grids/game/{}?dimensions="
                                 "600x900,342x482,660x930&types=static&nsfw=false&humor=false"
                                 "&epilepsy=false",
                         f.gameId),
                true)) {
            e.state = State::Failed;
            finish_fetch();
        }
        return;
    }
    case Fetch::Step::Grids:
    case Fetch::Step::GridsAny: {
        const std::string url = cover_art::pick_grid(reply);
        if (url.empty()) {
            if (f.step == Fetch::Step::Grids) {
                // No portrait art: any shape beats the Sol.
                f.step = Fetch::Step::GridsAny;
                if (!request(fmt::format("https://www.steamgriddb.com/api/v2/grids/game/{}"
                                         "?types=static&nsfw=false&humor=false&epilepsy=false",
                                 f.gameId),
                        true)) {
                    e.state = State::Failed;
                    finish_fetch();
                }
                return;
            }
            mark_missing(game);
            finish_fetch();
            return;
        }
        const auto dot = url.find_last_of('.');
        const std::string ext = dot != std::string::npos ? url.substr(dot) : std::string{};
        f.imageExt = ext == ".png" || ext == ".jpg" || ext == ".jpeg" ? ext : ".img";
        f.step = Fetch::Step::Image;
        if (!request(url, false)) {  // the image CDN gets no key
            e.state = State::Failed;
            finish_fetch();
        }
        return;
    }
    case Fetch::Step::Image: {
        std::vector<uint8_t> bytes(f.body.begin(), f.body.end());
        const std::string file = cover_art::file_stem(game) + f.imageExt;
        std::error_code ec;
        fs::create_directories(cache_dir(), ec);
        {
            std::ofstream out(cache_dir() / utf8_path(file), std::ios::binary | std::ios::trunc);
            out.write(f.body.data(), static_cast<std::streamsize>(f.body.size()));
        }
        g_index[game] = json{{"file", file}, {"id", f.gameId}, {"time", unix_now()}};
        save_index();
        finish_fetch();
        start_decode(game, std::move(bytes));
        return;
    }
    }
}

void start_next() {
    while (!g_queue.empty()) {
        const std::string game = g_queue.front();
        g_queue.pop_front();
        if (g_entries[game].state != State::Queued) {
            continue;
        }
        g_fetch.emplace();
        g_fetch->game = game;
        g_fetch->step = Fetch::Step::Search;
        const std::string url = "https://www.steamgriddb.com/api/v2/search/autocomplete/" +
                                cover_art::url_encode(cover_art::search_term(game));
        if (!request(url, true)) {
            g_queue.push_front(game);
            g_fetch.reset();
            g_retryAt = Clock::now() + std::chrono::seconds(30);
        }
        return;
    }
}

void poll_decodes() {
    for (auto it = g_decodes.begin(); it != g_decodes.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::optional<cover_art::Image> image = it->result.get();
        const std::string game = it->game;
        const bool current = it->generation == g_generation;
        it = g_decodes.erase(it);
        if (!current) {
            continue;
        }
        Entry& e = g_entries[game];
        if (!image) {
            // A cached file that doesn't decode: forget it so the next session downloads again.
            if (g_index.contains(game)) {
                g_index.erase(game);
                save_index();
            }
            e.state = State::Failed;
            continue;
        }
        auto cover = std::make_unique<Cover>();
        cover->texels = std::move(image->texels);
        cover->aspect = image->aspect;
        cover->spine = GXColor{image->spine[0], image->spine[1], image->spine[2], 255};
        GXInitTexObj(&cover->texture, cover->texels.data(), static_cast<u16>(image->width),
            static_cast<u16>(image->height), GX_TF_RGBA8_PC, GX_CLAMP, GX_CLAMP, GX_TRUE);
        GXInitTexObjLOD(&cover->texture, GX_LIN_MIP_LIN, GX_LINEAR, 0.0f,
            static_cast<float>(image->mips - 1), 0.0f, GX_FALSE, GX_TRUE, GX_ANISO_4);
        if (!image->icon.empty()) {
            cover->iconTimg.assign(sizeof(ResTIMG) + image->icon.size(), 0);
            auto* timg = reinterpret_cast<ResTIMG*>(cover->iconTimg.data());
            timg->format = GX_TF_RGBA8_PC;
            timg->alphaEnabled = 1;
            timg->width = static_cast<u16>(image->iconSize);
            timg->height = static_cast<u16>(image->iconSize);
            timg->wrapS = GX_CLAMP;
            timg->wrapT = GX_CLAMP;
            timg->mipmapEnabled = 1;
            timg->minFilter = GX_LIN_MIP_LIN;
            timg->magFilter = GX_LINEAR;
            timg->minLOD = 0;
            timg->maxLOD = static_cast<s8>((image->iconMips - 1) * 8);  // eighths
            timg->mipmapCount = static_cast<u8>(image->iconMips);
            timg->imageOffset = static_cast<s32>(sizeof(ResTIMG));
            std::memcpy(cover->iconTimg.data() + sizeof(ResTIMG), image->icon.data(), image->icon.size());
        }
        e.cover = std::move(cover);
        e.state = State::Ready;
    }
}

}  // namespace

void tick(const std::string& apiKey, bool enabled) {
    if (g_retireFrames > 0 && --g_retireFrames == 0) {
        g_retired.clear();
    }
    g_enabled = enabled;
    if (apiKey != g_key) {
        g_key = apiKey;
        g_keyRefused = false;
        g_problem.clear();
        g_retryAt = {};
    }
    poll_decodes();
    if (g_fetch && g_fetch->done) {
        advance();
    }
    if (!g_fetch && g_enabled && !g_key.empty() && !g_keyRefused && Clock::now() >= g_retryAt) {
        start_next();
    }
}

void want(const std::vector<std::string>& games) {
    for (const auto& game : games) {
        if (!cover_art::has_box(game)) {
            continue;
        }
        const auto it = g_entries.find(game);
        if (it == g_entries.end() || it->second.state == State::Idle) {
            consider(game);
        }
    }
}

const Cover* get(const std::string& game) {
    if (!g_enabled) {
        return nullptr;
    }
    const auto it = g_entries.find(game);
    return it != g_entries.end() && it->second.state == State::Ready ? it->second.cover.get() : nullptr;
}

std::string status() {
    int ready = 0;
    int pending = 0;
    int missing = 0;
    for (const auto& [game, e] : g_entries) {
        switch (e.state) {
        case State::Ready: ++ready; break;
        case State::Queued:
        case State::Decoding: ++pending; break;
        case State::Missing:
        case State::Failed: ++missing; break;
        default: break;
        }
    }
    std::string s;
    if (g_entries.empty()) {
        s = "Covers load when you connect to a room.";
    } else {
        s = fmt::format("Covers ready: {} of {}.", ready, g_entries.size());
        if (pending > 0) {
            s += fmt::format(" Downloading {}.", pending);
        }
        if (missing > 0) {
            s += fmt::format(" No art for {} (they stay Sols).", missing);
        }
    }
    if (pending > 0 && g_key.empty()) {
        s += " Add a SteamGridDB API key to download the rest.";
    }
    if (!g_problem.empty()) {
        s += " " + g_problem;
    }
    return s;
}

void refetch() {
    if (g_fetch && g_fetch->handle != 0 && svc_http != nullptr) {
        svc_http->cancel(mod_ctx, g_fetch->handle);
    }
    g_fetch.reset();
    g_queue.clear();
    ++g_generation;
    load_index();
    std::error_code ec;
    for (const auto& [game, info] : g_index.items()) {
        if (info.contains("file") && info["file"].is_string()) {
            fs::remove(cache_dir() / utf8_path(info["file"].get<std::string>()), ec);
        }
    }
    g_index = json::object();
    save_index();
    g_keyRefused = false;
    g_problem.clear();
    g_retryAt = {};
    std::vector<std::string> games;
    for (auto& [game, e] : g_entries) {
        if (e.cover) {
            g_retired.push_back(std::move(e.cover));  // a draw list may still point at it
        }
        e = Entry{};
        games.push_back(game);
    }
    g_retireFrames = 4;
    want(games);
}

}  // namespace ap::covers
