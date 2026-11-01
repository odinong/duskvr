#include "dusk/vr/vr_gamepad.h"

#include "dusk/logging.h"

#include <dolphin/pad.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace dusk::vr::gamepad {
namespace {

constexpr borealis::Log Log{"vr"};

SDL_JoystickID g_id = 0;
SDL_Joystick* g_joystick = nullptr;
std::string g_name;
bool g_portClaimed = false;
std::string g_previousOwner;  // controller that had player 1 before VR took it

Sint16 axis(float v) {
    return static_cast<Sint16>(std::clamp(std::lround(v * 32767.f), -32768l, 32767l));
}

Sint16 trigger(float v) {
    return static_cast<Sint16>(std::clamp(std::lround(v * 32767.f), 0l, 32767l));
}

int find_index(const std::string& name) {
    for (u32 i = 0; i < PADCount(); ++i) {
        const char* n = PADGetNameForControllerIndex(i);
        if (n != nullptr && name == n) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The gamepad shows up in the controller list a frame or two after it is attached.
void claim_port() {
    if (g_portClaimed) {
        return;
    }
    const int index = find_index(g_name);
    if (index < 0) {
        return;
    }
    if (const char* owner = PADGetName(PAD_CHAN0); owner != nullptr && g_name != owner) {
        g_previousOwner = owner;
    }
    PADSetPortForIndex(static_cast<u32>(index), PAD_CHAN0);
    g_portClaimed = true;
    Log.info("'{}' is player 1", g_name);
}

}  // namespace

void attach(const char* name) {
    if (g_id != 0) {
        return;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    g_id = SDL_AttachVirtualJoystick(&desc);
    if (g_id == 0) {
        Log.error("Could not create the VR gamepad: {}", SDL_GetError());
        return;
    }
    g_joystick = SDL_OpenJoystick(g_id);
    g_name = name;
    g_portClaimed = false;
    Log.info("VR controllers attached as gamepad '{}'", name);
}

bool attached() { return g_id != 0; }

void update(const State& s) {
    if (g_joystick == nullptr) {
        return;
    }
    claim_port();
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFTX, axis(s.leftX));
    // SDL's Y axes point down.
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFTY, axis(-s.leftY));
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis(s.rightX));
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis(-s.rightY));
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, trigger(s.leftTrigger));
    SDL_SetJoystickVirtualAxis(g_joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, trigger(s.rightTrigger));
    const auto button = [](SDL_GamepadButton b, bool down) {
        SDL_SetJoystickVirtualButton(g_joystick, b, down);
    };
    button(SDL_GAMEPAD_BUTTON_SOUTH, s.south);
    button(SDL_GAMEPAD_BUTTON_EAST, s.east);
    button(SDL_GAMEPAD_BUTTON_WEST, s.west);
    button(SDL_GAMEPAD_BUTTON_NORTH, s.north);
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, s.leftShoulder);
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, s.rightShoulder);
    button(SDL_GAMEPAD_BUTTON_LEFT_STICK, s.leftStick);
    button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, s.rightStick);
    button(SDL_GAMEPAD_BUTTON_START, s.start);
    button(SDL_GAMEPAD_BUTTON_BACK, s.back);
}

void detach() {
    if (g_id == 0) {
        return;
    }
    if (g_joystick != nullptr) {
        SDL_CloseJoystick(g_joystick);
        g_joystick = nullptr;
    }
    SDL_DetachVirtualJoystick(g_id);
    g_id = 0;
    if (g_portClaimed && !g_previousOwner.empty()) {
        if (const int index = find_index(g_previousOwner); index >= 0) {
            PADSetPortForIndex(static_cast<u32>(index), PAD_CHAN0);
        }
    }
    g_portClaimed = false;
    g_previousOwner.clear();
}

}  // namespace dusk::vr::gamepad
