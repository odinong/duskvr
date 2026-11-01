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
    bool dig = false;   // wolf: A
    bool spur = false;  // horse: A
    bool toss = false;  // carrying: A throws
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

// Body-driven locomotion.
Vec3 g_prevHead;
bool g_prevHeadValid = false;
float g_lungeCooldown = 0.f;
float g_digCooldown = 0.f;
float g_spurCooldown = 0.f;
float g_tossCooldown = 0.f;
bool g_climbActive = false;
float g_climbX = 0.f, g_climbY = 0.f;
float g_swimDrive = 0.f;
Vec3 g_prevGrip[2];
bool g_prevGripValid[2]{};
Vec3 g_handVelocity[2];

// Physical bow: the off hand holds the bow, the sword hand grabs the string near it and pulls.
constexpr float kNockDistance = 0.22f;  // meters between the hands to grab the string
constexpr float kFullDraw = 0.45f;      // meters of pull for a full draw
bool g_pointerCaptured = false;
bool g_bowDrawing = false;
float g_bowPull = 0.f;
float g_bowHapticTimer = 0.f;
Vec3 g_bowDir;

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

void detect_sword(const xr::HandState& hand, const Pose& head, float dt, const Context& ctx,
    float reach = kSwordLength) {
    g_swingCooldown = std::max(0.f, g_swingCooldown - dt);
    if (!hand.active || dt <= 0.f) {
        g_prevTipValid = false;
        return;
    }

    const Vec3 tip = hand.grip.position + hand.grip.forward() * reach;
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

void update_bow(const xr::InputState& in, float dt, const Context& ctx) {
    const auto& string = in.hands[sword_hand()];
    const auto& bow = in.hands[shield_hand()];
    const bool grip = in.squeeze[sword_hand()] > 0.6f || in.trigger[sword_hand()] > 0.6f;
    if (!ctx.bowReady || !string.active || !bow.active) {
        g_bowDrawing = false;
        return;
    }
    const Vec3 toBow = bow.grip.position - string.grip.position;
    const float dist = toBow.length();
    if (!g_bowDrawing) {
        if (grip && dist < kNockDistance) {
            g_bowDrawing = true;
            xr::haptic(sword_hand(), 0.4f, 0.03f);
        }
        return;
    }
    if (!grip) {
        // Letting go of the string releases the item button, which shoots.
        g_bowDrawing = false;
        xr::haptic(shield_hand(), 0.8f, 0.06f);
        return;
    }
    g_bowPull = std::clamp(dist / kFullDraw, 0.f, 1.f);
    if (dist > 0.05f) {
        g_bowDir = toBow * (1.f / dist);
    }
    // Rising string tension.
    g_bowHapticTimer -= dt;
    if (g_bowHapticTimer <= 0.f) {
        xr::haptic(sword_hand(), 0.1f + 0.4f * g_bowPull, 0.01f);
        g_bowHapticTimer = 0.08f - 0.05f * g_bowPull;
    }
}

// Hand velocities from the runtime when it reports them, else from motion between frames.
void update_hand_velocities(const xr::InputState& in, float dt) {
    for (int h = 0; h < 2; ++h) {
        const auto& hand = in.hands[h];
        if (!hand.active) {
            g_prevGripValid[h] = false;
            g_handVelocity[h] = {};
            continue;
        }
        if (hand.velocityValid) {
            g_handVelocity[h] = hand.velocity;
        } else if (g_prevGripValid[h] && dt > 0.f) {
            g_handVelocity[h] = (hand.grip.position - g_prevGrip[h]) * (1.f / dt);
        }
        g_prevGrip[h] = hand.grip.position;
        g_prevGripValid[h] = true;
    }
}

// Wolf Link: swipe a controller to claw, lunge your head forward to bite, dig with both hands low
// and moving.
void detect_wolf(const xr::InputState& in, const Pose& head, float dt, const Context& ctx) {
    detect_sword(in.hands[sword_hand()], head, dt, ctx, 0.15f);

    g_lungeCooldown = std::max(0.f, g_lungeCooldown - dt);
    if (g_prevHeadValid && dt > 0.f && g_lungeCooldown <= 0.f) {
        const Vec3 v = to_head_frame((head.position - g_prevHead) * (1.f / dt), head);
        if (-v.z > 1.3f) {
            g_latched.swing = true;
            g_lungeCooldown = 0.4f;
            xr::haptic(xr::kLeft, 0.5f, 0.05f);
            xr::haptic(xr::kRight, 0.5f, 0.05f);
        }
    }

    g_digCooldown = std::max(0.f, g_digCooldown - dt);
    const auto& l = in.hands[xr::kLeft];
    const auto& r = in.hands[xr::kRight];
    if (l.active && r.active && g_digCooldown <= 0.f) {
        const float low = -0.85f;  // meters below the eyes
        const bool handsLow = l.grip.position.y - head.position.y < low &&
                              r.grip.position.y - head.position.y < low;
        const float speed =
            0.5f * (g_handVelocity[xr::kLeft].length() + g_handVelocity[xr::kRight].length());
        if (handsLow && speed > 0.9f) {
            g_latched.dig = true;
            g_digCooldown = 0.5f;
            xr::haptic(xr::kLeft, 0.4f, 0.04f);
            xr::haptic(xr::kRight, 0.4f, 0.04f);
        }
    }
}

// Ladders, vines, ledges and ceilings: grab with either hand and pull. Pulling down (or toward
// you, hanging from a ceiling) climbs forward; sideways pulls move sideways.
void update_climb(const xr::InputState& in, const Pose& head) {
    float sx = 0.f, sy = 0.f;
    int grabbing = 0;
    for (int h = 0; h < 2; ++h) {
        if (!in.hands[h].active || in.squeeze[h] < 0.5f) {
            continue;
        }
        const Vec3 v = to_head_frame(g_handVelocity[h], head);
        sy += -v.y + v.z;
        sx += -v.x;
        ++grabbing;
    }
    g_climbActive = grabbing > 0;
    const float targetX = grabbing > 0 ? std::clamp(sx / grabbing / 0.5f, -1.f, 1.f) : 0.f;
    const float targetY = grabbing > 0 ? std::clamp(sy / grabbing / 0.5f, -1.f, 1.f) : 0.f;
    g_climbX += (targetX - g_climbX) * 0.3f;
    g_climbY += (targetY - g_climbY) * 0.3f;
}

// Swimming: breaststroke. Sweeping both hands backward pushes you forward.
void update_swim(const xr::InputState& in, const Pose& head, float dt) {
    g_swimDrive = std::max(0.f, g_swimDrive - dt * 0.8f);
    if (!in.hands[xr::kLeft].active || !in.hands[xr::kRight].active) {
        return;
    }
    const Vec3 l = to_head_frame(g_handVelocity[xr::kLeft], head);
    const Vec3 r = to_head_frame(g_handVelocity[xr::kRight], head);
    const float back = 0.5f * (l.z + r.z);
    if (back > 0.4f) {
        g_swimDrive = std::max(g_swimDrive, std::clamp((back - 0.2f) / 1.2f, 0.f, 1.f));
    }
}

// Riding: flick both hands down together, like snapping the reins, to spur Epona.
void detect_reins(const xr::InputState& in, float dt) {
    g_spurCooldown = std::max(0.f, g_spurCooldown - dt);
    if (g_spurCooldown > 0.f || !in.hands[xr::kLeft].active || !in.hands[xr::kRight].active) {
        return;
    }
    if (g_handVelocity[xr::kLeft].y < -1.4f && g_handVelocity[xr::kRight].y < -1.4f) {
        g_latched.spur = true;
        g_spurCooldown = 0.7f;
        xr::haptic(xr::kLeft, 0.6f, 0.06f);
        xr::haptic(xr::kRight, 0.6f, 0.06f);
    }
}

// Carrying something: throw it with a quick forward toss of either hand.
void detect_toss(const Pose& head, float dt) {
    g_tossCooldown = std::max(0.f, g_tossCooldown - dt);
    if (g_tossCooldown > 0.f) {
        return;
    }
    for (int h = 0; h < 2; ++h) {
        const Vec3 v = to_head_frame(g_handVelocity[h], head);
        if (-v.z > 1.6f) {
            g_latched.toss = true;
            g_tossCooldown = 0.6f;
            xr::haptic(static_cast<xr::Hand>(h), 0.6f, 0.05f);
            return;
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

void set_pointer_captured(bool captured) { g_pointerCaptured = captured; }

bool bow_aim(Vec3& dir) {
    if (!g_bowDrawing) {
        return false;
    }
    dir = g_bowDir;
    return true;
}

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

    update_hand_velocities(in, dt);
    if (ctx.gameplay && getSettings().vr.motionControls) {
        update_bow(in, dt, ctx);
        if (ctx.climbing) {
            update_climb(in, head);
        } else {
            g_climbActive = false;
        }
        if (ctx.swimming) {
            update_swim(in, head, dt);
        } else {
            g_swimDrive = 0.f;
        }
        if (ctx.riding) {
            detect_reins(in, dt);
        }
        if (ctx.carrying) {
            detect_toss(head, dt);
        }
        if (ctx.wolf) {
            detect_wolf(in, head, dt, ctx);
            g_shieldUp = false;
        } else if (ctx.climbing || ctx.swimming || ctx.carrying) {
            // Hands are busy climbing or swimming.
            g_prevTipValid = false;
            g_shieldUp = false;
        } else if (ctx.bowReady) {
            // Pulling a bowstring is not a sword swing, and the bow arm isn't a shield.
            g_prevTipValid = false;
            g_shieldUp = false;
        } else {
            detect_sword(in.hands[s], head, dt, ctx);
            detect_shield(in.hands[shield_hand()], head, ctx);
        }
    } else {
        g_prevTipValid = false;
        g_shieldUp = false;
        g_bowDrawing = false;
    }

    g_crouching = ctx.gameplay && !ctx.wolf && getSettings().vr.physicalCrouch &&
                  ctx.standingHeight > 0.5f && head.position.y < ctx.standingHeight * kCrouchRatio;
    g_prevHead = head.position;
    g_prevHeadValid = true;

    // How much the view is being moved artificially, for the comfort vignette.
    const int o = shield_hand();
    const float stick = std::min(1.f, std::hypot(deadzone(in.stickX[o]), deadzone(in.stickY[o])));
    const float smoothTurn = getSettings().vr.turnMode.getValue() == 1 ? std::fabs(turnX) : 0.f;
    result.motion = ctx.gameplay ? std::max({stick, smoothTurn, g_climbActive ? 0.5f : 0.f,
                                       g_swimDrive, ctx.riding ? 0.6f : 0.f}) :
                                   0.f;
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
    if (in.trigger[s] > 0.6f && !g_pointerCaptured) buttons |= PAD_BUTTON_B;
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

    if (ctx.gameplay && g_bowDrawing) {
        buttons |= ctx.bowSlot == 1 ? PAD_BUTTON_Y : PAD_BUTTON_X;
    }

    if (ctx.gameplay && getSettings().vr.motionControls) {
        const bool stickIdle = status.stickX == 0 && status.stickY == 0;
        if (ctx.climbing && g_climbActive && stickIdle) {
            status.stickX = to_stick(g_climbX);
            status.stickY = to_stick(g_climbY);
        }
        if (ctx.swimming && g_swimDrive > 0.05f && stickIdle) {
            status.stickY = to_stick(g_swimDrive);
        }
        if (g_latched.dig || g_latched.spur || g_latched.toss) {
            buttons |= PAD_BUTTON_A;
        }
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
