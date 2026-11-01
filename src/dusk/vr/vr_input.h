#pragma once

#include "dusk/vr/xr_session.h"

namespace dusk::vr::input {

struct FrameResult {
    float turnRad = 0.f;    // play-space yaw change requested this frame
    float motion = 0.f;     // 0..1, how much artificial movement is happening (comfort vignette)
    bool recenter = false;
    bool toggleView = false;
};

struct Context {
    bool gameplay = false;     // Link is controllable (no menu, event or pause)
    bool firstPerson = false;
    bool locked = false;       // Link is Z-targeting
    float standingHeight = 0;  // head height at recenter, meters
    bool bowReady = false;     // bow or slingshot out and aimed
    int bowSlot = 0;           // item slot the bow is on (0 = X, 1 = Y)
    bool wolf = false;
    bool climbing = false;     // ladder, vines, ledge or ceiling
    bool swimming = false;
    bool riding = false;       // on Epona
    bool carrying = false;     // holding a pot, bomb, rock... over his head
};

// Runs every headset frame: turning, view toggles and gesture detection. Gestures are latched until
// the next simulation tick consumes them.
FrameResult update_frame(const xr::InputState& in, const Pose& head, float dt, const Context& ctx);

// Runs before each simulation tick: writes the virtual GameCube pad for port 0.
void sync_virtual_pad(const Context& ctx);

// The hand holding the sword.
xr::Hand sword_hand();
xr::Hand shield_hand();

// While the bowstring is drawn: the shot direction (from the string hand to the bow hand), in
// tracking space.
bool bow_aim(Vec3& dir);

// While the laser points at a menu, the trigger clicks instead of pressing B.
void set_pointer_captured(bool captured);

}  // namespace dusk::vr::input
