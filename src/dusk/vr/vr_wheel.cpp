#include "dusk/vr/vr_wheel.h"

#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_item_data.h"
#include "d/d_save.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "m_Do/m_Do_ext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dusk::vr::wheel {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kIconSize = 0.065f;   // meters
constexpr float kMinRadius = 0.11f;
constexpr float kDeadZone = 0.4f;     // fraction of the radius that counts as the middle
constexpr float kOpenSqueeze = 0.7f;
constexpr float kReleaseSqueeze = 0.35f;

// An item icon drawn in 3D: J2DPicture's own drawing (layers, tints, alpha) with a world matrix
// in place of its 2D one.
class WorldPicture : public J2DPicture {
public:
    explicit WorldPicture(const ResTIMG* img) : J2DPicture(img) {}

    void draw_world(const Mtx posMtx) {
        if (mTextureNum == 0 || mTexture[0] == nullptr) {
            return;
        }
        for (u8 i = 0; i < mTextureNum; i++) {
            load(i);
        }
        GXSetNumTexGens(mTextureNum);
        mColorAlpha = mAlpha;
        JUtility::TColor color[4];
        getNewColor(color);
        setTevMode();
        GXLoadPosMtxImm(posMtx, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        JGeometry::TVec2<s16> coords[4];
        setTexCoord(coords, mTexture[0], BIND15, MIRROR0, false);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_S16, 8);
        // A unit square centered on the origin, +Y up (texture rows run downward).
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition3f32(-0.5f, 0.5f, 0.0f);
        GXColor1u32(color[0]);
        GXTexCoord2s16(coords[0].x, coords[0].y);
        GXPosition3f32(0.5f, 0.5f, 0.0f);
        GXColor1u32(color[1]);
        GXTexCoord2s16(coords[1].x, coords[1].y);
        GXPosition3f32(0.5f, -0.5f, 0.0f);
        GXColor1u32(color[3]);
        GXTexCoord2s16(coords[3].x, coords[3].y);
        GXPosition3f32(-0.5f, -0.5f, 0.0f);
        GXColor1u32(color[2]);
        GXTexCoord2s16(coords[2].x, coords[2].y);
        GXEnd();
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_POS_XYZ, GX_U16, 15);
        GXSetVtxDesc(GX_VA_TEX0, GX_NONE);
    }
};

struct Entry {
    u8 slot = 0;
    u8 item = 0;
    std::array<ResTIMG*, 3> buffers{};
    std::array<WorldPicture*, 3> layers{};
    int layerCount = 0;
    float scale = 1.f;
};

std::vector<Entry> g_entries;
bool g_open = false;
Vec3 g_center, g_right, g_up, g_normal;
float g_radius = kMinRadius;
int g_hover = -1;
int g_chosen = -1;    // entry index waiting for the next sim tick
u8 g_chosenSlot = 0;
float g_openAnim = 0.f;

JKRHeap* heap() { return mDoExt_getGameHeap(); }

void free_entries() {
    for (auto& e : g_entries) {
        for (int i = 0; i < 3; ++i) {
            if (e.layers[i] != nullptr) {
                JKR_DELETE(e.layers[i]);
            }
            if (e.buffers[i] != nullptr) {
                heap()->free(e.buffers[i]);
            }
        }
    }
    g_entries.clear();
}

// Loads every item you own, in the order the game's own ring lists them.
bool load_entries() {
    free_entries();
    JKRHeap* previous = heap()->becomeCurrentHeap();
    for (int slot = SLOT_0; slot <= SLOT_23; ++slot) {
        u8 item = dComIfGs_getItem(slot, false);
        if (item == dItemNo_NONE_e) {
            continue;
        }
        if (item == dItemNo_LIGHT_ARROW_e) {
            item = dItemNo_BOW_e;  // the ring does the same: there is no light arrow icon
        }
        Entry e;
        e.slot = static_cast<u8>(slot);
        e.item = item;
        bool ok = true;
        for (auto& buf : e.buffers) {
            buf = static_cast<ResTIMG*>(heap()->alloc(0xC00, 0x20));
            ok = ok && buf != nullptr;
        }
        if (!ok) {
            g_entries.push_back(e);
            break;
        }
        e.layerCount = dMeter2Info_readItemTexture(item, e.buffers[0], nullptr, e.buffers[1],
            nullptr, e.buffers[2], nullptr, nullptr, nullptr, -1);
        e.layerCount = std::clamp(e.layerCount, 0, 3);
        for (int k = 0; k < e.layerCount; ++k) {
            e.layers[k] = JKR_NEW WorldPicture(e.buffers[k]);
        }
        dMeter2Info_setItemColor(item, e.layers[0], e.layers[1], e.layers[2], nullptr);
        e.scale = dItem_data::getTexScale(item) / 100.f;
        g_entries.push_back(e);
    }
    previous->becomeCurrentHeap();
    return !g_entries.empty();
}

Vec3 slot_position(int i) {
    const float angle = kPi * 0.5f - 2.f * kPi * static_cast<float>(i) / g_entries.size();
    return g_center + g_right * (std::cos(angle) * g_radius) + g_up * (std::sin(angle) * g_radius);
}

int hovered_entry(const Vec3& hand) {
    const Vec3 d = hand - g_center;
    const float x = d.dot(g_right), y = d.dot(g_up);
    if (std::hypot(x, y) < g_radius * kDeadZone || g_entries.empty()) {
        return -1;
    }
    const float n = static_cast<float>(g_entries.size());
    const float step = 2.f * kPi / n;
    const float angle = std::atan2(y, x);
    int i = static_cast<int>(std::lround((kPi * 0.5f - angle) / step));
    i %= static_cast<int>(n);
    if (i < 0) {
        i += static_cast<int>(n);
    }
    return i;
}

// Builds the matrix that draws a unit square of `size` meters at `pos` (tracking space), facing
// the same way as the wheel.
void place(Mtx out, const Mtx view, const Mtx trackingToWorld, const Vec3& pos, float size) {
    Mtx local;
    local[0][0] = g_right.x * size; local[0][1] = g_up.x * size; local[0][2] = g_normal.x * size; local[0][3] = pos.x;
    local[1][0] = g_right.y * size; local[1][1] = g_up.y * size; local[1][2] = g_normal.y * size; local[1][3] = pos.y;
    local[2][0] = g_right.z * size; local[2][1] = g_up.z * size; local[2][2] = g_normal.z * size; local[2][3] = pos.z;
    Mtx world;
    MTXConcat(trackingToWorld, local, world);
    MTXConcat(view, world, out);
}

void draw_disc(const Mtx posMtx, GXColor color) {
    constexpr int kSegments = 40;
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXLoadPosMtxImm(posMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, kSegments + 2);
    GXPosition3f32(0.f, 0.f, 0.f);
    GXColor4u8(color.r, color.g, color.b, color.a);
    for (int i = 0; i <= kSegments; ++i) {
        const float a = 2.f * kPi * i / kSegments;
        GXPosition3f32(0.5f * std::cos(a), 0.5f * std::sin(a), 0.f);
        GXColor4u8(color.r, color.g, color.b, color.a / 3);
    }
    GXEnd();
}

}  // namespace

void update(bool allowed, xr::Hand hand, float squeeze, const Pose& handPose, const Pose& head) {
    if (!g_open) {
        g_openAnim = 0.f;
        if (!allowed || squeeze < kOpenSqueeze || g_chosen >= 0) {
            return;
        }
        if (!load_entries()) {
            return;
        }
        g_open = true;
        g_hover = -1;
        g_center = handPose.position;
        Vec3 toHead = head.position - g_center;
        g_normal = toHead.length() > 1e-3f ? toHead.normalized() : Vec3{0.f, 0.f, 1.f};
        g_right = Vec3{0.f, 1.f, 0.f}.cross(g_normal);
        g_right = g_right.length() > 1e-3f ? g_right.normalized() : Vec3{1.f, 0.f, 0.f};
        g_up = g_normal.cross(g_right).normalized();
        g_radius = std::max(kMinRadius,
            static_cast<float>(g_entries.size()) * kIconSize * 1.15f / (2.f * kPi));
        xr::haptic(hand, 0.3f, 0.04f);
        return;
    }
    g_openAnim = std::min(1.f, g_openAnim + 0.12f);
    if (!allowed) {
        g_open = false;
        close();
        return;
    }
    const int hover = hovered_entry(handPose.position);
    if (hover != g_hover && hover >= 0) {
        xr::haptic(hand, 0.25f, 0.02f);
    }
    g_hover = hover;
    if (squeeze < kReleaseSqueeze) {
        g_open = false;
        if (g_hover >= 0) {
            g_chosen = g_hover;
            g_chosenSlot = g_entries[g_hover].slot;
            xr::haptic(hand, 0.6f, 0.06f);
        }
    }
}

bool is_open() { return g_open; }

int take_pull_out() {
    if (g_chosen < 0) {
        return 0;
    }
    g_chosen = -1;
    const u8 slot = g_chosenSlot;
    free_entries();
    // Already on Y: just use it from there.
    if (dComIfGs_getSelectItemIndex(SELECT_ITEM_Y) == slot) {
        return -6;
    }
    dComIfGs_setMixItemIndex(SELECT_ITEM_X, 0xFF);
    dComIfGs_setSelectItemIndex(SELECT_ITEM_X, slot);
    return 6;
}

void draw(const Mtx view, const Mtx44 projection, const Mtx trackingToWorld) {
    if (!g_open || g_entries.empty()) {
        return;
    }
    GXSetProjection(projection, GX_PERSPECTIVE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0.f, GXColor{0, 0, 0, 0});
    GXSetNumIndStages(0);
    GXSetNumChans(1);

    // Grow in from the hand as it opens.
    const float grow = 0.3f + 0.7f * g_openAnim;
    Mtx m;
    place(m, view, trackingToWorld, g_center, (g_radius + kIconSize) * 2.f * grow);
    draw_disc(m, GXColor{10, 12, 24, 200});
    place(m, view, trackingToWorld, g_center, g_radius * kDeadZone * 2.f * grow);
    draw_disc(m, GXColor{90, 90, 110, 120});

    for (int i = 0; i < static_cast<int>(g_entries.size()); ++i) {
        const Entry& e = g_entries[i];
        const Vec3 pos = g_center + (slot_position(i) - g_center) * grow;
        const bool hovered = i == g_hover;
        if (hovered) {
            place(m, view, trackingToWorld, pos, kIconSize * 1.7f);
            draw_disc(m, GXColor{255, 220, 120, 230});
        }
        const float size = kIconSize * e.scale * (hovered ? 1.35f : 1.f) * grow;
        place(m, view, trackingToWorld, pos, size);
        for (int k = 0; k < e.layerCount; ++k) {
            if (e.layers[k] != nullptr) {
                e.layers[k]->draw_world(m);
            }
        }
    }
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetNumTexGens(0);
}

void close() {
    g_open = false;
    g_hover = -1;
    g_chosen = -1;
    free_entries();
}

}  // namespace dusk::vr::wheel
