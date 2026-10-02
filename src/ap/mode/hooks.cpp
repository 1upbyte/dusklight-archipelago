// Gameplay hooks: the AP item model scale and Transform Anywhere.

#include "internal.hpp"

namespace ap::internal {

// Used only in this file; defined below.
static float model_scale();
static void scale_model(J3DModel* model);
static bool transform_anywhere_on();

static float model_scale() {
    double s = 0.3;
    if (g_cfgModelScale != 0) {
        svc_mng.config->get_float(svc_mng.mod_ctx, g_cfgModelScale, &s);
    }
    return static_cast<float>(s);
}

static void scale_model(J3DModel* model) {
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


// ---------------------------------------------------------------------------------------
// Transform Anywhere
//
// With the slot's Logic Transform Anywhere on, logic may expect Link to transform where an NPC
// can see him, which the game refuses unless Dusklight's Can Transform Anywhere cheat is on.
// Mods can't reach host settings, so do what that setting does, for this save only: the host
// checks it at both calls of daMidna_searchNpc, and in one Castle Town branch of query042 (the
// flow query behind talking to Midna about transforming).

static bool transform_anywhere_on() {
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

}  // namespace ap::internal
