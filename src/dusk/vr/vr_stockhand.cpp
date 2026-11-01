#include "dusk/vr/vr_stockhand.h"

#include "dusk/logging.h"
#include "JSystem/J3DGraphAnimator/J3DModelData.h"
#include "JSystem/J3DGraphBase/J3DMaterial.h"
#include "JSystem/J3DGraphBase/J3DShape.h"
#include "JSystem/J3DGraphBase/J3DShapeDraw.h"
#include "JSystem/J3DGraphBase/J3DTexture.h"
#include "JSystem/J3DGraphBase/J3DVertex.h"
#include "m_Do/m_Do_lib.h"

#include <aurora/dl.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace dusk::vr::stockhand {
namespace {

constexpr borealis::Log Log{"vr"};

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }

u32 swap32(u32 v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// Descriptor tables may still be in the file's big-endian layout.
u32 fix_enum(u32 v) { return v > 0xFFFF ? swap32(v) : v; }

float read_component(const u8* p, u32 type, u8 frac, bool bigEndian) {
    const float scale = 1.f / static_cast<float>(1u << frac);
    switch (type) {
    case GX_F32: {
        u32 bits;
        std::memcpy(&bits, p, 4);
        if (bigEndian) {
            bits = swap32(bits);
        }
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    case GX_S16: {
        u16 v;
        std::memcpy(&v, p, 2);
        if (bigEndian) {
            v = static_cast<u16>((v >> 8) | (v << 8));
        }
        return static_cast<s16>(v) * scale;
    }
    case GX_U16: {
        u16 v;
        std::memcpy(&v, p, 2);
        if (bigEndian) {
            v = static_cast<u16>((v >> 8) | (v << 8));
        }
        return v * scale;
    }
    case GX_S8:
        return static_cast<s8>(p[0]) * scale;
    default:
        return p[0] * scale;
    }
}

int component_size(u32 type) {
    switch (type) {
    case GX_F32: return 4;
    case GX_S16:
    case GX_U16: return 2;
    default: return 1;
    }
}

struct Format {
    u32 cnt = 0, type = GX_F32;
    u8 frac = 0;
    bool found = false;
};

Format find_format(J3DVertexData& vtx, u32 attr) {
    Format f;
    for (const GXVtxAttrFmtList* it = vtx.getVtxAttrFmtList(); it != nullptr; ++it) {
        const u32 a = fix_enum(static_cast<u32>(it->attr));
        if (a == GX_VA_NULL) {
            break;
        }
        if (a == attr) {
            f.cnt = fix_enum(static_cast<u32>(it->cnt));
            f.type = fix_enum(static_cast<u32>(it->type));
            f.frac = it->frac;
            f.found = true;
            break;
        }
    }
    return f;
}

}  // namespace

bool read_shape(J3DModelData* data, J3DShape* shape, Mesh& out) {
    J3DVertexData& vtx = data->getVertexData();
    const Format posFmt = find_format(vtx, GX_VA_POS);
    const Format uvFmt = find_format(vtx, GX_VA_TEX0);
    if (!posFmt.found) {
        return false;
    }

    // Which attributes the display list carries, and how (direct or 8/16-bit index).
    struct Attr {
        u32 attr, type;
    };
    std::vector<Attr> attrs;
    for (GXVtxDescList* d = shape->getVtxDesc(); d != nullptr; ++d) {
        const u32 a = fix_enum(static_cast<u32>(d->attr));
        if (a == GX_VA_NULL) {
            break;
        }
        const u32 t = fix_enum(static_cast<u32>(d->type));
        if (t != GX_NONE) {
            attrs.push_back({a, t});
        }
    }

    // Positions: decide byte order from the shape's bounding box.
    const auto* posBytes = static_cast<const u8*>(vtx.getVtxPosArray());
    const u32 posCount = vtx.getVtxArrNum(GX_VA_POS);
    const int posStride = component_size(posFmt.type) * 3;
    const Vec min = *shape->getMin(), max = *shape->getMax();
    const auto plausible = [&](bool be) {
        int inside = 0, tested = 0;
        for (u32 i = 0; i < posCount; i += std::max(1u, posCount / 64), ++tested) {
            const u8* p = posBytes + i * posStride;
            const float x = read_component(p, posFmt.type, posFmt.frac, be);
            inside += std::isfinite(x) && std::fabs(x) < 10000.f;
        }
        return tested > 0 && inside == tested;
    };
    const bool posBE = plausible(true) || !plausible(false);
    out.positions.resize(posCount);
    for (u32 i = 0; i < posCount; ++i) {
        const u8* p = posBytes + i * posStride;
        const int c = component_size(posFmt.type);
        out.positions[i] = {read_component(p, posFmt.type, posFmt.frac, posBE),
            read_component(p + c, posFmt.type, posFmt.frac, posBE),
            read_component(p + 2 * c, posFmt.type, posFmt.frac, posBE)};
    }
    (void)min;
    (void)max;
    if (uvFmt.found && vtx.getVtxTexCoordArray(0) != nullptr) {
        const auto* uvBytes = static_cast<const u8*>(vtx.getVtxTexCoordArray(0));
        const u32 uvCount = vtx.getVtxArrNum(GX_VA_TEX0);
        const int c = component_size(uvFmt.type);
        out.uvs.resize(uvCount);
        for (u32 i = 0; i < uvCount; ++i) {
            const u8* p = uvBytes + i * c * 2;
            out.uvs[i] = {read_component(p, uvFmt.type, uvFmt.frac, posBE),
                read_component(p + c, uvFmt.type, uvFmt.frac, posBE)};
        }
    }

    // Walk the display lists (aurora's reader: plain GX draws, or its prebuilt indexed draws).
    namespace dl = aurora::gx::dl;
    for (u16 g = 0; g < shape->getMtxGroupNum(); ++g) {
        J3DShapeDraw* draw = shape->getShapeDraw(g);
        dl::Reader reader{draw->getDisplayList(), draw->getDisplayListSize(), shape->getVtxDesc()};
        while (const auto cmd = reader.next()) {
            const dl::DrawCmd& d = cmd->draw;
            const auto corner = [&](u32 v) {
                const bool hasUv = !out.uvs.empty();
                return Mesh::Corner{static_cast<int>(d.attr_idx(v, GX_VA_POS)),
                    hasUv ? static_cast<int>(d.attr_idx(v, GX_VA_TEX0)) : -1};
            };
            if (cmd->kind == dl::Command::Kind::Draw) {
                dl::expand_triangles(d.prim, d.vtxCount, [&](u16 a, u16 b, u16 c) {
                    out.corners.insert(out.corners.end(), {corner(a), corner(b), corner(c)});
                });
            } else if (cmd->kind == dl::Command::Kind::DrawIndexed) {
                for (u32 i = 0; i + 2 < d.indexCount; i += 3) {
                    out.corners.insert(out.corners.end(),
                        {corner(d.index(i)), corner(d.index(i + 1)), corner(d.index(i + 2))});
                }
            }
        }
    }
    return !out.corners.empty();
}

namespace {

// Link's left open hand ('al_handLF'), measured from the mesh, in the hand joint's frame (game
// units): fingers run along +X, the thumb is on the +Y side and the palm faces +Z. The right hand
// ('al_handRF') is the same with Z mirrored.
constexpr Vec3 kRestWrist{0.f, 0.f, -0.5f};
// Each chain: base joint, two middle joints, tip. Thumb first, then index to little.
constexpr Vec3 kRestFingers[5][4] = {
    {{2.5f, 3.0f, 0.5f}, {5.0f, 5.6f, 2.2f}, {6.7f, 6.7f, 3.3f}, {7.9f, 8.0f, 4.6f}},
    {{9.2f, 4.3f, -4.6f}, {12.6f, 4.7f, -5.4f}, {15.0f, 4.6f, -5.3f}, {17.6f, 4.6f, -4.3f}},
    {{9.8f, 1.2f, -5.0f}, {13.6f, 1.1f, -5.7f}, {16.0f, 1.0f, -5.4f}, {18.2f, 0.8f, -4.0f}},
    {{9.6f, -1.8f, -5.0f}, {13.2f, -1.9f, -5.4f}, {15.3f, -2.0f, -5.3f}, {17.4f, -2.1f, -4.1f}},
    {{9.0f, -4.4f, -4.8f}, {11.9f, -5.1f, -5.1f}, {13.6f, -5.4f, -5.1f}, {15.5f, -5.8f, -4.4f}},
};
// The tracked joints (XR_EXT_hand_tracking) whose orientation drives each bone of each chain.
constexpr int kTrackedBones[5][3] = {
    {2, 3, 4}, {7, 8, 9}, {12, 13, 14}, {17, 18, 19}, {22, 23, 24}};
constexpr int kTrackedWrist = 1;
constexpr int kBoneCount = 1 + 5 * 3;  // the palm, then three per finger

struct Rig {
    Mesh mesh;
    std::vector<std::array<u8, 2>> bones;  // per position: up to two bones
    std::vector<std::array<float, 2>> weights;
    std::array<Vec3, 5 * 4> joints;  // rest skeleton for this hand
    Vec3 wrist;
    float mirror = 1.f;  // -1 for the right hand
    ResTIMG* texture = nullptr;
    bool valid = false;
};

J3DModelData* g_preparedFor = nullptr;
Rig g_rigs[2];

// A rotation as three columns: side, dorsal (back of the hand / nail side), forward (toward the
// fingertip). Tracked joints and rest bones are both described this way.
struct Frame {
    Vec3 s, d, f;
    Vec3 apply(const Vec3& v) const { return s * v.x + d * v.y + f * v.z; }
    Vec3 inverse(const Vec3& v) const { return {s.dot(v), d.dot(v), f.dot(v)}; }
};

Frame make_frame(const Vec3& forward, const Vec3& dorsal) {
    Frame r;
    r.f = forward.normalized();
    r.d = (dorsal - r.f * r.f.dot(dorsal)).normalized();
    r.s = r.d.cross(r.f);
    return r;
}

Frame tracked_frame(const Pose& joint) {
    return make_frame(joint.orientation.rotate({0.f, 0.f, -1.f}),
        joint.orientation.rotate({0.f, 1.f, 0.f}));
}

float segment_distance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float t = std::clamp((p - a).dot(ab) / std::max(ab.dot(ab), 1e-6f), 0.f, 1.f);
    return (p - (a + ab * t)).length();
}

Vec3 rest_dorsal(const Rig& rig, int finger) {
    // The back of the hand faces -Z on the left hand; the thumb's nail turns out toward +Y.
    const Vec3 back{0.f, 0.f, -rig.mirror};
    return finger == 0 ? (back + Vec3{0.f, 1.f, 0.f}).normalized() : back;
}

bool build_rig(J3DModelData* data, const char* material, float mirror, Rig& rig) {
    rig = Rig{};
    rig.mirror = mirror;
    J3DShape* shape = nullptr;
    for (u16 i = 0; i < data->getShapeNum(); ++i) {
        J3DShape* candidate = data->getShapeNodePointer(i);
        J3DMaterial* m = candidate->getMaterial();
        if (m != nullptr && data->getMaterialName() != nullptr &&
            std::strcmp(data->getMaterialName()->getName(m->getIndex()), material) == 0)
        {
            shape = candidate;
            break;
        }
    }
    if (shape == nullptr || !read_shape(data, shape, rig.mesh) || rig.mesh.uvs.empty()) {
        return false;
    }
    const auto mirrored = [&](Vec3 v) { return Vec3{v.x, v.y, v.z * mirror}; };
    rig.wrist = mirrored(kRestWrist);
    for (int f = 0; f < 5; ++f) {
        for (int k = 0; k < 4; ++k) {
            rig.joints[f * 4 + k] = mirrored(kRestFingers[f][k]);
        }
    }

    // Weights: each vertex follows its nearest bone, blended with the next nearest near a joint.
    rig.bones.resize(rig.mesh.positions.size());
    rig.weights.resize(rig.mesh.positions.size());
    for (size_t i = 0; i < rig.mesh.positions.size(); ++i) {
        const Vec3& p = rig.mesh.positions[i];
        float dist[kBoneCount];
        float palm = 1e9f;
        for (int f = 0; f < 5; ++f) {
            palm = std::min(palm, segment_distance(p, rig.wrist, rig.joints[f * 4]));
        }
        palm = std::min(palm, segment_distance(p, rig.joints[1 * 4], rig.joints[4 * 4]));
        dist[0] = palm;
        for (int f = 0; f < 5; ++f) {
            for (int k = 0; k < 3; ++k) {
                dist[1 + f * 3 + k] =
                    segment_distance(p, rig.joints[f * 4 + k], rig.joints[f * 4 + k + 1]);
            }
        }
        int best = 0, second = 1;
        if (dist[second] < dist[best]) {
            std::swap(best, second);
        }
        for (int b = 2; b < kBoneCount; ++b) {
            if (dist[b] < dist[best]) {
                second = best;
                best = b;
            } else if (dist[b] < dist[second]) {
                second = b;
            }
        }
        constexpr float kBlend = 1.2f;
        const float gap = dist[second] - dist[best];
        const float w = gap >= kBlend ? 1.f : 0.5f + 0.5f * gap / kBlend;
        rig.bones[i] = {static_cast<u8>(best), static_cast<u8>(second)};
        rig.weights[i] = {w, 1.f - w};
    }

    if (J3DMaterial* m = shape->getMaterial(); m != nullptr && data->getTexture() != nullptr) {
        const u16 tex = m->getTexNo(0);
        if (tex != 0xFFFF) {
            rig.texture = data->getTexture()->getResTIMG(tex);
        }
    }
    rig.valid = true;
    return true;
}

}  // namespace

bool prepare(J3DModelData* data) {
    if (data == nullptr) {
        return false;
    }
    if (data != g_preparedFor) {
        g_preparedFor = data;
        build_rig(data, "al_handLF_m", 1.f, g_rigs[0]);
        build_rig(data, "al_handRF_m", -1.f, g_rigs[1]);
        Log.info("stock hands: left {}, right {}", g_rigs[0].valid, g_rigs[1].valid);
    }
    return g_rigs[0].valid && g_rigs[1].valid;
}

void pose(int hand, const Pose (&joints)[26], const Vec3& wrist, float metersPerUnit,
    std::vector<SkinnedVertex>& out) {
    const Rig& rig = g_rigs[hand];
    if (!rig.valid) {
        return;
    }
    // Each bone: where its base lands, and how it turns the rest pose (tracked frame times the
    // inverse of the rest frame).
    struct Bone {
        Vec3 base, restBase;
        Frame rest, now;
        Vec3 map(const Vec3& p, float scale) const {
            return base + now.apply(rest.inverse(p - restBase)) * scale;
        }
    };
    std::array<Bone, kBoneCount> bones;
    const Vec3 back{0.f, 0.f, -rig.mirror};
    bones[0].restBase = rig.wrist;
    bones[0].base = wrist;
    bones[0].rest = make_frame(rig.joints[2 * 4] - rig.wrist, back);
    bones[0].now = tracked_frame(joints[kTrackedWrist]);
    for (int f = 0; f < 5; ++f) {
        Vec3 base = bones[0].map(rig.joints[f * 4], metersPerUnit);
        for (int k = 0; k < 3; ++k) {
            Bone& b = bones[1 + f * 3 + k];
            const Vec3& a = rig.joints[f * 4 + k];
            const Vec3& c = rig.joints[f * 4 + k + 1];
            b.restBase = a;
            b.base = base;
            b.rest = make_frame(c - a, rest_dorsal(rig, f));
            b.now = tracked_frame(joints[kTrackedBones[f][k]]);
            base = b.map(c, metersPerUnit);
        }
    }

    std::vector<Vec3> posed(rig.mesh.positions.size());
    for (size_t i = 0; i < posed.size(); ++i) {
        const Vec3& p = rig.mesh.positions[i];
        const auto& bi = rig.bones[i];
        const auto& wi = rig.weights[i];
        posed[i] = bones[bi[0]].map(p, metersPerUnit) * wi[0];
        if (wi[1] > 0.f) {
            posed[i] = posed[i] + bones[bi[1]].map(p, metersPerUnit) * wi[1];
        }
    }
    // Smooth normals from the posed triangles.
    std::vector<Vec3> normals(posed.size(), Vec3{});
    const auto& corners = rig.mesh.corners;
    for (size_t t = 0; t + 2 < corners.size(); t += 3) {
        const Vec3& a = posed[corners[t].pos];
        const Vec3 n = (posed[corners[t + 1].pos] - a).cross(posed[corners[t + 2].pos] - a);
        for (int k = 0; k < 3; ++k) {
            normals[corners[t + k].pos] = normals[corners[t + k].pos] + n;
        }
    }
    for (const auto& corner : corners) {
        const Vec3 n = normals[corner.pos].length() > 1e-9f ? normals[corner.pos].normalized()
                                                            : Vec3{0.f, 1.f, 0.f};
        const auto& uv = corner.uv >= 0 && corner.uv < static_cast<int>(rig.mesh.uvs.size())
                             ? rig.mesh.uvs[corner.uv]
                             : std::array<float, 2>{0.f, 0.f};
        out.push_back({posed[corner.pos], n, uv[0], uv[1]});
    }
}

void synth_joints(int hand, const Quat& rotation, float curl, Pose (&out)[26]) {
    const Rig& rig = g_rigs[hand];
    const auto toPose = [&](const Frame& f) {
        // Tracked joints: -Z toward the fingertip, +Y dorsal, so +X is -side.
        Pose p;
        p.orientation = rotation * quat_from_axes(f.s * -1.f, f.d, f.f * -1.f);
        return p;
    };
    const Vec3 back{0.f, 0.f, -rig.mirror};
    out[kTrackedWrist] = toPose(make_frame(rig.joints[2 * 4] - rig.wrist, back));
    for (int f = 0; f < 5; ++f) {
        float angle = 0.f;
        for (int k = 0; k < 3; ++k) {
            Frame fr = make_frame(rig.joints[f * 4 + k + 1] - rig.joints[f * 4 + k], rest_dorsal(rig, f));
            angle += curl;
            // Curl toward the palm: turn about the side axis.
            const Vec3 fwd = fr.f * std::cos(angle) + fr.s.cross(fr.f) * std::sin(angle);
            const Vec3 dor = fr.d * std::cos(angle) + fr.s.cross(fr.d) * std::sin(angle);
            out[kTrackedBones[f][k]] = toPose(make_frame(fwd, dor));
        }
    }
}

void load_texture(int hand) {
    static TGXTexObj texObj[2];
    static TGXTlutObj tlutObj[2];
    if (g_rigs[hand].texture == nullptr) {
        return;
    }
    mDoLib_setResTimgObj(g_rigs[hand].texture, &texObj[hand], GX_TLUT0, &tlutObj[hand]);
    GXLoadTexObj(&texObj[hand], GX_TEXMAP0);
}

void dump_obj(J3DModelData* data, const char* dir) {
    for (u16 s = 0; s < data->getShapeNum(); ++s) {
        J3DShape* shape = data->getShapeNodePointer(s);
        Mesh mesh;
        if (!read_shape(data, shape, mesh)) {
            Log.info("hand shape {}: unreadable", s);
            continue;
        }
        const char* name = "?";
        if (J3DMaterial* m = shape->getMaterial(); m != nullptr && data->getMaterialName() != nullptr) {
            name = data->getMaterialName()->getName(m->getIndex());
        }
        const std::string path = std::string(dir) + "/hand_" + std::to_string(s) + "_" + name + ".obj";
        FILE* f = std::fopen(path.c_str(), "w");
        if (f == nullptr) {
            continue;
        }
        for (const Vec3& p : mesh.positions) {
            std::fprintf(f, "v %f %f %f\n", p.x, p.y, p.z);
        }
        for (const auto& uv : mesh.uvs) {
            std::fprintf(f, "vt %f %f\n", uv[0], uv[1]);
        }
        for (size_t i = 0; i + 2 < mesh.corners.size(); i += 3) {
            std::fprintf(f, "f");
            for (int k = 0; k < 3; ++k) {
                const auto& c = mesh.corners[i + k];
                if (c.uv >= 0) {
                    std::fprintf(f, " %d/%d", c.pos + 1, c.uv + 1);
                } else {
                    std::fprintf(f, " %d", c.pos + 1);
                }
            }
            std::fprintf(f, "\n");
        }
        std::fclose(f);
        Log.info("hand shape {} '{}': {} triangles, joint {}", s, name, mesh.corners.size() / 3,
            shape->getMaterial() ? shape->getMaterial()->getJoint()->getJntNo() : -1);
    }
}

}  // namespace dusk::vr::stockhand
