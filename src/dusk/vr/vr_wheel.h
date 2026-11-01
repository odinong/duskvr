#pragma once

#include "dusk/vr/vr_math.h"
#include "dusk/vr/xr_session.h"

#include <dolphin/mtx.h>

// The item wheel: squeeze the sword hand's grip and a ring of everything you own floats around
// your hand. Move your hand onto an item and let go to pull it out; let go in the middle to put
// the wheel away. No buttons to remember.
namespace dusk::vr::wheel {

// Once per VR frame. Poses are in tracking space; `allowed` is false whenever the grip means
// something else (climbing, drawing the bow, menus...).
void update(bool allowed, xr::Hand hand, float squeeze, const Pose& handPose, const Pose& head);

bool is_open();

// At the start of a sim tick: equips the chosen item to X and returns how many frames to hold
// X so Link pulls it out (0 when nothing was chosen).
int take_pull_out();

// Draws the wheel into the current eye, on top of the scene. `trackingToWorld` maps tracking
// space (meters) into the game world for this eye.
void draw(const Mtx view, const Mtx44 projection, const Mtx trackingToWorld);

// Frees the icons (VR shutting down, or the game leaving play).
void close();

}  // namespace dusk::vr::wheel
