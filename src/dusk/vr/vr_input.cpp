#include "dusk/vr/vr_input.h"

#include "dusk/action_bindings.h"
#include "dusk/settings.h"

#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace dusk::vr::input {
namespace {

constexpr float kStickDeadzone = 0.2f;
constexpr float kSnapThreshold = 0.75f;
constexpr float kSnapRelease = 0.35f;
constexpr float kSwordLength = 0.85f;  // meters from grip to blade tip
constexpr float kSwingCooldown = 0.32f;
constexpr float kSpinDegrees = 300.f;
constexpr float kSpinWindow = 1.2f;
constexpr float kBashSpeed = 1.8f;
constexpr float kCrouchRatio = 0.72f;
constexpr float kRecenterHold = 1.0f;

struct PadFrame {
    u16 buttons = 0;
    float stickX = 0.f, stickY = 0.f;
    bool overrideStick = false;
};

// Gesture output latched between headset frames and sim ticks.
struct Latched {
    bool swing = false;
    bool thrust = false;
    bool spin = false;
    bool bash = false;
};

Latched g_latched;
std::deque<PadFrame> g_macro;  // queued multi-tick inputs (spin attack)

bool g_snapArmed = true;
float g_swingCooldown = 0.f;
Vec3 g_prevTip;
bool g_prevTipValid = false;
float g_spinAccum = 0.f;
float g_spinTime = 0.f;
float g_prevSpinAngle = 0.f;
bool g_shieldUp = false;
bool g_crouching = false;
float g_recenterHeld = 0.f;
bool g_prevStickClick = false;

xr::InputState g_lastInput;

s8 to_stick(float v) {
    return static_cast<s8>(std::clamp(std::lround(v * 127.f), -127l, 127l));
}

float deadzone(float v) {
    const float a = std::fabs(v);
    if (a < kStickDeadzone) {
        return 0.f;
    }
    return std::copysign((a - kStickDeadzone) / (1.f - kStickDeadzone), v);
}

// Converts a tracking-space vector into the head's heading frame (x right, z back).
Vec3 to_head_frame(const Vec3& v, const Pose& head) {
    return heading_only(head.orientation).conjugate().rotate(v);
}

void detect_sword(const xr::HandState& hand, const Pose& head, float dt, const Context& ctx) {
    g_swingCooldown = std::max(0.f, g_swingCooldown - dt);
    if (!hand.active || dt <= 0.f) {
        g_prevTipValid = false;
        return;
    }

    const Vec3 tip = hand.grip.position + hand.grip.forward() * kSwordLength;
    Vec3 tipVelocity;
    if (g_prevTipValid) {
        tipVelocity = (tip - g_prevTip) * (1.f / dt);
    }
    g_prevTip = tip;
    const bool hadPrev = g_prevTipValid;
    g_prevTipValid = true;
    if (!hadPrev) {
        return;
    }

    const float speed = tipVelocity.length();
    const float threshold = std::max(0.8f, getSettings().vr.swingSpeed.getValue());

    // Spin: the tip sweeps around the head horizontally.
    const Vec3 rel = tip - head.position;
    const float angle = std::atan2(rel.x, rel.z);
    if (speed > threshold * 0.6f) {
        g_spinAccum += wrap_pi(angle - g_prevSpinAngle);
        g_spinTime += dt;
        if (std::fabs(g_spinAccum) > deg2rad(kSpinDegrees) && g_spinTime < kSpinWindow) {
            g_latched.spin = true;
            g_spinAccum = 0.f;
            g_spinTime = 0.f;
            g_swingCooldown = kSwingCooldown * 2.f;
        }
    } else {
        g_spinAccum = 0.f;
        g_spinTime = 0.f;
    }
    g_prevSpinAngle = angle;

    if (g_swingCooldown > 0.f || speed < threshold) {
        return;
    }
    const Vec3 dir = tipVelocity * (1.f / speed);
    const bool thrust = std::fabs(dir.dot(hand.grip.forward())) > 0.8f;
    if (thrust) {
        g_latched.thrust = true;
    } else {
        g_latched.swing = true;
    }
    g_swingCooldown = kSwingCooldown;
    xr::haptic(sword_hand(), 0.6f, 0.05f);
    (void)ctx;
}

void detect_shield(const xr::HandState& hand, const Pose& head, const Context& ctx) {
    if (!hand.active) {
        g_shieldUp = false;
        return;
    }
    // Shield held up in front of the chest.
    const Vec3 local = to_head_frame(hand.grip.position - head.position, head);
    const bool inFront = -local.z > 0.18f;
    const bool chestHeight = local.y > -0.55f && local.y < 0.15f;
    const bool wasUp = g_shieldUp;
    g_shieldUp = inFront && chestHeight;
    if (g_shieldUp && !wasUp) {
        xr::haptic(shield_hand(), 0.3f, 0.03f);
    }

    // Shield bash: a punch forward while guarding.
    if (g_shieldUp && ctx.locked && hand.velocityValid) {
        const Vec3 v = to_head_frame(hand.velocity, head);
        if (-v.z > kBashSpeed) {
            g_latched.bash = true;
        }
    }
}

void queue_spin() {
    // Quick spin: a full stick rotation followed by B.
    g_macro.clear();
    constexpr int kSteps = 8;
    for (int i = 0; i < kSteps; ++i) {
        const float a = 2.f * kPi * static_cast<float>(i) / kSteps;
        g_macro.push_back({0, std::sin(a), std::cos(a), true});
    }
    g_macro.push_back({PAD_BUTTON_B, 0.f, 0.f, true});
}

}  // namespace

xr::Hand sword_hand() { return getSettings().vr.leftHanded ? xr::kLeft : xr::kRight; }
xr::Hand shield_hand() { return getSettings().vr.leftHanded ? xr::kRight : xr::kLeft; }

FrameResult update_frame(const xr::InputState& in, const Pose& head, float dt, const Context& ctx) {
    FrameResult result;
    g_lastInput = in;
    const int s = sword_hand();

    // Turning on the sword-hand stick (first person only; third person uses it for the camera).
    const float turnX = deadzone(in.stickX[s]);
    if (ctx.firstPerson && ctx.gameplay) {
        switch (getSettings().vr.turnMode.getValue()) {
        case 0:  // snap
            if (g_snapArmed && std::fabs(turnX) > kSnapThreshold) {
                result.turnRad = -std::copysign(
                    deg2rad(static_cast<float>(getSettings().vr.snapTurnDegrees.getValue())), turnX);
                g_snapArmed = false;
            } else if (std::fabs(turnX) < kSnapRelease) {
                g_snapArmed = true;
            }
            break;
        case 1:  // smooth
            result.turnRad = -turnX * deg2rad(getSettings().vr.smoothTurnSpeed.getValue()) * dt;
            break;
        default:
            break;
        }
    }

    // Sword-hand stick click: tap toggles first/third person, hold recenters.
    if (in.stickClick[s]) {
        g_recenterHeld += dt;
        if (g_recenterHeld >= kRecenterHold && g_recenterHeld - dt < kRecenterHold) {
            result.recenter = true;
            xr::haptic(sword_hand(), 0.5f, 0.1f);
        }
    } else {
        if (g_prevStickClick && g_recenterHeld < kRecenterHold) {
            result.toggleView = true;
        }
        g_recenterHeld = 0.f;
    }
    g_prevStickClick = in.stickClick[s];

    if (ctx.gameplay && getSettings().vr.motionControls) {
        detect_sword(in.hands[s], head, dt, ctx);
        detect_shield(in.hands[shield_hand()], head, ctx);
    } else {
        g_prevTipValid = false;
        g_shieldUp = false;
    }

    g_crouching = ctx.gameplay && getSettings().vr.physicalCrouch && ctx.standingHeight > 0.5f &&
                  head.position.y < ctx.standingHeight * kCrouchRatio;
    return result;
}

void sync_virtual_pad(const Context& ctx) {
    const auto& in = g_lastInput;
    const int s = sword_hand();
    const int o = shield_hand();

    PADStatus status{};
    status.err = PAD_ERR_NONE;

    // Movement on the off-hand stick.
    status.stickX = to_stick(deadzone(in.stickX[o]));
    status.stickY = to_stick(deadzone(in.stickY[o]));

    // Camera on the sword-hand stick in third person; menus get the stick as well.
    if (!ctx.firstPerson || !ctx.gameplay) {
        status.substickX = to_stick(deadzone(in.stickX[s]));
        status.substickY = to_stick(deadzone(in.stickY[s]));
    }

    u16 buttons = 0;
    if (in.trigger[s] > 0.6f) buttons |= PAD_BUTTON_B;
    if (in.buttonLower[s]) buttons |= PAD_BUTTON_A;
    if (in.buttonUpper[s]) buttons |= PAD_TRIGGER_Z;
    if (in.buttonLower[o]) buttons |= PAD_BUTTON_X;
    if (in.buttonUpper[o]) buttons |= PAD_BUTTON_Y;
    if (in.menu) buttons |= PAD_BUTTON_START;
    if (in.trigger[o] > 0.6f) {
        buttons |= PAD_TRIGGER_L;
        status.triggerLeft = 200;
    }
    if (in.squeeze[o] > 0.6f) {
        buttons |= PAD_TRIGGER_R;
        status.triggerRight = 200;
    }

    if (ctx.gameplay) {
        // Raising the shield locks on and guards, like holding L.
        if (g_shieldUp) {
            buttons |= PAD_TRIGGER_L;
            status.triggerLeft = 200;
        }
        if (g_crouching && !ctx.locked) {
            buttons |= PAD_TRIGGER_R;
            status.triggerRight = 200;
        }
        if (g_latched.bash) {
            buttons |= PAD_TRIGGER_R;
            status.triggerRight = 200;
        }
        if (g_latched.spin) {
            queue_spin();
        } else if (g_latched.thrust && ctx.locked) {
            buttons |= PAD_BUTTON_B;
            status.stickX = 0;
            status.stickY = 127;
        } else if (g_latched.swing || g_latched.thrust) {
            buttons |= PAD_BUTTON_B;
        }
    } else {
        g_macro.clear();
    }
    g_latched = {};

    if (!g_macro.empty()) {
        const PadFrame frame = g_macro.front();
        g_macro.pop_front();
        buttons |= frame.buttons;
        if (frame.overrideStick) {
            status.stickX = to_stick(frame.stickX);
            status.stickY = to_stick(frame.stickY);
        }
    }

    // Off-hand stick click opens the map.
    setVirtualActionBind(ActionBinds::OPEN_MAP_SCREEN, PAD_CHAN0, in.stickClick[o]);

    status.button = buttons;
    PADSetVirtualStatus(PAD_CHAN0, &status);
}

}  // namespace dusk::vr::input
