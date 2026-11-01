#pragma once

#include "dusk/vr/xr_session.h"

namespace dusk::vr::input {

struct FrameResult {
    float turnRad = 0.f;    // play-space yaw change requested this frame
    bool recenter = false;
    bool toggleView = false;
};

struct Context {
    bool gameplay = false;     // Link is controllable (no menu, event or pause)
    bool firstPerson = false;
    bool locked = false;       // Link is Z-targeting
    float standingHeight = 0;  // head height at recenter, meters
};

// Runs every headset frame: turning, view toggles and gesture detection. Gestures are latched until
// the next simulation tick consumes them.
FrameResult update_frame(const xr::InputState& in, const Pose& head, float dt, const Context& ctx);

// Runs before each simulation tick: writes the virtual GameCube pad for port 0.
void sync_virtual_pad(const Context& ctx);

// The hand holding the sword.
xr::Hand sword_hand();
xr::Hand shield_hand();

}  // namespace dusk::vr::input
