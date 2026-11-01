#include "dusk/vr/vr.h"

#include "dusk/interp/frame_interpolation.h"
#include "dusk/logging.h"
#include "dusk/menu_pointer.h"
#include "dusk/settings.h"
#include "dusk/config.hpp"
#include "dusk/vr/vr_input.h"
#include "dusk/vr/xr_session.h"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_camera_mng.h"
#include "JSystem/J3DGraphBase/J3DSys.h"
#include "Z2AudioLib/Z2Audience.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"

#include <aurora/aurora.h>
#include <aurora/rmlui.hpp>
#include <aurora/gfx.h>
#include <aurora/xr.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

namespace dusk::vr {

bool desktop_fill();

namespace {

constexpr borealis::Log Log{"vr"};

// Human Link's arms (from the model's skeleton: shoulder, upper arm, forearm, hand, weapon). Each
// controller drives the arm on its own side. The hand model's joints 1 and 2 copy the hands.
struct ArmChain {
    int shoulder, upper, lower, hand, weapon, handModelJoint;
};
constexpr ArmChain kLeftArm{6, 7, 8, 9, 10, 1};
constexpr ArmChain kRightArm{11, 12, 13, 14, 15, 2};
constexpr int kHeadJoint = 4;  // same index for human and wolf Link

const ArmChain& arm_for(xr::Hand hand) { return hand == xr::kLeft ? kLeftArm : kRightArm; }

// The game puts the sword on the "left item" joint and the shield on the "right" one (Link is left
// handed). In VR they follow the player's handedness.
bool g_itemsSwapped = false;

// Item joint axes in the controller's grip frame: the blade runs along the item joint's +X (the
// game puts the sword tip at +X), its edges face +/-Z. A sword held like the controller points
// forward (-Z) with its edges up and down (+/-Y).
constexpr Quat kItemFromGrip{-0.5f, 0.5f, 0.5f, 0.5f};

// Where each held item was placed at the last sim tick, so presentation can move it to where the
// controller is now.
struct HandPlacement {
    bool valid = false;
    Mtx simTarget;
};
HandPlacement g_placements[2];

void latch_hands(daAlink_c* link, float step);
void hide_head(daAlink_c* link);

// A joint's rest (bind) pose in model space: the inverse of its stored inverse bind matrix.
void bind_matrix(J3DModelData* data, int joint, Mtx out) {
    const auto& src = data->getInvJointMtx(joint);
    Mtx inv;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            inv[r][c] = src[r][c];
        }
    }
    MTXInverse(inv, out);
}

bool g_initialized = false;
bool g_active = false;
Pass g_pass = Pass::Flat;
bool g_hudPending = true;
bool g_hudPlaced = false;
float g_hudYaw = 0.f;
bool g_hudFollowing = false;
Vec3 g_hudAnchor;
Pose g_hudPose;
float g_hudWidth = 1.5f;
float g_hudHeight = 0.84f;
bool g_pointerDown = false;
bool g_pointerOnPanel = false;
bool g_frameOpen = false;
int g_currentEye = -1;
bool g_prevPauseOnFocusLost = true;

// Tracking space -> world mapping.
float g_yawOffset = 0.f;        // first person: world yaw of the tracking space
float g_headYawAtRecenter = 0.f;  // third person: head heading treated as "game camera forward"
Vec3 g_body;                    // tracking-space point under Link's feet (y = recenter head height)
float g_standingHeight = 0.f;
bool g_needsRecenter = true;

// Player snapshots per sim tick, for interpolated presentation.
cXyz g_linkPrev, g_linkCurr;
float g_eyeHeightPrev = 0.f, g_eyeHeightCurr = 0.f;
// Link's eyes sit ahead of his feet; the camera goes there, not above the middle of his body.
float g_eyeFwdPrev = 0.f, g_eyeFwdCurr = 0.f;
s16 g_facingPrev = 0, g_facingCurr = 0;
bool g_linkValid = false;
bool g_wasWolf = false;

// Player state sampled at the end of each sim tick, where the game's systems are consistent.
bool g_controllable = false;
bool g_locked = false;
bool g_bowReady = false;
int g_bowSlot = 0;
bool g_prevSwordHit = false;
bool g_prevDamaged = false;
bool g_wolf = false;
bool g_climbing = false;
bool g_swimming = false;
bool g_riding = false;
bool g_carrying = false;
bool g_bodyTurning = false;
bool g_stepping = false;

// Spectator (desktop) camera.
uint32_t g_frameCounter = 0;
float g_headsetFps = 0.f;
bool g_specValid = false;
Quat g_specRot;
Vec3 g_specPos;
double g_specLastTime = 0.0;

// You can lean and look around this far (meters) before Link's feet step to stay under you.
constexpr float kLeanRadius = 0.3f;

// Link's eye position before the head was posed to the headset. The camera anchors on this, not on
// the posed head, or the head would chase the camera that follows it.
cXyz g_preIkEye;
bool g_preIkEyeValid = false;

double g_lastDisplayTime = 0.0;
bool g_firstPersonFrame = false;
input::Context g_ctx;

daAlink_c* get_link() {
    auto* player = daPy_getPlayerActorClass();
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) {
        return nullptr;
    }
    return static_cast<daAlink_c*>(player);
}

float units_per_meter() { return std::clamp(getSettings().vr.worldScale.getValue(), 10.f, 1000.f); }

Vec3 to_vec(const cXyz& v) { return {v.x, v.y, v.z}; }
cXyz to_cxyz(const Vec3& v) { return {v.x, v.y, v.z}; }

s16 game_yaw(const Vec3& dir) { return cM_atan2s(dir.x, dir.z); }

// World yaw (radians, -Z convention) of a game s16 facing angle.
float yaw_from_game(s16 angle) {
    return yaw_of({cM_ssin(angle), 0.f, cM_scos(angle)});
}

// The sim-time game camera view, for third person and cutscenes.
view_class* game_view() { return dComIfGd_getView(); }

float game_camera_yaw(const view_class* view) {
    const Vec3 fwd = to_vec(view->lookat.center) - to_vec(view->lookat.eye);
    return yaw_of(fwd);
}

bool player_controllable(daAlink_c* link) {
    return link != nullptr && !dComIfGp_event_runCheck() && dMeter2Info_getPauseStatus() == 0 &&
           !link->checkDeadHP();
}

bool can_room_scale(daAlink_c* link) {
    if (!link->mLinkAcch.ChkGroundHit() || link->checkHorseRide() || link->checkCanoeRide() ||
        link->checkBoardRide())
    {
        return false;
    }
    switch (link->mProcID) {
    case daAlink_c::PROC_WAIT:
    case daAlink_c::PROC_MOVE:
    case daAlink_c::PROC_ATN_MOVE:
    case daAlink_c::PROC_ATN_ACTOR_WAIT:
    case daAlink_c::PROC_ATN_ACTOR_MOVE:
    case daAlink_c::PROC_WAIT_TURN:
    case daAlink_c::PROC_MOVE_TURN:
    case daAlink_c::PROC_TIRED_WAIT:
    case daAlink_c::PROC_SERVICE_WAIT:
    case daAlink_c::PROC_WOLF_WAIT:
    case daAlink_c::PROC_WOLF_MOVE:
    case daAlink_c::PROC_WOLF_TIRED_WAIT:
    case daAlink_c::PROC_WOLF_SERVICE_WAIT:
    case daAlink_c::PROC_WOLF_WAIT_TURN:
    case daAlink_c::PROC_WOLF_ATN_AC_MOVE:
        return true;
    default:
        return false;
    }
}

void activate() {
    g_active = true;
    g_needsRecenter = true;
    aurora::xr::set_framebuffer_override(xr::eye_width(), xr::eye_height());
    aurora_enable_vsync(false);
    // The desktop window usually loses focus to the headset; never pause for that.
    g_prevPauseOnFocusLost = getSettings().game.pauseOnFocusLost;
    aurora_set_pause_on_focus_lost(false);
    Log.info("VR active: {}x{} per eye", xr::eye_width(), xr::eye_height());
}

void deactivate() {
    g_active = false;
    aurora::xr::set_framebuffer_override(0, 0);
    aurora_enable_vsync(getSettings().video.enableVsync);
    aurora_set_pause_on_focus_lost(g_prevPauseOnFocusLost);
    PADClearVirtualStatus(PAD_CHAN0);
    Log.info("VR inactive");
}

// Rotates the play space around the head so turning never swings the camera sideways.
void turn(float rad) {
    if (rad == 0.f) {
        return;
    }
    const Vec3 head = xr::head().position;
    const Vec3 offset{head.x - g_body.x, 0.f, head.z - g_body.z};
    const Vec3 rotated = Quat::yaw(-rad).rotate(offset);
    g_body.x = head.x - rotated.x;
    g_body.z = head.z - rotated.z;
    g_yawOffset = wrap_pi(g_yawOffset + rad);
}

void build_view_matrix(Mtx out, const Quat& rot, const Vec3& pos) {
    const Vec3 r = rot.rotate({1.f, 0.f, 0.f});
    const Vec3 u = rot.rotate({0.f, 1.f, 0.f});
    const Vec3 b = rot.rotate({0.f, 0.f, 1.f});
    out[0][0] = r.x; out[0][1] = r.y; out[0][2] = r.z; out[0][3] = -r.dot(pos);
    out[1][0] = u.x; out[1][1] = u.y; out[1][2] = u.z; out[1][3] = -u.dot(pos);
    out[2][0] = b.x; out[2][1] = b.y; out[2][2] = b.z; out[2][3] = -b.dot(pos);
}

void build_world_matrix(Mtx out, const Quat& rot, const Vec3& pos) {
    const Vec3 r = rot.rotate({1.f, 0.f, 0.f});
    const Vec3 u = rot.rotate({0.f, 1.f, 0.f});
    const Vec3 b = rot.rotate({0.f, 0.f, 1.f});
    out[0][0] = r.x; out[0][1] = u.x; out[0][2] = b.x; out[0][3] = pos.x;
    out[1][0] = r.y; out[1][1] = u.y; out[1][2] = b.y; out[1][3] = pos.y;
    out[2][0] = r.z; out[2][1] = u.z; out[2][2] = b.z; out[2][3] = pos.z;
}

Quat euler_offset(float pitch, float yaw, float roll) {
    return Quat::yaw(deg2rad(yaw)) * Quat::axisAngle({1.f, 0.f, 0.f}, deg2rad(pitch)) *
           Quat::axisAngle({0.f, 0.f, 1.f}, deg2rad(roll));
}

Quat item_offset(xr::Hand hand) {
    const auto& vr = getSettings().vr;
    if (hand == input::sword_hand()) {
        return euler_offset(vr.handOffsetPitch.getValue(), vr.handOffsetYaw.getValue(),
                   vr.handOffsetRoll.getValue()) *
               kItemFromGrip;
    }
    return euler_offset(vr.shieldOffsetPitch.getValue(), vr.shieldOffsetYaw.getValue(),
               vr.shieldOffsetRoll.getValue()) *
           kItemFromGrip;
}

// Maps a tracking-space point to the world, anchored at Link (first person).
Vec3 tracking_to_world_fp(const Vec3& p, const Vec3& anchor) {
    const Vec3 local{p.x - g_body.x, p.y - g_body.y, p.z - g_body.z};
    return anchor + Quat::yaw(g_yawOffset).rotate(local) * units_per_meter();
}

// The world point that the tracking space's origin (the recentered head) maps to: Link's eyes.
Vec3 first_person_anchor(const cXyz& feet, float eyeHeight, s16 facing, float eyeFwd) {
    return {feet.x + cM_ssin(facing) * eyeFwd,
        feet.y + eyeHeight + getSettings().vr.heightOffset.getValue(),
        feet.z + cM_scos(facing) * eyeFwd};
}

// The eye anchor between the last two sim ticks.
Vec3 interpolated_anchor(float step) {
    cXyz feet;
    feet.x = g_linkPrev.x + (g_linkCurr.x - g_linkPrev.x) * step;
    feet.y = g_linkPrev.y + (g_linkCurr.y - g_linkPrev.y) * step;
    feet.z = g_linkPrev.z + (g_linkCurr.z - g_linkPrev.z) * step;
    const float eyeHeight = g_eyeHeightPrev + (g_eyeHeightCurr - g_eyeHeightPrev) * step;
    const float eyeFwd = g_eyeFwdPrev + (g_eyeFwdCurr - g_eyeFwdPrev) * step;
    const s16 facing =
        static_cast<s16>(g_facingPrev + static_cast<s16>(g_facingCurr - g_facingPrev) * step);
    return first_person_anchor(feet, eyeHeight, facing, eyeFwd);
}

// The HUD panel floats in front of you and lazily turns to follow your gaze, so it stays readable
// without being glued to your face. It holds still while a menu is open.
void update_hud_pose(float dt, bool paused) {
    if (!xr::head_valid()) {
        return;
    }
    const Pose& head = xr::head();
    const float headYaw = yaw_of(head.forward());
    if (!g_hudPlaced) {
        g_hudYaw = headYaw;
        g_hudAnchor = head.position;
        g_hudPlaced = true;
    } else if (!paused) {
        const float diff = wrap_pi(headYaw - g_hudYaw);
        if (std::fabs(diff) > deg2rad(35.f)) {
            g_hudFollowing = true;
        } else if (std::fabs(diff) < deg2rad(3.f)) {
            g_hudFollowing = false;
        }
        if (g_hudFollowing) {
            g_hudYaw = wrap_pi(g_hudYaw + diff * std::min(1.f, dt * 4.f));
        }
        const float k = std::min(1.f, dt * 2.f);
        g_hudAnchor = g_hudAnchor + (head.position - g_hudAnchor) * k;
    }

    const auto& vr = getSettings().vr;
    const float width = std::clamp(vr.hudWidth.getValue(), 0.3f, 4.f);
    const float distance = std::clamp(vr.hudDistance.getValue(), 0.4f, 5.f);
    Pose pose;
    pose.orientation = Quat::yaw(g_hudYaw);
    pose.position = g_hudAnchor + pose.orientation.rotate({0.f, 0.f, -distance});
    pose.position.y = g_hudAnchor.y + vr.hudHeight.getValue();
    g_hudPose = pose;
    g_hudWidth = width;
    g_hudHeight = width * 9.f / 16.f;
    xr::set_hud_pose(pose, g_hudWidth, g_hudHeight);
}

// In menus, the sword hand is a laser pointer onto the HUD panel; the trigger clicks.
void update_pointer() {
    const auto& in = xr::input();
    const xr::Hand handIdx = input::sword_hand();
    const auto& hand = in.hands[handIdx];
    bool onPanel = false;
    float u = 0.f, v = 0.f;
    Pose hit;
    if (menu_pointer::enabled() && menu_pointer::active() && hand.active) {
        const Vec3 origin = hand.aim.position;
        const Vec3 dir = hand.aim.forward();
        const Vec3 normal = g_hudPose.orientation.rotate({0.f, 0.f, 1.f});
        const float denom = dir.dot(normal);
        if (denom < -1e-4f) {
            const float t = (g_hudPose.position - origin).dot(normal) / denom;
            if (t > 0.f) {
                const Vec3 point = origin + dir * t;
                const Vec3 local = g_hudPose.orientation.conjugate().rotate(point - g_hudPose.position);
                u = local.x / g_hudWidth + 0.5f;
                v = 0.5f - local.y / g_hudHeight;
                onPanel = u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
                hit.orientation = g_hudPose.orientation;
                hit.position = point + normal * 0.005f;
            }
        }
    }

    auto* context = aurora::rmlui::get_context();
    if (!onPanel || context == nullptr) {
        if (g_pointerDown) {
            menu_pointer::handle_fallthrough_pointer(0.f, 0.f, menu_pointer::Phase::Cancel, true);
            g_pointerDown = false;
        }
        g_pointerOnPanel = false;
        input::set_pointer_captured(false);
        xr::set_cursor(false, {}, 0.f);
        return;
    }

    // The pointer speaks in UI (RmlUi) coordinates, which span the game's 2D screen.
    const auto dims = context->GetDimensions();
    const float x = u * static_cast<float>(dims.x);
    const float y = v * static_cast<float>(dims.y);
    const bool trigger = in.trigger[handIdx] > 0.6f;
    menu_pointer::Phase phase = menu_pointer::Phase::Move;
    if (trigger && !g_pointerDown) {
        phase = menu_pointer::Phase::Press;
        xr::haptic(handIdx, 0.3f, 0.02f);
    } else if (!trigger && g_pointerDown) {
        phase = menu_pointer::Phase::Release;
    }
    menu_pointer::handle_fallthrough_pointer(x, y, phase, true);
    g_pointerDown = trigger;
    g_pointerOnPanel = true;
    input::set_pointer_captured(true);
    xr::set_cursor(true, hit, 0.02f * g_hudWidth / 1.5f);
}

}  // namespace

void initialize() {
    if (g_initialized || !getSettings().vr.enabled) {
        return;
    }
    g_initialized = xr::initialize();
}

void shutdown() {
    if (!g_initialized) {
        return;
    }
    if (g_active) {
        deactivate();
    }
    xr::shutdown();
    g_initialized = false;
}

bool active() { return g_active; }

void recenter() {
    if (!xr::head_valid()) {
        g_needsRecenter = true;
        return;
    }
    const Pose& head = xr::head();
    const float headYaw = yaw_of(head.forward());
    g_body = {head.position.x, head.position.y, head.position.z};
    g_standingHeight = xr::floor_relative() ? head.position.y : 0.f;
    g_headYawAtRecenter = headYaw;
    if (auto* link = get_link()) {
        g_yawOffset = wrap_pi(yaw_from_game(link->shape_angle.y) - headYaw);
    } else {
        g_yawOffset = 0.f;
    }
    g_needsRecenter = false;
    g_hudPlaced = false;
    Log.info("Recentered");
}

bool begin_frame() {
    if (!g_initialized) {
        return false;
    }
    if (!xr::poll_events()) {
        shutdown();
        return false;
    }
    if (!xr::session_running()) {
        if (g_active) {
            deactivate();
        }
        return false;
    }
    if (!g_active) {
        activate();
    }
    if (!xr::begin_frame()) {
        return false;
    }
    g_frameOpen = true;
    ++g_frameCounter;
    {
        static double windowStart = 0.0;
        static uint32_t windowFrames = 0;
        const double t = xr::predicted_display_seconds();
        ++windowFrames;
        if (windowStart <= 0.0) {
            windowStart = t;
        } else if (t - windowStart >= 2.0) {
            g_headsetFps = static_cast<float>(windowFrames / (t - windowStart));
            if (std::getenv("DUSK_VR_DEBUG_PERF") != nullptr) {
                Log.info("headset {:.1f} fps", g_headsetFps);
            }
            windowStart = t;
            windowFrames = 0;
        }
    }

    const double now = xr::predicted_display_seconds();
    const float dt = g_lastDisplayTime > 0.0 ?
                         static_cast<float>(std::clamp(now - g_lastDisplayTime, 0.0, 0.1)) :
                         1.f / 90.f;
    g_lastDisplayTime = now;

    g_firstPersonFrame =
        getSettings().vr.viewMode.getValue() == 0 && g_controllable && g_linkValid;

    g_ctx.gameplay = g_controllable;
    g_ctx.firstPerson = g_firstPersonFrame;
    g_ctx.locked = g_locked;
    g_ctx.bowReady = g_bowReady;
    g_ctx.bowSlot = g_bowSlot;
    g_ctx.wolf = g_wolf;
    g_ctx.climbing = g_climbing;
    g_ctx.swimming = g_swimming;
    g_ctx.riding = g_riding;
    g_ctx.carrying = g_carrying;
    g_ctx.standingHeight = g_standingHeight;

    if (g_needsRecenter && g_linkValid) {
        recenter();
    }

    update_hud_pose(dt, dMeter2Info_getPauseStatus() != 0);
    update_pointer();

    const auto result = input::update_frame(xr::input(), xr::head(), dt, g_ctx);
    turn(result.turnRad);
    {
        static float vignette = 0.f;
        const float target = getSettings().vr.comfortVignette && g_firstPersonFrame ? result.motion : 0.f;
        vignette += (target - vignette) * std::min(1.f, dt * 6.f);
        xr::set_vignette(vignette);
    }
    if (result.recenter) {
        recenter();
    }
    if (result.toggleView) {
        getSettings().vr.viewMode.setValue(getSettings().vr.viewMode.getValue() == 0 ? 1 : 0);
        config::save();
    }
    return true;
}

// Eyes are drawn whenever tracking is valid, even if the runtime won't show them (headset off
// the head), so the desktop mirror keeps working. end_frame only submits what is visible.
int eye_count() { return g_frameOpen && xr::views_valid() ? 2 : 0; }

void begin_eye(int eye) {
    g_pass = Pass::Eye;
    g_currentEye = eye;
    const int desktop = getSettings().vr.desktopView.getValue();
    const bool crop = eye == 1 && desktop == 0;
    xr::render_eye(eye, crop);
    // The desktop window shows the right eye (16:9 band, or raw); never block on its vsync.
    aurora::xr::set_next_frame_window_present(eye == 1 && (desktop == 0 || desktop == 4));
    aurora::xr::set_next_frame_present_fill(desktop_fill());
}

// Every aurora frame in a VR frame must agree on the window layout, or the UI overlay re-lays
// itself out for each one.
bool desktop_fill() { return getSettings().vr.desktopView.getValue() <= 3; }

bool hud_pending() { return g_hudPending; }

void begin_hud(int phase) {
    g_pass = phase == 0 ? Pass::HudBlack : Pass::HudWhite;
    g_currentEye = -1;
    xr::render_hud(phase);
    aurora::xr::set_next_frame_window_present(
        phase != 0 && getSettings().vr.desktopView.getValue() == 5);
    aurora::xr::set_next_frame_present_fill(desktop_fill());
    if (phase != 0) {
        g_hudPending = false;
    }
}

Pass render_pass() { return g_pass; }

bool spectator_pending() {
    const int view = getSettings().vr.desktopView.getValue();
    const int rate = std::clamp(getSettings().vr.spectatorRate.getValue(), 1, 8);
    return view >= 1 && view <= 3 && g_frameCounter % rate == 0;
}

void begin_spectator() {
    g_pass = Pass::Spectator;
    g_currentEye = -1;
    aurora::xr::set_next_frame_window_present(true);
    // Drawn 16:9 into the eye-shaped framebuffer; stretching it over the window undoes that.
    aurora::xr::set_next_frame_present_fill(desktop_fill());
}

bool spectator_hud() { return getSettings().vr.spectatorHud; }

bool next_frame_clear(u8& level) {
    // Frame order: eye 0, eye 1, then (after a sim tick) HUD over black, HUD over white.
    if (g_pass == Pass::Eye && g_currentEye == 1 && g_hudPending) {
        level = 0;
        return true;
    }
    if (g_pass == Pass::HudBlack) {
        level = 255;
        return true;
    }
    return false;
}

void end_frame() {
    g_pass = Pass::Flat;
    if (!g_frameOpen) {
        return;
    }
    xr::end_frame();
    g_frameOpen = false;
    g_currentEye = -1;
}

void before_sim_tick() {
    if (!g_active) {
        return;
    }
    input::sync_virtual_pad(g_ctx);

    // Room scale: physically walking moves Link; the collision pass pushes him out of walls, so
    // the camera can't be walked through geometry either.
    daAlink_c* link = get_link();
    if (link == nullptr || !g_firstPersonFrame || !xr::head_valid()) {
        return;
    }
    // Like a VR avatar: standing still, the body turns to face where you look once you look far
    // enough away.
    switch (link->mProcID) {
    case daAlink_c::PROC_WAIT:
    case daAlink_c::PROC_TIRED_WAIT:
    case daAlink_c::PROC_SERVICE_WAIT:
    case daAlink_c::PROC_WOLF_WAIT:
    case daAlink_c::PROC_WOLF_TIRED_WAIT:
    case daAlink_c::PROC_WOLF_SERVICE_WAIT:
        if (!g_locked && link->mLinkAcch.ChkGroundHit()) {
            const Vec3 fwd = Quat::yaw(g_yawOffset).rotate(xr::head().forward());
            const s16 diff = static_cast<s16>(game_yaw(fwd) - link->shape_angle.y);
            if (std::abs(diff) > 0x2AAA) {
                g_bodyTurning = true;
            } else if (std::abs(diff) < 0x0400) {
                g_bodyTurning = false;
            }
            if (g_bodyTurning) {
                // Pivot around the eyes so the view doesn't swing as the body turns under it.
                const s16 before = link->shape_angle.y;
                link->shape_angle.y += std::clamp<s16>(diff, -0x0A00, 0x0A00);
                link->current.angle.y = link->shape_angle.y;
                link->current.pos.x += (cM_ssin(before) - cM_ssin(link->shape_angle.y)) * g_eyeFwdCurr;
                link->current.pos.z += (cM_scos(before) - cM_scos(link->shape_angle.y)) * g_eyeFwdCurr;
            }
        }
        break;
    default:
        g_bodyTurning = false;
        break;
    }

    const Vec3 head = xr::head().position;
    const Vec3 offset{head.x - g_body.x, 0.f, head.z - g_body.z};
    if (!getSettings().vr.roomScale || !can_room_scale(link)) {
        // Don't let a large offset accumulate while Link can't follow.
        if (offset.lengthXZ() > 0.6f) {
            g_body.x = head.x;
            g_body.z = head.z;
        }
        return;
    }
    // Lean and look around freely: the feet stay planted (the spine bends to follow your head)
    // until your head is more than kLeanRadius from them, then Link steps to catch up.
    const float dist = offset.lengthXZ();
    if (dist > kLeanRadius) {
        g_stepping = true;
    } else if (dist < 0.04f) {
        g_stepping = false;
    }
    if (!g_stepping) {
        return;
    }
    const Vec3 step = offset * 0.35f;
    const Vec3 world = Quat::yaw(g_yawOffset).rotate(step) * units_per_meter();
    link->current.pos.x += world.x;
    link->current.pos.z += world.z;
    g_body.x += step.x;
    g_body.z += step.z;
}

// Debug aid: DUSK_VR_DUMP_JOINTS=1 logs Link's skeleton (index, name, parent, rest position) once
// per form.
void dump_skeleton(J3DModel* model, const char* label) {
    J3DModelData* data = model->getModelData();
    const u16 count = data->getJointNum();
    std::vector<int> parent(count, -1);
    for (u16 i = 0; i < count; ++i) {
        J3DJoint* joint = data->getJointNodePointer(i);
        for (J3DJoint* child = joint->getChild(); child != nullptr; child = child->getYounger()) {
            parent[child->getJntNo()] = i;
        }
    }
    JUTNameTab* names = data->getJointName();
    for (u16 i = 0; i < count; ++i) {
        Mtx bind;
        bind_matrix(data, i, bind);
        Log.info("{} joint {} '{}' parent {} rest ({:.1f}, {:.1f}, {:.1f})", label, i,
            names ? names->getName(i) : "?", parent[i], bind[0][3], bind[1][3], bind[2][3]);
    }
}

void after_sim_tick() {
    // The HUD only changes when the game does.
    g_hudPending = true;
    daAlink_c* link = get_link();
    if (link == nullptr) {
        g_linkValid = false;
        g_controllable = false;
        g_locked = false;
        g_bowReady = false;
        return;
    }
    g_bowReady = !link->checkWolf() && link->checkBowAndSlingItem(link->mEquipItem) &&
                 link->checkBowAnime();
    g_bowSlot = link->mSelectItemId;
    g_wolf = link->checkWolf();
    g_riding = link->checkHorseRide();
    g_carrying = !g_wolf && link->checkGrabAnime();
    switch (link->mProcID) {
    case daAlink_c::PROC_LADDER_UP_START:
    case daAlink_c::PROC_LADDER_UP_END:
    case daAlink_c::PROC_LADDER_DOWN_START:
    case daAlink_c::PROC_LADDER_DOWN_END:
    case daAlink_c::PROC_LADDER_MOVE:
    case daAlink_c::PROC_CLIMB_UP_START:
    case daAlink_c::PROC_CLIMB_DOWN_START:
    case daAlink_c::PROC_CLIMB_MOVE_UPDOWN:
    case daAlink_c::PROC_CLIMB_MOVE_SIDE:
    case daAlink_c::PROC_CLIMB_WAIT:
    case daAlink_c::PROC_HANG_WAIT:
    case daAlink_c::PROC_HANG_MOVE:
    case daAlink_c::PROC_ROOF_HANG_WAIT:
    case daAlink_c::PROC_ROOF_HANG_FRONT_MOVE:
    case daAlink_c::PROC_ROOF_HANG_SIDE_MOVE:
        g_climbing = true;
        break;
    default:
        g_climbing = false;
        break;
    }
    switch (link->mProcID) {
    case daAlink_c::PROC_SWIM_WAIT:
    case daAlink_c::PROC_SWIM_MOVE:
    case daAlink_c::PROC_WOLF_SWIM_WAIT:
    case daAlink_c::PROC_WOLF_SWIM_MOVE:
        g_swimming = true;
        break;
    default:
        g_swimming = false;
        break;
    }

    // Feel what Link feels: the blade connecting, and getting hurt.
    if (g_active) {
        const bool swordHit = link->mAtCps[0].ChkAtHit() || link->mAtCps[1].ChkAtHit() ||
                              link->mAtCps[2].ChkAtHit();
        if (swordHit && !g_prevSwordHit) {
            xr::haptic(input::sword_hand(), 1.f, 0.08f);
        }
        g_prevSwordHit = swordHit;
        const bool damaged = link->mDamageTimer != 0;
        if (damaged && !g_prevDamaged) {
            xr::haptic(xr::kLeft, 0.9f, 0.2f);
            xr::haptic(xr::kRight, 0.9f, 0.2f);
        }
        g_prevDamaged = damaged;
    }
    g_controllable = player_controllable(link);
    static bool dumpedHuman = false, dumpedWolf = false;
    if (std::getenv("DUSK_VR_DUMP_JOINTS") != nullptr && link->mpLinkModel != nullptr) {
        bool& dumped = link->checkWolf() ? dumpedWolf : dumpedHuman;
        if (!dumped) {
            dump_skeleton(link->mpLinkModel, link->checkWolf() ? "wolf" : "human");
            if (!link->checkWolf() && link->mpLinkHandModel != nullptr) {
                J3DModelData* hands = link->mpLinkHandModel->getModelData();
                for (u16 i = 0; i < hands->getMaterialNum(); ++i) {
                    Log.info("hand material {} '{}' joint {}", i,
                        hands->getMaterialName() ? hands->getMaterialName()->getName(i) : "?",
                        hands->getMaterialNodePointer(i)->getJoint()->getJntNo());
                }
            }
            dumped = true;
        }
    }
    const daPy_py_c* player = link;
    g_locked = player->checkAttentionLock() || player->checkEnemyAttentionLock();
    const cXyz eyeRef = g_preIkEyeValid ? g_preIkEye : link->eyePos;
    g_preIkEyeValid = false;
    const float targetEye = eyeRef.y - link->current.pos.y;
    const s16 facing = link->shape_angle.y;
    const float targetFwd = std::clamp(
        (eyeRef.x - link->current.pos.x) * cM_ssin(facing) +
            (eyeRef.z - link->current.pos.z) * cM_scos(facing),
        0.f, link->checkWolf() ? 100.f : 25.f);
    const bool wolf = link->checkWolf();
    if (!g_linkValid || wolf != g_wasWolf) {
        g_linkPrev = g_linkCurr = link->current.pos;
        g_eyeHeightPrev = g_eyeHeightCurr = targetEye;
        g_eyeFwdPrev = g_eyeFwdCurr = targetFwd;
        g_facingPrev = g_facingCurr = facing;
    } else {
        g_linkPrev = g_linkCurr;
        g_linkCurr = link->current.pos;
        g_eyeHeightPrev = g_eyeHeightCurr;
        g_eyeFwdPrev = g_eyeFwdCurr;
        g_facingPrev = g_facingCurr;
        g_facingCurr = facing;
        // Smooth out animation bob in the eye position.
        g_eyeHeightCurr += (targetEye - g_eyeHeightCurr) * 0.3f;
        g_eyeFwdCurr += (targetFwd - g_eyeFwdCurr) * 0.2f;
    }
    // Teleports (loading zones, warps) shouldn't be interpolated across.
    if (g_linkPrev.abs(g_linkCurr) > 300.f) {
        g_linkPrev = g_linkCurr;
        g_facingPrev = g_facingCurr;
    }
    g_wasWolf = wolf;
    g_linkValid = true;
}

namespace {

// Fills in everything the game derives from a camera pose. The projection must already be set.
void finish_view(view_class* view, const Quat& rot, const Vec3& pos, float fovy, float aspect) {
    build_view_matrix(view->viewMtx, rot, pos);
    const Vec3 fwd = rot.rotate({0.f, 0.f, -1.f});
    const Vec3 up = rot.rotate({0.f, 1.f, 0.f});
    view->lookat.eye = to_cxyz(pos);
    view->lookat.center = to_cxyz(pos + fwd * 100.f);
    view->lookat.up = to_cxyz(up);
    view->bank = 0;
    view->fovy = fovy;
    view->aspect = aspect;

    j3dSys.setViewMtx(view->viewMtx);
    cMtx_inverse(view->viewMtx, view->invViewMtx);
    MTXCopy(view->viewMtx, view->viewMtxNoTrans);
    view->viewMtxNoTrans[0][3] = 0.0f;
    view->viewMtxNoTrans[1][3] = 0.0f;
    view->viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view->projMtx, view->viewMtx, view->projViewMtx);
    mDoLib_clipper::setup(fovy + 10.f, aspect, view->near_, view->far_);
}

// Heading and pitch only: a streamer's camera never rolls.
Quat level_rotation(const Vec3& fwd) {
    const float pitch = std::asin(std::clamp(fwd.y, -1.f, 1.f));
    return Quat::yaw(yaw_of(fwd)) * Quat::axisAngle({1.f, 0.f, 0.f}, pitch);
}

Quat nlerp(const Quat& a, const Quat& b, float t) {
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float s = dot < 0.f ? -1.f : 1.f;
    return normalized({a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t,
        a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t});
}

// The desktop's 16:9 camera, LIV style: a smoothed first-person view, the game's own (collision-
// aware) third-person camera showing your whole body, or a selfie camera in front of you.
bool apply_spectator_view(view_class* view, float step) {
    constexpr float kAspect = 16.f / 9.f;
    const int mode = getSettings().vr.desktopView.getValue() - 1;  // 0 smooth, 1 third person, 2 selfie
    const float scale = units_per_meter();
    const double now = xr::predicted_display_seconds();
    const float dt = g_specLastTime > 0.0 ? static_cast<float>(std::clamp(now - g_specLastTime, 0.0, 0.2)) : 0.f;
    g_specLastTime = now;

    if (g_firstPersonFrame && getSettings().vr.motionControls) {
        if (daAlink_c* link = get_link(); link != nullptr && !link->checkWolf()) {
            latch_hands(link, step);
        }
    }

    float fovy = std::clamp(getSettings().vr.spectatorFov.getValue(), 30.f, 120.f);
    Vec3 pos;
    Quat rot;
    if (mode == 1 || !xr::head_valid()) {
        // The game camera, as interpolated for this frame.
        pos = to_vec(view->lookat.eye);
        rot = level_rotation((to_vec(view->lookat.center) - pos).normalized());
        fovy = view->fovy;
    } else {
        const Pose& head = xr::head();
        Vec3 eye;
        Quat look;
        if (g_firstPersonFrame) {
            eye = tracking_to_world_fp(head.position, interpolated_anchor(step));
            look = Quat::yaw(g_yawOffset) * head.orientation;
        } else {
            const float base = game_camera_yaw(view) - g_headYawAtRecenter;
            eye = to_vec(view->lookat.eye) + Quat::yaw(base).rotate(head.position - g_body) * scale;
            look = Quat::yaw(base) * head.orientation;
        }
        const Vec3 fwd = look.rotate({0.f, 0.f, -1.f});
        if (mode == 0) {
            // Your view, turning, tilting and rolling with your head (or kept level, if chosen).
            pos = eye;
            rot = getSettings().vr.spectatorLevel ? level_rotation(fwd) : look;
        } else {
            // Selfie: in front of you, looking back at your face.
            const Vec3 flat = Vec3{fwd.x, 0.f, fwd.z}.normalized();
            pos = eye + flat * (1.6f * scale) + Vec3{0.f, 0.15f * scale, 0.f};
            rot = level_rotation((eye - pos).normalized());
        }
        // Take the edge off head jitter without lagging behind: at 0 smoothing the view follows
        // the headset exactly.
        const float smoothing = std::clamp(getSettings().vr.spectatorSmoothing.getValue(), 0.f, 1.f);
        const float rate = 40.f - 36.f * smoothing;  // per second
        const float k = g_specValid && smoothing > 0.f ? 1.f - std::exp(-dt * rate) : 1.f;
        g_specRot = nlerp(g_specRot, rot, k);
        g_specPos = g_specValid ? g_specPos + (pos - g_specPos) * std::min(1.f, k * 1.5f) : pos;
        g_specValid = true;
        rot = g_specRot;
        pos = g_specPos;
    }

    view->near_ = std::max(view->near_, 1.f);
    C_MTXPerspective(view->projMtx, fovy, kAspect, view->near_, view->far_);
    finish_view(view, rot, pos, fovy, kAspect);
    return true;
}

}  // namespace

bool apply_eye_view(view_class* view, float step) {
    if (g_active && g_pass == Pass::Spectator && view != nullptr) {
        return apply_spectator_view(view, step);
    }
    if (!g_active || g_currentEye < 0 || view == nullptr) {
        return false;
    }
    const xr::EyeView& eye = xr::eye(g_currentEye);
    const float scale = units_per_meter();

    Vec3 eyePos;
    Quat eyeRot;
    if (g_firstPersonFrame) {
        const float feetY = g_linkPrev.y + (g_linkCurr.y - g_linkPrev.y) * step;
        eyePos = tracking_to_world_fp(eye.pose.position, interpolated_anchor(step));
        eyePos.y = std::max(eyePos.y, feetY + 20.f);
        eyeRot = Quat::yaw(g_yawOffset) * eye.pose.orientation;
    } else {
        // Third person and cutscenes: head tracking on top of the game camera, horizon level.
        const float base = game_camera_yaw(view) - g_headYawAtRecenter;
        const Vec3 local = eye.pose.position - g_body;
        eyePos = to_vec(view->lookat.eye) + Quat::yaw(base).rotate(local) * scale;
        eyeRot = Quat::yaw(base) * eye.pose.orientation;
    }

    if (g_firstPersonFrame) {
        if (daAlink_c* link = get_link(); link != nullptr && link->mpLinkModel != nullptr) {
            // You are inside Link's head: only your own eyes skip drawing it.
            hide_head(link);
            if (!link->checkWolf() && getSettings().vr.motionControls) {
                latch_hands(link, step);
            }
        }
    }

    const float n = std::max(view->near_, 1.f);
    const float f = view->far_;
    const float tl = std::tan(eye.angleLeft), tr = std::tan(eye.angleRight);
    const float tu = std::tan(eye.angleUp), td = std::tan(eye.angleDown);
    C_MTXFrustum(view->projMtx, tu * n, td * n, tl * n, tr * n, n, f);

    // The clipper only understands symmetric frusta; cover the whole asymmetric one.
    const float halfV = std::max(std::fabs(tu), std::fabs(td));
    const float halfH = std::max(std::fabs(tl), std::fabs(tr));
    view->near_ = n;
    finish_view(view, eyeRot, eyePos, 2.f * std::atan(halfV) * (180.f / kPi), halfH / halfV);

    if (g_currentEye == 0) {
        Z2GetAudience()->setAudioCamera(view->viewMtx, view->lookat.eye, view->lookat.center,
            view->fovy, view->aspect, false, 0, false);
    }
    return true;
}

s16 control_angle_y(s16 gameCameraAngle) {
    if (!g_active || !xr::head_valid()) {
        return gameCameraAngle;
    }
    const view_class* view = game_view();
    if (view == nullptr) {
        return gameCameraAngle;
    }

    const auto& in = xr::input();
    Vec3 dir = xr::head().forward();
    const int moveHand = input::shield_hand();
    if (getSettings().vr.handOrientedMovement && in.hands[moveHand].active) {
        dir = in.hands[moveHand].aim.forward();
    }
    const float trackingYaw = yaw_of(dir);
    const float worldYaw = g_firstPersonFrame ?
                               trackingYaw + g_yawOffset :
                               trackingYaw - g_headYawAtRecenter + game_camera_yaw(view);
    const float cameraYaw = game_camera_yaw(view);
    const float diff = wrap_pi(worldYaw - cameraYaw);
    // Game angles grow counterclockwise seen from above, like our yaw.
    return static_cast<s16>(gameCameraAngle + cM_rad2s(diff));
}

bool first_person() { return g_active && g_firstPersonFrame; }

// The spectator camera shows all of Link; only your own eyes leave out what's in the way.
bool hide_link_body() {
    return first_person() && g_pass != Pass::Spectator && !getSettings().vr.fullBody;
}

bool hide_link_head() { return first_person() && g_pass != Pass::Spectator; }

namespace {

Vec3 mtx_pos(const Mtx m) { return {m[0][3], m[1][3], m[2][3]}; }

// Rotates a world matrix by q about pivot, then moves it.
void rotate_about(Mtx m, const Quat& q, const Vec3& pivot, const Vec3& move) {
    for (int c = 0; c < 3; ++c) {
        const Vec3 axis = q.rotate({m[0][c], m[1][c], m[2][c]});
        m[0][c] = axis.x;
        m[1][c] = axis.y;
        m[2][c] = axis.z;
    }
    const Vec3 p = q.rotate(mtx_pos(m) - pivot) + pivot + move;
    m[0][3] = p.x;
    m[1][3] = p.y;
    m[2][3] = p.z;
}

void collect_subtree(J3DModelData* data, int root, std::vector<u16>& out) {
    out.clear();
    std::vector<J3DJoint*> stack{data->getJointNodePointer(root)};
    while (!stack.empty()) {
        J3DJoint* joint = stack.back();
        stack.pop_back();
        out.push_back(joint->getJntNo());
        for (J3DJoint* child = joint->getChild(); child != nullptr; child = child->getYounger()) {
            stack.push_back(child);
        }
    }
}

// Two-bone IK: bends the arm so the weapon joint lands on the controller. The elbow hangs down
// and out to the side, the way a relaxed arm does. Reaching past full extension stretches the arm
// a little rather than letting the hand fall short of the controller.
void solve_arm(J3DModel* model, const ArmChain& chain, const Mtx weaponTarget, const Vec3& bodyFwd,
    const Vec3& bodyRight, bool leftSide) {
    J3DModelData* data = model->getModelData();
    const auto M = [&](int j) { return model->getAnmMtx(j); };

    // Where the hand must be for the weapon joint to hit the target.
    Mtx handInv, rel, relInv, handTarget;
    MTXInverse(M(chain.hand), handInv);
    MTXConcat(handInv, M(chain.weapon), rel);
    MTXInverse(rel, relInv);
    MTXConcat(weaponTarget, relInv, handTarget);

    const Vec3 shoulder = mtx_pos(M(chain.upper));
    const Vec3 elbow = mtx_pos(M(chain.lower));
    const Vec3 wrist = mtx_pos(M(chain.hand));
    const Vec3 target = mtx_pos(handTarget);
    float upperLen = (elbow - shoulder).length();
    float lowerLen = (wrist - elbow).length();
    const Vec3 toTarget = target - shoulder;
    const float dist = toTarget.length();
    if (upperLen < 1e-3f || lowerLen < 1e-3f || dist < 1e-3f) {
        return;
    }
    const float stretch = std::clamp(dist / ((upperLen + lowerLen) * 0.999f), 1.f, 1.3f);
    upperLen *= stretch;
    lowerLen *= stretch;
    const Vec3 dir = toTarget * (1.f / dist);
    const float d = std::clamp(dist, std::fabs(upperLen - lowerLen) + 1e-3f,
        (upperLen + lowerLen) * 0.9999f);
    const float along = (upperLen * upperLen - lowerLen * lowerLen + d * d) / (2.f * d);
    const float out = std::sqrt(std::max(0.f, upperLen * upperLen - along * along));
    const Vec3 side = leftSide ? bodyRight * -1.f : bodyRight;
    const Vec3 pole = Vec3{0.f, -1.f, 0.f} + side * 0.6f + bodyFwd * -0.3f;
    Vec3 bend = pole - dir * pole.dot(dir);
    bend = bend.length() > 1e-4f ? bend.normalized() : Vec3{0.f, -1.f, 0.f};
    const Vec3 newElbow = shoulder + dir * along + bend * out;

    std::vector<u16> joints;
    // Upper arm (and everything below it) swings about the shoulder.
    collect_subtree(data, chain.upper, joints);
    const Quat upperRot = from_to(elbow - shoulder, newElbow - shoulder);
    for (u16 j : joints) {
        rotate_about(M(j), upperRot, shoulder, {});
    }
    // Forearm moves to the new elbow and turns toward the wrist target.
    collect_subtree(data, chain.lower, joints);
    const Vec3 movedElbow = mtx_pos(M(chain.lower));
    const Vec3 movedWrist = mtx_pos(M(chain.hand));
    const Quat lowerRot = from_to(movedWrist - movedElbow, target - newElbow);
    for (u16 j : joints) {
        rotate_about(M(j), lowerRot, movedElbow, newElbow - movedElbow);
    }
    // The hand (and weapon) take the controller's pose exactly.
    collect_subtree(data, chain.hand, joints);
    Mtx inv, delta;
    MTXInverse(M(chain.hand), inv);
    MTXConcat(handTarget, inv, delta);
    for (u16 j : joints) {
        Mtx m;
        MTXConcat(delta, M(j), m);
        MTXCopy(m, M(j));
    }
}

// Joint parents from the model's joint tree.
void joint_parents(J3DModelData* data, std::vector<int>& parent) {
    parent.assign(data->getJointNum(), -1);
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        for (J3DJoint* c = data->getJointNodePointer(i)->getChild(); c != nullptr; c = c->getYounger()) {
            parent[c->getJntNo()] = i;
        }
    }
}

// Link's neutral standing pose in model space: the model's rest pose (a T-pose), with human Link's
// arms lowered to his sides.
void neutral_pose(J3DModelData* data, int joint, bool wolf, const std::vector<int>& parent, Mtx out) {
    bind_matrix(data, joint, out);
    if (wolf) {
        return;
    }
    for (int j = joint; j >= 0; j = parent[j]) {
        if (j == kLeftArm.upper || j == kRightArm.upper) {
            Mtx shoulder;
            bind_matrix(data, j, shoulder);
            const float angle = deg2rad(j == kLeftArm.upper ? -80.f : 80.f);
            rotate_about(out, Quat::axisAngle({0.f, 0.f, 1.f}, angle), mtx_pos(shoulder), {});
            return;
        }
    }
}

bool is_idle_proc(u16 proc) {
    switch (proc) {
    case daAlink_c::PROC_WAIT:
    case daAlink_c::PROC_TIRED_WAIT:
    case daAlink_c::PROC_SERVICE_WAIT:
    case daAlink_c::PROC_ATN_ACTOR_WAIT:
    case daAlink_c::PROC_WOLF_WAIT:
    case daAlink_c::PROC_WOLF_TIRED_WAIT:
    case daAlink_c::PROC_WOLF_SERVICE_WAIT:
        return true;
    default:
        return false;
    }
}

bool is_walk_proc(u16 proc) {
    switch (proc) {
    case daAlink_c::PROC_MOVE:
    case daAlink_c::PROC_ATN_MOVE:
    case daAlink_c::PROC_ATN_ACTOR_MOVE:
    case daAlink_c::PROC_WAIT_TURN:
    case daAlink_c::PROC_MOVE_TURN:
    case daAlink_c::PROC_WOLF_MOVE:
    case daAlink_c::PROC_WOLF_ATN_AC_MOVE:
    case daAlink_c::PROC_WOLF_WAIT_TURN:
        return true;
    default:
        return false;
    }
}

// Steadies the body in first person. Standing still, Link holds a neutral stance instead of his
// idle fidgets. Walking or running, only the legs animate: the spine, head and arms stay in the
// neutral pose, riding the hips' height, while anything that reaches the ground (legs, and the
// tunic skirt hanging off the hips) keeps the animation. Returns whether the pose changed.
bool steady_body(daAlink_c* link, J3DModel* model) {
    const auto& vr = getSettings().vr;
    const bool idle = vr.stillIdle.getValue() && is_idle_proc(link->mProcID);
    const bool walk = vr.legsOnlyWalk.getValue() && is_walk_proc(link->mProcID);
    if (!idle && !walk) {
        return false;
    }
    const bool wolf = link->checkWolf();
    J3DModelData* data = model->getModelData();
    const u16 count = data->getJointNum();
    static std::vector<int> parent;
    joint_parents(data, parent);

    static std::vector<std::array<std::array<f32, 4>, 3>> before;
    before.resize(count);
    for (u16 j = 0; j < count; ++j) {
        std::memcpy(before[j].data(), model->getAnmMtx(j), sizeof(Mtx));
    }
    const auto old = [&](int j) { return reinterpret_cast<MtxP>(before[j].data()); };

    MtxP base = model->getBaseTRMtx();
    Mtx rootRest;
    bind_matrix(data, 0, rootRest);
    Mtx rootRestWorld;
    MTXConcat(base, rootRest, rootRestWorld);
    // Idle: feet planted at Link's position. Walking: follow the hips' bob.
    const Vec3 offset = idle ? Vec3{} : mtx_pos(old(0)) - mtx_pos(rootRestWorld);
    const auto setNeutral = [&](int j) {
        Mtx rest, world;
        neutral_pose(data, j, wolf, parent, rest);
        MTXConcat(base, rest, world);
        world[0][3] += offset.x;
        world[1][3] += offset.y;
        world[2][3] += offset.z;
        MTXCopy(world, model->getAnmMtx(j));
    };

    if (idle) {
        for (u16 j = 0; j < count; ++j) {
            setNeutral(j);
        }
        return true;
    }

    // The spine: every joint from the root to the head.
    std::vector<bool> onSpine(count, false);
    for (int j = kHeadJoint; j > 0 && j < count; j = parent[j]) {
        onSpine[j] = true;
    }
    for (u16 j = 1; j < count; ++j) {
        if (onSpine[j]) {
            setNeutral(j);
        }
    }
    const float ground = rootRest[1][3] * 0.35f;
    std::vector<u16> subtree;
    for (u16 c = 1; c < count; ++c) {
        const int p = parent[c];
        if (onSpine[c] || p <= 0 || !onSpine[p]) {
            continue;
        }
        collect_subtree(data, c, subtree);
        bool reachesGround = false;
        for (u16 j : subtree) {
            Mtx rest;
            bind_matrix(data, j, rest);
            reachesGround = reachesGround || rest[1][3] < ground;
        }
        if (reachesGround) {
            // A leg on the spine (wolf forelegs): keep animating, carried by the steadied spine.
            Mtx inv, delta;
            MTXInverse(old(p), inv);
            MTXConcat(model->getAnmMtx(p), inv, delta);
            for (u16 j : subtree) {
                MTXConcat(delta, old(j), model->getAnmMtx(j));
            }
        } else {
            for (u16 j : subtree) {
                setNeutral(j);
            }
        }
    }
    return true;
}

// Skinned vertices (elbows, shoulders) follow the new joints, and interpolation sees the posed
// skeleton rather than the animation's.
void finish_pose(J3DModel* model) {
    model->calcWeightEnvelopeMtx();
    J3DModelData* data = model->getModelData();
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        interp::record_final_mtx(model->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        interp::record_final_mtx(model->getWeightAnmMtx(i));
    }
}

// Eye position in the head joint's frame (from the game's own eye computation).
constexpr Vec3 kHumanLocalEye{12.f, -8.f, 0.f};
constexpr Vec3 kWolfLocalEye{30.f, -5.f, 0.f};

Vec3 mul_point(const Mtx m, const Vec3& v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3],
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3]};
}

Quat mtx_rot(const Mtx m) {
    return quat_from_axes({m[0][0], m[1][0], m[2][0]}, {m[0][1], m[1][1], m[2][1]},
        {m[0][2], m[1][2], m[2][2]});
}

// Puts Link's head where the headset is, the way a VR avatar does: the spine and neck bend toward
// it (so leaning leans the chest and shoulders) and twist with where you look, and the head takes
// the headset's orientation. The hips and feet stay where they are.
void solve_spine(daAlink_c* link, J3DModel* model, const Vec3& anchor) {
    J3DModelData* data = model->getModelData();
    const auto M = [&](int j) { return model->getAnmMtx(j); };
    const Pose& hmd = xr::head();

    // The head joint's rest orientation, relative to looking straight along Link's facing.
    Mtx bindHead, restHead;
    bind_matrix(data, kHeadJoint, bindHead);
    MTXConcat(model->getBaseTRMtx(), bindHead, restHead);
    const float facingYaw = yaw_from_game(link->shape_angle.y);
    const Quat headFromView = Quat::yaw(facingYaw).conjugate() * mtx_rot(restHead);

    const Quat view = Quat::yaw(g_yawOffset) * hmd.orientation;
    const Quat targetRot = normalized(view * headFromView);
    const Vec3 eye = tracking_to_world_fp(hmd.position, anchor);
    const Vec3 targetPos = eye - targetRot.rotate(kHumanLocalEye);

    // Lean: swing the upper body about the base of the spine toward the head target.
    constexpr int kSpine[] = {1, 2, 3};  // backbone1, backbone2, neck
    constexpr float kLean[] = {0.4f, 0.35f, 0.25f};
    constexpr float kTwist[] = {0.2f, 0.3f, 0.3f};  // the head itself takes the rest
    const Vec3 root = mtx_pos(M(kSpine[0]));
    Quat lean = from_to(mtx_pos(M(kHeadJoint)) - root, targetPos - root);
    const float leanAngle = 2.f * std::acos(std::fmin(1.f, std::fabs(lean.w)));
    const float maxLean = deg2rad(55.f);
    if (leanAngle > maxLean) {
        lean = partial(lean, maxLean / leanAngle);
    }
    // Twist: look sideways and the shoulders follow part of the way.
    const float twistAngle = std::clamp(
        wrap_pi(yaw_of(view.rotate({0.f, 0.f, -1.f})) - facingYaw), deg2rad(-70.f), deg2rad(70.f));
    const Quat twist = Quat::yaw(twistAngle);

    std::vector<u16> joints;
    for (int i = 0; i < 3; ++i) {
        const Vec3 pivot = mtx_pos(M(kSpine[i]));
        const Quat q = normalized(partial(lean, kLean[i]) * partial(twist, kTwist[i]));
        collect_subtree(data, kSpine[i], joints);
        for (u16 j : joints) {
            rotate_about(M(j), q, pivot, {});
        }
    }
    // The head looks exactly where you look.
    const Quat delta = normalized(targetRot * mtx_rot(M(kHeadJoint)).conjugate());
    const Vec3 headPos = mtx_pos(M(kHeadJoint));
    collect_subtree(data, kHeadJoint, joints);
    for (u16 j : joints) {
        rotate_about(M(j), delta, headPos, {});
    }

    // Debug aid: DUSK_VR_DEBUG_IK=1 logs how far the posed eyes land from the camera.
    static int counter = 0;
    if (std::getenv("DUSK_VR_DEBUG_IK") != nullptr && counter++ % 60 == 0) {
        const Vec3 posedEye = mul_point(M(kHeadJoint), kHumanLocalEye);
        Log.info("spine IK: eye error {:.1f} units, lean {:.0f} deg, twist {:.0f} deg",
            (posedEye - eye).length(), leanAngle * 180.f / kPi, twistAngle * 180.f / kPi);
    }
}

void restore_item_joints(daAlink_c* link) {
    if (g_itemsSwapped && !link->checkWolf()) {
        link->mLeftItemJntNo = kLeftArm.weapon;
        link->mRightItemJntNo = kRightArm.weapon;
    }
    g_itemsSwapped = false;
}

}  // namespace

void override_link_hands(daAlink_c* link) {
    J3DModel* model = link->mpLinkModel;
    if (!first_person() || model == nullptr || !g_linkValid) {
        restore_item_joints(link);
        g_placements[xr::kLeft].valid = g_placements[xr::kRight].valid = false;
        return;
    }
    bool posed = getSettings().vr.fullBody && steady_body(link, model);

    // Where the eyes are before the head follows the headset (the camera anchors here).
    const Vec3 eyeLocal = link->checkWolf() ? kWolfLocalEye : kHumanLocalEye;
    const Vec3 preEye = mul_point(model->getAnmMtx(kHeadJoint), eyeLocal);
    g_preIkEye = to_cxyz(preEye);
    g_preIkEyeValid = true;

    if (getSettings().vr.fullBody && !link->checkWolf() && xr::head_valid()) {
        solve_spine(link, model,
            first_person_anchor(link->current.pos, g_eyeHeightCurr, link->shape_angle.y, g_eyeFwdCurr));
        posed = true;
    }

    if (!getSettings().vr.motionControls || link->checkWolf()) {
        restore_item_joints(link);
        g_placements[xr::kLeft].valid = g_placements[xr::kRight].valid = false;
        if (posed) {
            finish_pose(model);
        }
        return;
    }

    // The sword goes in the player's sword hand, on that side of Link's body. The bow and slingshot
    // stay in the off hand (Link's own left), so the sword hand draws the string.
    const bool swordRight =
        !getSettings().vr.leftHanded && !link->checkBowAndSlingItem(link->mEquipItem);
    link->mLeftItemJntNo = swordRight ? kRightArm.weapon : kLeftArm.weapon;
    link->mRightItemJntNo = swordRight ? kLeftArm.weapon : kRightArm.weapon;
    g_itemsSwapped = swordRight;

    const auto& in = xr::input();
    const Vec3 anchor =
        first_person_anchor(link->current.pos, g_eyeHeightCurr, link->shape_angle.y, g_eyeFwdCurr);
    const Vec3 fwd{cM_ssin(link->shape_angle.y), 0.f, cM_scos(link->shape_angle.y)};
    const Vec3 right{-fwd.z, 0.f, fwd.x};

    for (const xr::Hand hand : {xr::kLeft, xr::kRight}) {
        const auto& state = in.hands[hand];
        g_placements[hand].valid = false;
        if (!state.active) {
            continue;
        }
        Mtx& target = g_placements[hand].simTarget;
        build_world_matrix(target, Quat::yaw(g_yawOffset) * state.grip.orientation * item_offset(hand),
            tracking_to_world_fp(state.grip.position, anchor));
        solve_arm(model, arm_for(hand), target, fwd, right, hand == xr::kLeft);
        g_placements[hand].valid = true;
        posed = true;
    }
    if (posed) {
        finish_pose(model);
    }
}

namespace {

bool attached_to(J3DModel* model, MtxP joint) {
    return model != nullptr && std::memcmp(model->getBaseTRMtx(), joint, sizeof(Mtx)) == 0;
}

void override_model(J3DModel* model, const Mtx delta) {
    const u16 joints = model->getModelData()->getJointNum();
    for (u16 i = 0; i < joints; ++i) {
        Mtx m;
        MTXConcat(delta, model->getAnmMtx(i), m);
        interp::set_presentation_override(model->getAnmMtx(i), m);
    }
    const u16 weights = model->getModelData()->getWEvlpMtxNum();
    for (u16 i = 0; i < weights; ++i) {
        Mtx m;
        MTXConcat(delta, model->getWeightAnmMtx(i), m);
        interp::set_presentation_override(model->getWeightAnmMtx(i), m);
    }
}

// Late latch: at draw time, move everything a hand holds from where the controller was at the
// last sim tick to where it is for this frame. Hands then track at the headset's rate instead of
// the game's 30 Hz.
void latch_hands(daAlink_c* link, float step) {
    const auto& in = xr::input();
    const Vec3 anchor = interpolated_anchor(step);

    const auto latch = [&](xr::Hand hand) {
        const ArmChain& chain = arm_for(hand);
        const int handJoint = chain.hand;
        const int itemJoint = chain.weapon;
        const int handModelJoint = chain.handModelJoint;
        const auto& state = in.hands[hand];
        if (!g_placements[hand].valid || !state.active) {
            return;
        }
        Mtx now, inv, delta;
        build_world_matrix(now, Quat::yaw(g_yawOffset) * state.grip.orientation * item_offset(hand),
            tracking_to_world_fp(state.grip.position, anchor));
        MTXInverse(g_placements[hand].simTarget, inv);
        MTXConcat(now, inv, delta);

        MtxP item = link->mpLinkModel->getAnmMtx(itemJoint);
        for (MtxP joint : {link->mpLinkModel->getAnmMtx(handJoint), item}) {
            Mtx m;
            MTXConcat(delta, joint, m);
            interp::set_presentation_override(joint, m);
        }
        if (link->mpLinkHandModel != nullptr) {
            MtxP handModel = link->mpLinkHandModel->getAnmMtx(handModelJoint);
            Mtx m;
            MTXConcat(delta, handModel, m);
            interp::set_presentation_override(handModel, m);
        }
        for (J3DModel* held : {link->mSwordModel, link->mShieldModel, link->mHeldItemModel}) {
            if (attached_to(held, item)) {
                override_model(held, delta);
            }
        }
    };
    latch(xr::kLeft);
    latch(xr::kRight);
}

void collapse(MtxP m) {
    Mtx collapsed{};
    collapsed[0][3] = m[0][3];
    collapsed[1][3] = m[1][3];
    collapsed[2][3] = m[2][3];
    interp::set_presentation_override(m, collapsed);
}

void collapse_model(J3DModel* model) {
    if (model == nullptr) {
        return;
    }
    J3DModelData* data = model->getModelData();
    for (u16 i = 0; i < data->getJointNum(); ++i) {
        collapse(model->getAnmMtx(i));
    }
    for (u16 i = 0; i < data->getWEvlpMtxNum(); ++i) {
        collapse(model->getWeightAnmMtx(i));
    }
}

// For the current eye frame only, shrinks Link's head (and his face and hat models) to nothing.
// His draw lists are built once per sim tick and also feed the spectator view, so the head is
// hidden here rather than left out of them.
void hide_head(daAlink_c* link) {
    J3DModel* model = link->mpLinkModel;
    if (!getSettings().vr.fullBody) {
        collapse_model(model);
    }
    if (!link->checkWolf()) {
        collapse_model(link->mpLinkFaceModel);
        collapse_model(link->mpLinkHatModel);
        collapse_model(link->mpDemoFCBlendModel);
        collapse_model(link->mpDemoFCTongueModel);
        // The Hawkeye sits on the head; in your own view it would cover your eyes.
        if (attached_to(link->mHeldItemModel, model->getAnmMtx(kHeadJoint))) {
            collapse_model(link->mHeldItemModel);
        }
    }
    // Collapse the neck too (the head's parent), or you look down into the collar.
    J3DModelData* data = model->getModelData();
    int root = kHeadJoint;
    for (u16 i = 1; i < data->getJointNum(); ++i) {
        J3DJoint* first = data->getJointNodePointer(i)->getChild();
        // Only a neck that carries nothing but the head (not a spine joint with legs on it).
        if (first != nullptr && first->getJntNo() == kHeadJoint && first->getYounger() == nullptr) {
            root = i;
        }
    }
    static std::vector<u16> joints;
    collect_subtree(data, root, joints);
    for (u16 j : joints) {
        collapse(model->getAnmMtx(j));
    }
}

}  // namespace

void override_hand_shapes(daAlink_c* link, u8& left, u8& right) {
    // Debug aid: DUSK_VR_HAND_CYCLE=1 steps both hands through every shape every two seconds.
    if (std::getenv("DUSK_VR_HAND_CYCLE") != nullptr) {
        static u32 ticks = 0;
        const u32 step = ticks++ / 60;
        left = static_cast<u8>(step % 5);
        right = static_cast<u8>(5 + step % 6);
        if (ticks % 60 == 1) {
            Log.info("hand cycle: left {} right {}", left, right);
        }
        return;
    }
    if (!first_person() || !getSettings().vr.motionControls || link->checkWolf()) {
        return;
    }
    // Link's hand model has a few shapes per hand (from how his animations use them):
    //   B fist, C weapon/shield grip, E small-object pinch, F climbing hook, G relaxed.
    struct Shapes {
        u8 fist, grip, pinch, hook, relaxed;
    };
    constexpr Shapes kLeftShapes{0, 1, 2, 3, 4};
    constexpr Shapes kRightShapes{5, 6, 8, 9, 10};
    const auto& in = xr::input();
    const auto pick = [&](xr::Hand hand, const Shapes& shapes, u8 current) -> u8 {
        if (!in.hands[hand].active) {
            return current;
        }
        MtxP weapon = link->mpLinkModel->getAnmMtx(arm_for(hand).weapon);
        if (attached_to(link->mShieldModel, weapon)) {
            return shapes.grip;
        }
        if (attached_to(link->mSwordModel, weapon) || attached_to(link->mHeldItemModel, weapon)) {
            return shapes.fist;
        }
        // An empty hand follows your fingers.
        const float* c = in.curl[hand];
        const float others = (c[2] + c[3] + c[4]) / 3.f;
        if (others > 0.65f && c[1] > 0.6f) {
            return shapes.fist;
        }
        if (c[1] > 0.55f && c[0] > 0.45f && others < 0.4f) {
            return shapes.pinch;
        }
        if (others > 0.3f || c[1] > 0.4f) {
            return shapes.hook;
        }
        return shapes.relaxed;
    };
    left = pick(xr::kLeft, kLeftShapes, left);
    right = pick(xr::kRight, kRightShapes, right);
}

bool hand_aim(s16& yaw, s16& pitch) {
    if (!g_active || !getSettings().vr.motionControls) {
        return false;
    }
    // A drawn bow shoots along the string; otherwise point with the hand holding the item (the bow
    // hand for the bow and slingshot).
    Vec3 trackingDir;
    if (!input::bow_aim(trackingDir)) {
        const auto& hand = xr::input().hands[g_bowReady ? input::shield_hand() : input::sword_hand()];
        if (!hand.active) {
            return false;
        }
        trackingDir = hand.aim.forward();
    }
    float yawRad;
    if (g_firstPersonFrame) {
        yawRad = g_yawOffset;
    } else {
        const view_class* view = game_view();
        if (view == nullptr) {
            return false;
        }
        yawRad = game_camera_yaw(view) - g_headYawAtRecenter;
    }
    const Vec3 dir = Quat::yaw(yawRad).rotate(trackingDir);
    yaw = game_yaw(dir);
    pitch = cM_atan2s(-dir.y, dir.lengthXZ());
    return true;
}

void draw_imgui_window(bool* open) {
    if (!ImGui::Begin("VR", open)) {
        ImGui::End();
        return;
    }
    auto& vr = getSettings().vr;
    bool changed = false;
    const auto checkbox = [&](const char* label, ConfigVar<bool>& var) {
        bool v = var;
        if (ImGui::Checkbox(label, &v)) {
            var.setValue(v);
            changed = true;
        }
    };
    const auto sliderF = [&](const char* label, ConfigVar<float>& var, float lo, float hi,
                             const char* fmt = "%.2f") {
        float v = var;
        if (ImGui::SliderFloat(label, &v, lo, hi, fmt)) {
            var.setValue(v);
            changed = true;
        }
    };
    const auto combo = [&](const char* label, ConfigVar<int>& var, const char* items) {
        int v = var;
        if (ImGui::Combo(label, &v, items)) {
            var.setValue(v);
            changed = true;
        }
    };

    ImGui::Text("Status: %s", !g_initialized ? "no headset" : g_active ? "running" : "waiting for headset");
    if (g_active) {
        ImGui::Text("Per-eye resolution: %ux%u", xr::eye_width(), xr::eye_height());
        ImGui::Text("Headset frame rate: %.0f fps", g_headsetFps);
    }
    checkbox("Enable VR (restart to apply)", vr.enabled);
    ImGui::SeparatorText("View");
    combo("Perspective", vr.viewMode, "First person\0Third person\0");
    sliderF("World scale (units/m)", vr.worldScale, 50.f, 200.f, "%.0f");
    sliderF("Height offset", vr.heightOffset, -80.f, 80.f, "%.0f");
    checkbox("Level horizon", vr.levelHorizon);
    if (ImGui::Button("Recenter")) {
        recenter();
    }
    ImGui::SeparatorText("Desktop / streaming");
    combo("Desktop window shows", vr.desktopView,
        "Headset view (16:9)\0Smooth first person\0Third person, whole body\0"
        "Selfie camera\0Raw right eye\0HUD panel\0");
    checkbox("HUD on the desktop view", vr.spectatorHud);
    sliderF("Desktop field of view", vr.spectatorFov, 40.f, 110.f, "%.0f deg");
    sliderF("Desktop smoothing", vr.spectatorSmoothing, 0.f, 1.f);
    checkbox("Keep desktop horizon level", vr.spectatorLevel);
    {
        int v = vr.spectatorRate;
        if (ImGui::SliderInt("Draw desktop every N headset frames", &v, 1, 4)) {
            vr.spectatorRate.setValue(v);
            changed = true;
        }
    }
    ImGui::SeparatorText("HUD panel");
    sliderF("HUD distance", vr.hudDistance, 0.5f, 3.f, "%.2f m");
    sliderF("HUD width", vr.hudWidth, 0.5f, 3.f, "%.2f m");
    sliderF("HUD height", vr.hudHeight, -1.f, 1.f, "%.2f m");
    ImGui::SeparatorText("Movement");
    combo("Turning", vr.turnMode, "Snap\0Smooth\0Off\0");
    {
        int v = vr.snapTurnDegrees;
        if (ImGui::SliderInt("Snap angle", &v, 15, 90)) {
            vr.snapTurnDegrees.setValue(v);
            changed = true;
        }
    }
    sliderF("Smooth turn speed", vr.smoothTurnSpeed, 30.f, 360.f, "%.0f deg/s");
    checkbox("Move where the off hand points", vr.handOrientedMovement);
    checkbox("Room scale (walk to move Link)", vr.roomScale);
    checkbox("Physical crouch", vr.physicalCrouch);
    ImGui::SeparatorText("Motion controls");
    checkbox("Motion controls", vr.motionControls);
    checkbox("Full body (first person)", vr.fullBody);
    checkbox("No idle animations (stand still)", vr.stillIdle);
    checkbox("Walking animates the legs only", vr.legsOnlyWalk);
    checkbox("Comfort vignette while moving", vr.comfortVignette);
    checkbox("Left handed (sword in left controller)", vr.leftHanded);
    sliderF("Swing speed", vr.swingSpeed, 1.0f, 5.0f, "%.1f m/s");
    sliderF("Sword pitch", vr.handOffsetPitch, -180.f, 180.f, "%.0f deg");
    sliderF("Sword yaw", vr.handOffsetYaw, -180.f, 180.f, "%.0f deg");
    sliderF("Sword roll", vr.handOffsetRoll, -180.f, 180.f, "%.0f deg");
    sliderF("Shield pitch", vr.shieldOffsetPitch, -180.f, 180.f, "%.0f deg");
    sliderF("Shield yaw", vr.shieldOffsetYaw, -180.f, 180.f, "%.0f deg");
    sliderF("Shield roll", vr.shieldOffsetRoll, -180.f, 180.f, "%.0f deg");
    ImGui::SeparatorText("Rendering");
    sliderF("Resolution scale (restart)", vr.resolutionScale, 0.5f, 1.5f);
    if (changed) {
        config::save();
    }
    ImGui::End();
}

}  // namespace dusk::vr
