#include "dusk/vr/vr_hands.h"

#include "dusk/vr/vr_stockhand.h"
#include "dusk/vr/xr_session.h"

#include <dolphin/gx.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dusk::vr::hands {
namespace {

constexpr float kPi = 3.14159265f;
constexpr int kSides = 8;

// Link's colors: bare fingers, brown leather glove over the palm and wrist.
constexpr GXColor kSkin{236, 184, 150, 255};
constexpr GXColor kGlove{104, 64, 38, 255};

struct Vertex {
    Vec3 pos;
    GXColor color;
};

std::vector<Vertex> g_tris;

GXColor shade(GXColor base, const Vec3& normal) {
    static const Vec3 kLight = Vec3{0.3f, 0.85f, 0.45f}.normalized();
    const float lit = 0.5f + 0.5f * std::max(0.f, normal.dot(kLight));
    return GXColor{static_cast<u8>(base.r * lit), static_cast<u8>(base.g * lit),
        static_cast<u8>(base.b * lit), 255};
}

// Two unit vectors perpendicular to `axis` and to each other.
void basis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 ref = std::fabs(axis.y) < 0.9f ? Vec3{0.f, 1.f, 0.f} : Vec3{1.f, 0.f, 0.f};
    u = axis.cross(ref).normalized();
    v = axis.cross(u);
}

// A tapered tube from a (radius ra) to b (radius rb).
void tube(const Vec3& a, float ra, const Vec3& b, float rb, GXColor color) {
    const Vec3 d = b - a;
    if (d.length() < 1e-4f) {
        return;
    }
    const Vec3 axis = d.normalized();
    Vec3 u, v;
    basis(axis, u, v);
    for (int i = 0; i < kSides; ++i) {
        const float a0 = 2.f * kPi * i / kSides, a1 = 2.f * kPi * (i + 1) / kSides;
        const Vec3 n0 = u * std::cos(a0) + v * std::sin(a0);
        const Vec3 n1 = u * std::cos(a1) + v * std::sin(a1);
        const Vertex p00{a + n0 * ra, shade(color, n0)}, p01{a + n1 * ra, shade(color, n1)};
        const Vertex p10{b + n0 * rb, shade(color, n0)}, p11{b + n1 * rb, shade(color, n1)};
        g_tris.insert(g_tris.end(), {p00, p10, p11, p00, p11, p01});
    }
}

// A low-poly ball, for knuckles and fingertips.
void ball(const Vec3& c, float r, GXColor color) {
    constexpr int kRings = 4;
    const auto point = [&](int ring, int side) {
        const float lat = kPi * ring / kRings - kPi * 0.5f;
        const float lon = 2.f * kPi * side / kSides;
        const Vec3 n{std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
        return Vertex{c + n * r, shade(color, n)};
    };
    for (int ring = 0; ring < kRings; ++ring) {
        for (int side = 0; side < kSides; ++side) {
            const Vertex a = point(ring, side), b = point(ring, side + 1);
            const Vertex cc = point(ring + 1, side), d = point(ring + 1, side + 1);
            g_tris.insert(g_tris.end(), {a, cc, d, a, d, b});
        }
    }
}

void build_hand(const xr::InputState& in, int h, const Vec3& wristAnchor) {
    const Pose* j = in.joints[h];
    const Vec3 shift = wristAnchor - j[1].position;
    const auto P = [&](int i) { return j[i].position + shift; };
    const auto R = [&](int i) { return std::clamp(in.jointRadius[h][i], 0.005f, 0.014f); };

    // Glove: the cuff, the back of the hand (each metacarpal, fattened so they merge into a palm)
    // and the thumb's base.
    const Vec3 forearm = (P(1) - P(11)).normalized();
    tube(P(1) + forearm * 0.04f, 0.022f, P(1), 0.019f, kGlove);
    for (int base : {6, 11, 16, 21}) {
        tube(P(1), 0.012f, P(base), 0.01f, kGlove);
        tube(P(base), 0.01f, P(base + 1), R(base + 1) * 1.05f, kGlove);
    }
    tube(P(7), R(7), P(22), R(22), kGlove);
    tube(P(6), 0.01f, P(21), 0.01f, kGlove);
    tube(P(1), 0.012f, P(2), 0.01f, kGlove);
    tube(P(2), 0.01f, P(3), R(3), kGlove);

    // Fingers, bone by bone.
    const auto finger = [&](int first, int last) {
        for (int i = first; i < last; ++i) {
            tube(P(i), R(i), P(i + 1), R(i + 1), kSkin);
            ball(P(i), R(i), kSkin);
        }
        ball(P(last), R(last), kSkin);
    };
    finger(3, 5);
    finger(7, 10);
    finger(12, 15);
    finger(17, 20);
    finger(22, 25);
}


void set_common_state(const Mtx view, const Mtx44 projection, const Mtx trackingToWorld) {
    Mtx posMtx;
    MTXConcat(view, trackingToWorld, posMtx);
    GXSetProjection(projection, GX_PERSPECTIVE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetFog(GX_FOG_NONE, 0.f, 0.f, 0.f, 0.f, GXColor{0, 0, 0, 0});
    GXSetNumIndStages(0);
    GXSetNumChans(1);
    GXSetNumTevStages(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXLoadPosMtxImm(posMtx, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

// Link's own open hands, skinned to the tracked fingers and drawn with their texture.
bool draw_stock(const Frame& frame, const Mtx view, const Mtx44 projection,
    const Mtx trackingToWorld) {
    if (frame.handModel == nullptr || !stockhand::prepare(frame.handModel)) {
        return false;
    }
    const auto& in = xr::input();
    static std::vector<stockhand::SkinnedVertex> verts;
    bool stateSet = false;
    for (int h = 0; h < 2; ++h) {
        if (!frame.show[h] || (!in.fingersTracked[h] && frame.testJoints == nullptr)) {
            continue;
        }
        verts.clear();
        stockhand::pose(h, frame.testJoints != nullptr ? frame.testJoints[h] : in.joints[h],
            frame.wrist[h], frame.metersPerUnit, verts);
        if (verts.empty()) {
            continue;
        }
        if (!stateSet) {
            set_common_state(view, projection, trackingToWorld);
            GXSetNumTexGens(1);
            GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
            GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
            GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
            stateSet = true;
        }
        stockhand::load_texture(h);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(verts.size()));
        for (const auto& v : verts) {
            const GXColor lit = shade(GXColor{255, 255, 255, 255}, v.normal);
            GXPosition3f32(v.pos.x, v.pos.y, v.pos.z);
            GXColor4u8(lit.r, lit.g, lit.b, 255);
            GXTexCoord2f32(v.u, v.v);
        }
        GXEnd();
    }
    if (stateSet) {
        GXSetVtxDesc(GX_VA_TEX0, GX_NONE);
        GXSetNumTexGens(0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    }
    return true;
}

}  // namespace

void draw(const Frame& frame, const Mtx view, const Mtx44 projection, const Mtx trackingToWorld) {
    if (draw_stock(frame, view, projection, trackingToWorld)) {
        return;
    }
    const auto& in = xr::input();
    g_tris.clear();
    for (int h = 0; h < 2; ++h) {
        if (frame.show[h] && in.fingersTracked[h]) {
            build_hand(in, h, frame.wrist[h]);
        }
    }
    if (g_tris.empty()) {
        return;
    }

    set_common_state(view, projection, trackingToWorld);
    GXSetNumTexGens(0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    constexpr size_t kBatch = 3 * 20000;
    for (size_t start = 0; start < g_tris.size(); start += kBatch) {
        const size_t count = std::min(kBatch, g_tris.size() - start);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(count));
        for (size_t i = start; i < start + count; ++i) {
            const Vertex& vtx = g_tris[i];
            GXPosition3f32(vtx.pos.x, vtx.pos.y, vtx.pos.z);
            GXColor4u8(vtx.color.r, vtx.color.g, vtx.color.b, vtx.color.a);
        }
        GXEnd();
    }
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

}  // namespace dusk::vr::hands
