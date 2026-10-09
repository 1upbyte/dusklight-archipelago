#include "tp_map_server.hpp"

#include "session.hpp"
#include "ui/tracker.hpp"
#include "randomizer_context.hpp"
#include "ap/ap_mode.hpp"

#include <mods/svc/log.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tp_map_server {
namespace {
using randomizer::session::svc_mng;
using json = nlohmann::json;

constexpr int64_t kDefaultPort = 38282;
constexpr size_t kChunk = 128 * 1024;
constexpr size_t kRequestLimit = 8192;

struct Client {
    std::string request;
    std::string response;
    size_t sent = 0;
    bool ready = false;
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
};

NetHandle listener = 0;
ConfigVarHandle portOption = 0;
int64_t attemptedPort = -1;
std::unordered_map<NetHandle, Client> clients;
std::string snapshot = R"({"seed":"","checks":[]})";
int frames = 0;

std::string mime_type(std::string_view path) {
    auto dot = path.rfind('.');
    std::string_view ext = dot == std::string_view::npos ? "" : path.substr(dot);
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf" || ext == ".TTF") return "font/ttf";
    if (ext == ".otf") return "font/otf";
    return "application/octet-stream";
}

bool valid_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find("..") != path.npos ||
        path.find("//") != path.npos ||
        path.find('\\') != path.npos || path.find('%') != path.npos || path.find(':') != path.npos)
        return false;
    for (unsigned char c : path) {
        if (c < 0x20 || c >= 0x7f) return false;
    }
    return true;
}

std::string response(int status, std::string_view type, std::string body) {
    const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason +
        "\r\nContent-Type: " + std::string(type) +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n\r\n";
    header += body;
    return header;
}

void prepare(Client& client) {
    const size_t end = client.request.find("\r\n");
    if (end == std::string::npos) return;
    const std::string line = client.request.substr(0, end);
    if (!line.starts_with("GET ") || !line.ends_with(" HTTP/1.1")) {
        client.response = response(400, "text/plain", "Bad request\n");
    } else {
        std::string path = line.substr(4, line.size() - 4 - 9);
        if (const size_t query = path.find('?'); query != std::string::npos) path.resize(query);
        if (!valid_path(path)) {
            client.response = response(400, "text/plain", "Bad path\n");
        } else if (path == "/api/checks") {
            client.response = response(200, "application/json; charset=utf-8", snapshot);
        } else {
            if (path == "/") path = "/index.html";
            if (path.back() == '/') path += "index.html";
            std::string resource = path == "/tp-map-sync.js"
                ? "tp-map-sync.js" : "tp-map" + path;
            ResourceBuffer buffer = RESOURCE_BUFFER_INIT;
            if (svc_mng.resource->load(svc_mng.mod_ctx, resource.c_str(), &buffer) != MOD_OK) {
                client.response = response(404, "text/plain", "Not found\n");
            } else {
                std::string body(static_cast<const char*>(buffer.data), buffer.size);
                svc_mng.resource->free(svc_mng.mod_ctx, &buffer);
                if (path == "/index.html") {
                    const size_t pos = body.rfind("</body>");
                    if (pos != std::string::npos)
                        body.insert(pos, "<script src=\"/tp-map-sync.js\"></script>\n");
                }
                client.response = response(200, mime_type(path), std::move(body));
            }
        }
    }
    client.request.clear();
    client.ready = true;
}

void send_pending(NetHandle handle, Client& client) {
    if (!client.ready) return;
    NetStats stats = NET_STATS_INIT;
    if (svc_net->stats(svc_mng.mod_ctx, handle, &stats) != MOD_OK) return;
    while (client.sent < client.response.size() && stats.queued_send_bytes < 512 * 1024) {
        const size_t n = std::min(kChunk, client.response.size() - client.sent);
        if (svc_net->send(svc_mng.mod_ctx, handle, client.response.data() + client.sent, n) != MOD_OK)
            return;
        client.sent += n;
        stats.queued_send_bytes += n;
        client.lastActivity = std::chrono::steady_clock::now();
    }
    if (client.sent == client.response.size()) {
        svc_net->close(svc_mng.mod_ctx, handle); // NetService flushes queued data before closing.
        clients.erase(handle);
    }
}

void refresh_snapshot() {
    std::string seed;
    std::vector<std::string> checks;
    if (!ap::tp_map_snapshot(seed, checks) && randomizer::session::g_seedActivated) {
        seed = randomizer_GetContext().mHash;
        auto& tracker = randomizer::ui::g_tracker;
        tracker.generateLocationInfo();
        for (const auto& location : tracker.m_LocationInfo) {
            if (tracker.isLocationObtained(location)) checks.push_back(location.name);
        }
    }
    snapshot = json{{"seed", seed}, {"checks", checks}}.dump();
}
void ensure_port_option() {
    if (portOption == 0) {
        ConfigVarDesc option = CONFIG_VAR_DESC_INIT;
        option.name = "tpMapPort";
        option.type = CONFIG_VAR_INT;
        option.default_int = kDefaultPort;
        if (svc_mng.config->register_var(svc_mng.mod_ctx, &option, &portOption) != MOD_OK) {
            mods::log::warn("tp-map: could not register port option; using {}", kDefaultPort);
        }
    }
}
} // namespace

void start() {
    ensure_port_option();
    tick();
}

void bind_port(int64_t port) {
    attemptedPort = port;
    if (port < 1 || port > 65535) {
        mods::log::warn("tp-map: invalid port {} (choose 1-65535)", port);
        return;
    }
    const std::string bind = "tcp://0.0.0.0:" + std::to_string(port);
    NetListenDesc desc = NET_LISTEN_DESC_INIT;
    desc.bind = bind.c_str();
    NetHandle nextListener = 0;
    NetEndpoint local{};
    NetError error = NET_ERROR_NONE;
    if (svc_net->listen(svc_mng.mod_ctx, &desc, &nextListener, &local, &error) != MOD_OK) {
        mods::log::warn("tp-map: could not listen on 0.0.0.0:{} (net error {})", port, static_cast<int>(error));
        return;
    }
    stop();
    listener = nextListener;
    attemptedPort = port;
    mods::log::info("tp-map listening on 0.0.0.0:{} (open http://127.0.0.1:{}/ locally)", port, port);
}

void stop() {
    for (const auto& [handle, _] : clients) svc_net->close(svc_mng.mod_ctx, handle);
    clients.clear();
    if (listener != 0) svc_net->close(svc_mng.mod_ctx, listener);
    listener = 0;
    attemptedPort = -1;
}

void tick() {
    int64_t port = kDefaultPort;
    if (portOption != 0) svc_mng.config->get_int(svc_mng.mod_ctx, portOption, &port);
    if (port != attemptedPort) bind_port(port);
    if (listener == 0) return;
    if (++frames >= 30) {
        frames = 0;
        refresh_snapshot();
    }
    // send_pending may erase its client.
    for (auto it = clients.begin(); it != clients.end();) {
        const NetHandle handle = it++->first;
        if (auto found = clients.find(handle); found != clients.end()) {
            if (std::chrono::steady_clock::now() - found->second.lastActivity >
                std::chrono::seconds(10)) {
                svc_net->close(svc_mng.mod_ctx, handle);
                clients.erase(found);
            } else {
                send_pending(handle, found->second);
            }
        }
    }
}

ModResult add_port_control(ModContext* ctx, UiElementHandle pane) {
    ensure_port_option();
    if (portOption == 0) return MOD_ERROR;
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_NUMBER;
    control.label = "Map port";
    control.help_rml = "Open http://127.0.0.1:&lt;port&gt;/ here, or use this computer's IP address from another device. Changes apply immediately.";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = portOption;
    control.min = 1;
    control.max = 65535;
    control.step = 1;
    return svc_mng.ui->pane_add_control(ctx, pane, &control, nullptr);
}

void on_net_event(const mods::net::Event& event) {
    if (listener == 0) return;
    if (event.type == NET_EVENT_ACCEPTED && event.handle == listener) {
        auto socket = mods::net::adopt(event.accepted);
        const NetHandle handle = socket.handle();
        socket.detach();
        clients.emplace(handle, Client{});
        return;
    }
    auto it = clients.find(event.handle);
    if (it == clients.end()) return;
    if (event.type == NET_EVENT_CLOSED) {
        clients.erase(it);
    } else if (event.type == NET_EVENT_STREAM_DATA && !it->second.ready) {
        it->second.lastActivity = std::chrono::steady_clock::now();
        it->second.request.append(reinterpret_cast<const char*>(event.data.data()), event.data.size());
        if (it->second.request.size() > kRequestLimit) {
            it->second.response = response(400, "text/plain", "Request too large\n");
            it->second.ready = true;
        } else if (it->second.request.find("\r\n\r\n") != std::string::npos) {
            prepare(it->second);
        }
    }
}
} // namespace tp_map_server
