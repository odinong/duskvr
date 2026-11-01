#pragma once

#include "dusk/vr/vr_math.h"

#include <dolphin/mtx.h>

class J3DModelData;

// Hands with fingers. Link's own hands are a handful of fixed shapes; with finger tracking each
// hand is instead built from the tracked skeleton (every bone of every finger) and drawn into
// the eye, attached to the end of Link's arm.
namespace dusk::vr::hands {

struct Frame {
    bool show[2]{};
    // Where Link's wrist is, tracking space: the tracked hand is moved so its wrist lands here.
    Vec3 wrist[2];
    // Link's hand model: its open hands are skinned to the tracked fingers. Without it, simple
    // stand-in hands are drawn.
    J3DModelData* handModel = nullptr;
    float metersPerUnit = 0.01f;
    // Debug aid: joints to use instead of the tracked ones.
    const Pose (*testJoints)[26] = nullptr;
};

// Draws the hands into the current view, depth tested against the scene. `trackingToWorld`
// maps tracking space (meters) into the game world.
void draw(const Frame& frame, const Mtx view, const Mtx44 projection, const Mtx trackingToWorld);

}  // namespace dusk::vr::hands
