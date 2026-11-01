#pragma once

#include "dusk/vr/vr_math.h"

#include <array>
#include <cstdint>
#include <vector>

class J3DModelData;
class J3DShape;

// Reads Link's hand shapes back out of the model: triangles with positions and texture
// coordinates, so a stock hand can be deformed by tracked finger bones.
namespace dusk::vr::stockhand {

struct Mesh {
    std::vector<Vec3> positions;               // model data's position array
    std::vector<std::array<float, 2>> uvs;     // texture coordinate array 0
    // Triangle corners: position index, uv index (or -1).
    struct Corner {
        int pos;
        int uv;
    };
    std::vector<Corner> corners;  // three per triangle
};

bool read_shape(J3DModelData* data, J3DShape* shape, Mesh& out);

// Rigs Link's open-hand shapes to a finger skeleton. Call with Link's hand model data; cheap
// when nothing changed. Returns whether skinned hands are available.
bool prepare(J3DModelData* data);

struct SkinnedVertex {
    Vec3 pos;  // tracking space, meters
    Vec3 normal;
    float u, v;
};

// Poses one hand (0 left, 1 right) from the tracked joints in `in`: palm on the wrist anchor,
// every finger bone turned like the tracked one, Link's own proportions kept. Appends a
// triangle list.
void pose(int hand, const Pose (&joints)[26], const Vec3& wrist, float metersPerUnit,
    std::vector<SkinnedVertex>& out);

// Debug aid: a tracked-style skeleton for the rest hand turned by `rotation`, every finger bone
// curled by `curl` radians.
void synth_joints(int hand, const Quat& rotation, float curl, Pose (&out)[26]);

// Loads the hand's texture into GX_TEXMAP0.
void load_texture(int hand);

// Debug aid: writes every hand shape of `data` as an OBJ file into `dir`.
void dump_obj(J3DModelData* data, const char* dir);

}  // namespace dusk::vr::stockhand
