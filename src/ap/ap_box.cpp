#include "ap_box.hpp"

#include "ap_covers.hpp"

#include "JSystem/J3DGraphAnimator/J3DJoint.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.h"
#include "JSystem/J3DGraphBase/J3DPacket.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DShapeMtx.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "d/actor/d_a_itembase.h"
#include "d/d_kankyo.h"

#include <dolphin/gx.h>
#include <mods/svc/hook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <unordered_map>

namespace ap::box {
namespace {

// Box shape, relative to the model it replaces: this much taller than that model is big, a
// game case's depth, and art no wider or narrower than a box plausibly is.
constexpr float kHeightOfModel = 1.35f;
constexpr float kDepthOfHeight = 0.14f;
constexpr float kMinAspect = 0.5f;
constexpr float kMaxAspect = 1.9f;
constexpr uint32_t kForgetAfterTicks = 120;
// In the world (not held): this much of the held size, leaning back by this much.
constexpr float kWorldSize = 0.8f;
constexpr float kWorldTiltDegrees = 20.0f;

// Frame interpolation lives in dusk::interp, which isn't exported: look it up by name. Without
// it the box still draws, just at the game's own 30 Hz steps.
using RecordFn = void (*)(float (*)[4], const void*);
using LookupFn = bool (*)(const void*, float (*)[4]);
using ForgetFn = void (*)(const void*);
RecordFn g_record = nullptr;
LookupFn g_lookup = nullptr;
ForgetFn g_forget = nullptr;

void* resolve(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        void* address = nullptr;
        if (svc_hook != nullptr && svc_hook->resolve(mod_ctx, name, &address, nullptr) == MOD_OK &&
            address != nullptr) {
            return address;
        }
    }
    return nullptr;
}

void resolve_interp() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    g_record = reinterpret_cast<RecordFn>(resolve({"?record_final_mtx@interp@dusk@@YAXQEAY03MPEBX@Z",
        "_ZN4dusk6interp16record_final_mtxEPA4_fPKv"}));
    g_lookup = reinterpret_cast<LookupFn>(resolve({"?lookup_replacement@interp@dusk@@YA_NPEBXQEAY03M@Z",
        "_ZN4dusk6interp18lookup_replacementEPKvPA4_f"}));
    g_forget = reinterpret_cast<ForgetFn>(
        resolve({"?forget_mtx@interp@dusk@@YAXPEBX@Z", "_ZN4dusk6interp10forget_mtxEPKv"}));
    if (g_record == nullptr || g_lookup == nullptr || g_forget == nullptr) {
        g_record = nullptr;
        g_lookup = nullptr;
        g_forget = nullptr;
    }
}

// Plain white, for the edges: the vertex colour tints it, so one TEV setup draws everything.
uint8_t g_whiteTexels[4 * 4 * 4];
GXTexObj g_white;

void init_white() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    std::memset(g_whiteTexels, 0xFF, sizeof(g_whiteTexels));
    GXInitTexObj(&g_white, g_whiteTexels, 4, 4, GX_TF_RGBA8_PC, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&g_white, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

void concat(const float a[3][4], const float b[3][4], float out[3][4]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0f);
        }
    }
}

GXColor shade(GXColor c, float f) {
    return GXColor{static_cast<u8>(std::min(255.0f, c.r * f)), static_cast<u8>(std::min(255.0f, c.g * f)),
        static_cast<u8>(std::min(255.0f, c.b * f)), 255};
}

void vertex(float x, float y, float z, GXColor c, float s, float t) {
    GXPosition3f32(x, y, z);
    GXColor4u8(c.r, c.g, c.b, 255);
    GXTexCoord2f32(s, t);
}

class BoxPacket final : public J3DPacket {
public:
    void draw() override;

    Mtx model{};  // box space (centred on the replaced model) to world
    const covers::Cover* cover = nullptr;
    dKy_tevstr_c* tevStr = nullptr;
    float hw = 0.0f;
    float hh = 0.0f;
    float hd = 0.0f;
    uint32_t lastSeen = 0;
};

void BoxPacket::draw() {
    if (cover == nullptr) {
        return;
    }
    Mtx presented;
    MtxP world = model;
    if (g_lookup != nullptr && g_lookup(this, presented)) {
        world = presented;
    }
    Mtx view;
    concat(j3dSys.getViewMtx(), world, view);

    j3dSys.reinitGX();
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetZCompLoc(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetCullMode(GX_CULL_NONE);
    if (tevStr != nullptr) {
        dKy_GxFog_tevstr_set(tevStr);  // fades into fog like everything around it
    }

    // Faces: the cover on both big sides (reading correctly from each), edges darker.
    const GXColor front{255, 255, 255, 255};
    const GXColor back{225, 225, 225, 255};
    GXLoadTexObj(const_cast<GXTexObj*>(&cover->texture), GX_TEXMAP0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 8);
    vertex(-hw, hh, hd, front, 0.0f, 0.0f);
    vertex(hw, hh, hd, front, 1.0f, 0.0f);
    vertex(hw, -hh, hd, front, 1.0f, 1.0f);
    vertex(-hw, -hh, hd, front, 0.0f, 1.0f);
    vertex(hw, hh, -hd, back, 0.0f, 0.0f);
    vertex(-hw, hh, -hd, back, 1.0f, 0.0f);
    vertex(-hw, -hh, -hd, back, 1.0f, 1.0f);
    vertex(hw, -hh, -hd, back, 0.0f, 1.0f);
    GXEnd();

    const GXColor side = shade(cover->spine, 1.0f);
    const GXColor top = shade(cover->spine, 1.25f);
    const GXColor bottom = shade(cover->spine, 0.7f);
    GXLoadTexObj(&g_white, GX_TEXMAP0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 16);
    vertex(hw, hh, hd, side, 0.5f, 0.5f);
    vertex(hw, hh, -hd, side, 0.5f, 0.5f);
    vertex(hw, -hh, -hd, side, 0.5f, 0.5f);
    vertex(hw, -hh, hd, side, 0.5f, 0.5f);
    vertex(-hw, hh, -hd, side, 0.5f, 0.5f);
    vertex(-hw, hh, hd, side, 0.5f, 0.5f);
    vertex(-hw, -hh, hd, side, 0.5f, 0.5f);
    vertex(-hw, -hh, -hd, side, 0.5f, 0.5f);
    vertex(-hw, hh, -hd, top, 0.5f, 0.5f);
    vertex(hw, hh, -hd, top, 0.5f, 0.5f);
    vertex(hw, hh, hd, top, 0.5f, 0.5f);
    vertex(-hw, hh, hd, top, 0.5f, 0.5f);
    vertex(-hw, -hh, hd, bottom, 0.5f, 0.5f);
    vertex(hw, -hh, hd, bottom, 0.5f, 0.5f);
    vertex(hw, -hh, -hd, bottom, 0.5f, 0.5f);
    vertex(-hw, -hh, -hd, bottom, 0.5f, 0.5f);
    GXEnd();

    // Hand the pipeline back in the state J3D expects: it caches vertex formats and matrices.
    J3DShape::resetVcdVatCache();
    J3DShapeMtx::resetMtxLoadCache();
    j3dSys.reinitGX();
}

std::unordered_map<const void*, std::unique_ptr<BoxPacket>> g_packets;
uint32_t g_tick = 0;

}  // namespace

bool draw(daItemBase_c* item, const covers::Cover& cover, bool held) {
    J3DModel* model = item != nullptr ? item->mpModel : nullptr;
    if (model == nullptr || model->getModelData() == nullptr) {
        return false;
    }
    resolve_interp();
    init_white();

    auto& slot = g_packets[item];
    if (!slot) {
        slot = std::make_unique<BoxPacket>();
    }
    BoxPacket* p = slot.get();
    if (p->lastSeen + 2 < g_tick && g_forget != nullptr) {
        g_forget(p);  // a new item (or one back after a while): nothing to blend from
    }
    p->lastSeen = g_tick;
    p->cover = &cover;
    p->tevStr = &item->tevStr;

    // Size and centre from the model being replaced, so it sits where that did.
    float center[3] = {0.0f, 0.0f, 0.0f};
    float extent = 60.0f;
    if (J3DJoint* joint = model->getModelData()->getJointNodePointer(0)) {
        const Vec* lo = joint->getMin();
        const Vec* hi = joint->getMax();
        const float e = std::max({hi->x - lo->x, hi->y - lo->y, hi->z - lo->z});
        if (e > 1.0f && e < 10000.0f) {
            extent = e;
            center[0] = (lo->x + hi->x) * 0.5f;
            center[1] = (lo->y + hi->y) * 0.5f;
            center[2] = (lo->z + hi->z) * 0.5f;
        }
    }
    p->hh = extent * kHeightOfModel * 0.5f * (held ? 1.0f : kWorldSize);
    p->hw = p->hh * std::clamp(cover.aspect, kMinAspect, kMaxAspect);
    p->hd = p->hh * kDepthOfHeight;

    // The model's own placement (base matrix and scale), then over to its centre.
    const Vec* scale = model->getBaseScale();
    const float s[3] = {scale->x, scale->y, scale->z};
    Mtx& base = model->getBaseTRMtx();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            p->model[r][c] = base[r][c] * s[c];
        }
        p->model[r][3] = base[r][3] + p->model[r][0] * center[0] + p->model[r][1] * center[1] +
                         p->model[r][2] * center[2];
    }
    if (!held) {
        // Lean back: the top tips away from the front cover, about the box's centre, so it
        // tilts with the cover as the item turns.
        const float a = kWorldTiltDegrees * 3.14159265f / 180.0f;
        const float c = std::cos(a);
        const float sn = std::sin(a);
        for (int r = 0; r < 3; ++r) {
            const float y = p->model[r][1];
            const float z = p->model[r][2];
            p->model[r][1] = y * c - z * sn;
            p->model[r][2] = y * sn + z * c;
        }
    }
    if (g_record != nullptr) {
        g_record(p->model, p);
    }

    // DrawBase without the model: lighting, list, box, shadow. calc() keeps the model's joints
    // current for the shadow, which is still cast from it.
    item->setTevStr();
    model->calc();
    item->setListStart();
    j3dSys.getDrawBuffer(0)->entryImm(p, 0);
    item->setListEnd();
    item->setShadow();
    return true;
}

void tick() {
    ++g_tick;
    for (auto it = g_packets.begin(); it != g_packets.end();) {
        if (it->second->lastSeen + kForgetAfterTicks < g_tick) {
            if (g_forget != nullptr) {
                g_forget(it->second.get());
            }
            it = g_packets.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace ap::box
