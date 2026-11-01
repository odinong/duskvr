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
#include <cstring>
#include <cmath>

namespace dusk::vr {
namespace {

constexpr borealis::Log Log{"vr"};

// Human Link's hand joints and the item joints parented to them. Link is left handed: the sword
// sits in the left hand, the shield in the right. The hand model's joints 1 and 2 copy the hands.
constexpr int kSwordHandJoint = 9;
constexpr int kSwordItemJoint = 10;
constexpr int kShieldHandJoint = 14;
constexpr int kShieldItemJoint = 15;
constexpr int kSwordHandModelJoint = 1;
constexpr int kShieldHandModelJoint = 2;

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
bool g_linkValid = false;
bool g_wasWolf = false;

// Player state sampled at the end of each sim tick, where the game's systems are consistent.
bool g_controllable = false;
bool g_locked = false;
bool g_bowReady = false;
int g_bowSlot = 0;
bool g_prevSwordHit = false;
bool g_prevDamaged = false;

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

Vec3 first_person_anchor(const cXyz& feet, float eyeHeight) {
    return {feet.x, feet.y + eyeHeight + getSettings().vr.heightOffset.getValue(), feet.z};
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
    g_ctx.standingHeight = g_standingHeight;

    if (g_needsRecenter && g_linkValid) {
        recenter();
    }

    update_hud_pose(dt, dMeter2Info_getPauseStatus() != 0);
    update_pointer();

    const auto result = input::update_frame(xr::input(), xr::head(), dt, g_ctx);
    turn(result.turnRad);
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
    xr::render_eye(eye);
    // Mirror the last eye to the desktop window; never block on the window's vsync.
    aurora::xr::set_next_frame_window_present(eye == 1 && getSettings().vr.mirror.getValue() == 0);
}

bool hud_pending() { return g_hudPending; }

void begin_hud(int phase) {
    g_pass = phase == 0 ? Pass::HudBlack : Pass::HudWhite;
    g_currentEye = -1;
    xr::render_hud(phase);
    aurora::xr::set_next_frame_window_present(phase != 0 && getSettings().vr.mirror.getValue() == 1);
    if (phase != 0) {
        g_hudPending = false;
    }
}

Pass render_pass() { return g_pass; }

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
    if (offset.lengthXZ() < 0.01f) {
        return;
    }
    const Vec3 world = Quat::yaw(g_yawOffset).rotate(offset) * units_per_meter();
    link->current.pos.x += world.x;
    link->current.pos.z += world.z;
    g_body.x = head.x;
    g_body.z = head.z;
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
    const daPy_py_c* player = link;
    g_locked = player->checkAttentionLock() || player->checkEnemyAttentionLock();
    const float targetEye = link->eyePos.y - link->current.pos.y;
    const bool wolf = link->checkWolf();
    if (!g_linkValid || wolf != g_wasWolf) {
        g_linkPrev = g_linkCurr = link->current.pos;
        g_eyeHeightPrev = g_eyeHeightCurr = targetEye;
    } else {
        g_linkPrev = g_linkCurr;
        g_linkCurr = link->current.pos;
        g_eyeHeightPrev = g_eyeHeightCurr;
        // Smooth out animation bob in the eye height.
        g_eyeHeightCurr += (targetEye - g_eyeHeightCurr) * 0.3f;
    }
    // Teleports (loading zones, warps) shouldn't be interpolated across.
    if (g_linkPrev.abs(g_linkCurr) > 300.f) {
        g_linkPrev = g_linkCurr;
    }
    g_wasWolf = wolf;
    g_linkValid = true;
}

bool apply_eye_view(view_class* view, float step) {
    if (!g_active || g_currentEye < 0 || view == nullptr) {
        return false;
    }
    const xr::EyeView& eye = xr::eye(g_currentEye);
    const float scale = units_per_meter();

    Vec3 eyePos;
    Quat eyeRot;
    if (g_firstPersonFrame) {
        cXyz feet;
        feet.x = g_linkPrev.x + (g_linkCurr.x - g_linkPrev.x) * step;
        feet.y = g_linkPrev.y + (g_linkCurr.y - g_linkPrev.y) * step;
        feet.z = g_linkPrev.z + (g_linkCurr.z - g_linkPrev.z) * step;
        const float eyeHeight = g_eyeHeightPrev + (g_eyeHeightCurr - g_eyeHeightPrev) * step;
        eyePos = tracking_to_world_fp(eye.pose.position, first_person_anchor(feet, eyeHeight));
        eyePos.y = std::max(eyePos.y, feet.y + 20.f);
        eyeRot = Quat::yaw(g_yawOffset) * eye.pose.orientation;
    } else {
        // Third person and cutscenes: head tracking on top of the game camera, horizon level.
        const float base = game_camera_yaw(view) - g_headYawAtRecenter;
        const Vec3 local = eye.pose.position - g_body;
        eyePos = to_vec(view->lookat.eye) + Quat::yaw(base).rotate(local) * scale;
        eyeRot = Quat::yaw(base) * eye.pose.orientation;
    }

    if (g_firstPersonFrame && getSettings().vr.motionControls) {
        if (daAlink_c* link = get_link(); link != nullptr && !link->checkWolf()) {
            latch_hands(link, step);
        }
    }

    build_view_matrix(view->viewMtx, eyeRot, eyePos);
    const Vec3 fwd = eyeRot.rotate({0.f, 0.f, -1.f});
    const Vec3 up = eyeRot.rotate({0.f, 1.f, 0.f});
    view->lookat.eye = to_cxyz(eyePos);
    view->lookat.center = to_cxyz(eyePos + fwd * 100.f);
    view->lookat.up = to_cxyz(up);
    view->bank = 0;

    const float n = std::max(view->near_, 1.f);
    const float f = view->far_;
    const float tl = std::tan(eye.angleLeft), tr = std::tan(eye.angleRight);
    const float tu = std::tan(eye.angleUp), td = std::tan(eye.angleDown);
    C_MTXFrustum(view->projMtx, tu * n, td * n, tl * n, tr * n, n, f);

    // The clipper only understands symmetric frusta; cover the whole asymmetric one.
    const float halfV = std::max(std::fabs(tu), std::fabs(td));
    const float halfH = std::max(std::fabs(tl), std::fabs(tr));
    view->fovy = 2.f * std::atan(halfV) * (180.f / kPi);
    view->aspect = halfH / halfV;
    view->near_ = n;

    j3dSys.setViewMtx(view->viewMtx);
    cMtx_inverse(view->viewMtx, view->invViewMtx);
    MTXCopy(view->viewMtx, view->viewMtxNoTrans);
    view->viewMtxNoTrans[0][3] = 0.0f;
    view->viewMtxNoTrans[1][3] = 0.0f;
    view->viewMtxNoTrans[2][3] = 0.0f;
    cMtx_concatProjView(view->projMtx, view->viewMtx, view->projViewMtx);
    mDoLib_clipper::setup(view->fovy + 10.f, view->aspect, n, f);

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

bool hide_link_body() { return first_person(); }

void override_link_hands(daAlink_c* link) {
    if (!first_person() || !getSettings().vr.motionControls || link->checkWolf() ||
        link->mpLinkModel == nullptr || !g_linkValid)
    {
        return;
    }
    const auto& in = xr::input();
    const Vec3 anchor = first_person_anchor(link->current.pos, g_eyeHeightCurr);

    // The controller holds the item joint (where the game attaches the sword, shield or tool);
    // the hand follows it rigidly.
    const auto place = [&](xr::Hand hand, int handJoint, int itemJoint) {
        const auto& state = in.hands[hand];
        g_placements[hand].valid = false;
        if (!state.active) {
            return;
        }
        Mtx& target = g_placements[hand].simTarget;
        build_world_matrix(target, Quat::yaw(g_yawOffset) * state.grip.orientation * item_offset(hand),
            tracking_to_world_fp(state.grip.position, anchor));

        MtxP item = link->mpLinkModel->getAnmMtx(itemJoint);
        MtxP handMtx = link->mpLinkModel->getAnmMtx(handJoint);
        Mtx inv, delta, moved;
        MTXInverse(item, inv);
        MTXConcat(target, inv, delta);
        MTXConcat(delta, handMtx, moved);
        MTXCopy(moved, handMtx);
        MTXCopy(target, item);
        g_placements[hand].valid = true;
    };
    place(input::sword_hand(), kSwordHandJoint, kSwordItemJoint);
    place(input::shield_hand(), kShieldHandJoint, kShieldItemJoint);
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
    cXyz feet;
    feet.x = g_linkPrev.x + (g_linkCurr.x - g_linkPrev.x) * step;
    feet.y = g_linkPrev.y + (g_linkCurr.y - g_linkPrev.y) * step;
    feet.z = g_linkPrev.z + (g_linkCurr.z - g_linkPrev.z) * step;
    const float eyeHeight = g_eyeHeightPrev + (g_eyeHeightCurr - g_eyeHeightPrev) * step;
    const Vec3 anchor = first_person_anchor(feet, eyeHeight);

    const auto latch = [&](xr::Hand hand, int handJoint, int itemJoint, int handModelJoint) {
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
    latch(input::sword_hand(), kSwordHandJoint, kSwordItemJoint, kSwordHandModelJoint);
    latch(input::shield_hand(), kShieldHandJoint, kShieldItemJoint, kShieldHandModelJoint);
}

}  // namespace

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
    combo("Desktop window shows", vr.mirror, "Right eye\0HUD panel\0");
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
