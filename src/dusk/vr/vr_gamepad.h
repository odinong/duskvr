#pragma once

#include <dolphin/types.h>

// The VR controllers as a real (virtual SDL) gamepad, so Dusklight's input menu lists them, they
// can be rebound like any controller, and its own menus can be navigated with them.
namespace dusk::vr::gamepad {

struct State {
    float leftX = 0.f, leftY = 0.f, rightX = 0.f, rightY = 0.f;  // -1..1, +y up
    float leftTrigger = 0.f, rightTrigger = 0.f;                  // 0..1
    bool south = false, east = false, west = false, north = false;
    bool leftShoulder = false, rightShoulder = false;
    bool leftStick = false, rightStick = false;
    bool start = false, back = false;
};

// Creates the gamepad (once) and puts it on player 1, remembering who had that port.
void attach(const char* name);
bool attached();
void update(const State& state);
// Removes the gamepad and gives player 1 back to the controller that had it.
void detach();

}  // namespace dusk::vr::gamepad
