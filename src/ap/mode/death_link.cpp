// Death link: sending our deaths, and dying when someone else does.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static bool link_is_dead(const daAlink_c* link);
static void on_link_died(daAlink_c* link, const char* how);


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

static bool link_is_dead(const daAlink_c* link) {
    return link->mProcID == daAlink_c::PROC_DEAD || link->mProcID == daAlink_c::PROC_FOG_DEAD;
}

static void on_link_died(daAlink_c* link, const char* how) {
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

}  // namespace ap::internal
