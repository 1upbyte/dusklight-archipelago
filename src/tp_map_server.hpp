#pragma once

#include <mods/svc/net.hpp>
#include <mods/svc/ui.h>

namespace tp_map_server {
void start();
void stop();
void tick();
void on_net_event(const mods::net::Event& event);
ModResult add_port_control(ModContext* ctx, UiElementHandle pane);
}
