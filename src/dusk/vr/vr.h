#pragma once

#include <dolphin/types.h>

class daAlink_c;
struct view_class;

// Stereo 6DoF VR on top of the frame-interpolation presentation path.
//
// Each headset frame runs the simulation as usual, then presents the 3D scene once per eye. The
// per-eye camera replaces the interpolated game camera inside dusk::interp's presentation. The 2D
// HUD is drawn separately after each simulation tick and shown on a floating panel.
namespace dusk::vr {

// What the current aurora frame is drawing.
enum class Pass {
    Flat,      // not in VR
    Eye,       // 3D scene for one eye, no HUD
    HudBlack,  // HUD only, over black
    HudWhite,  // HUD only, over white
    Spectator, // 16:9 view for the desktop window (streaming)
};

#if DUSK_VR
// Creates the OpenXR instance/session if VR is enabled and a headset is present. Call once after
// the graphics device exists.
void initialize();
void shutdown();

// True while frames are being submitted to the headset.
bool active();

// Main loop. When begin_frame() returns true, the caller renders eye_count() presentations,
// bracketing each with begin_eye()/aurora frame, then calls end_frame().
bool begin_frame();
int eye_count();
void begin_eye(int eye);
// The HUD is redrawn after simulation ticks: two extra frames, over black then over white.
bool hud_pending();
void begin_hud(int phase);
// The desktop window gets its own 16:9 camera (smoothed first person, third person or selfie),
// drawn every few headset frames.
bool spectator_pending();
void begin_spectator();
// Whether the spectator view should include the 2D HUD.
bool spectator_hud();
void end_frame();
Pass render_pass();
// The background the frame after this one needs (HUD passes), as a gray level.
bool next_frame_clear(u8& level);

// Simulation tick hooks.
void before_sim_tick();  // pushes controller state into the virtual pad
void after_sim_tick();   // snapshots the player for interpolated presentation

// Presentation hook: overrides the view with the current eye. Returns true if it did.
bool apply_eye_view(view_class* view, float step);

// Gameplay hooks.
s16 control_angle_y(s16 gameCameraAngle);
bool first_person();
bool hide_link_body();
bool hide_link_head();
void override_link_hands(daAlink_c* link);
// Picks Link's hand shapes (indices into his hand model: left 0-4, right 5-10).
void override_hand_shapes(daAlink_c* link, u8& left, u8& right);
bool hand_aim(s16& yaw, s16& pitch);

// Debug/settings UI.
void draw_imgui_window(bool* open);
void recenter();
#else
inline void initialize() {}
inline void shutdown() {}
inline bool active() { return false; }
inline bool begin_frame() { return false; }
inline int eye_count() { return 0; }
inline void begin_eye(int) {}
inline bool hud_pending() { return false; }
inline void begin_hud(int) {}
inline bool spectator_pending() { return false; }
inline void begin_spectator() {}
inline bool spectator_hud() { return true; }
inline void end_frame() {}
inline Pass render_pass() { return Pass::Flat; }
inline bool next_frame_clear(u8&) { return false; }
inline void before_sim_tick() {}
inline void after_sim_tick() {}
inline bool apply_eye_view(view_class*, float) { return false; }
inline s16 control_angle_y(s16 a) { return a; }
inline bool first_person() { return false; }
inline bool hide_link_body() { return false; }
inline bool hide_link_head() { return false; }
inline void override_link_hands(daAlink_c*) {}
inline void override_hand_shapes(daAlink_c*, u8&, u8&) {}
inline bool hand_aim(s16&, s16&) { return false; }
inline void draw_imgui_window(bool*) {}
inline void recenter() {}
#endif

}  // namespace dusk::vr
