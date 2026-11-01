#pragma once

#include <dolphin/types.h>

class daAlink_c;
struct view_class;

// Stereo 6DoF VR on top of the frame-interpolation presentation path.
//
// Each headset frame runs the simulation as usual, then presents the scene once per eye. The
// per-eye camera replaces the interpolated game camera inside dusk::interp's presentation, so
// everything the game draws during presentation (world, particles, HUD) lands in that eye.
namespace dusk::vr {

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
void end_frame();

// Simulation tick hooks.
void before_sim_tick();  // pushes controller state into the virtual pad
void after_sim_tick();   // snapshots the player for interpolated presentation

// Presentation hook: overrides the view with the current eye. Returns true if it did.
bool apply_eye_view(view_class* view, float step);

// Gameplay hooks.
s16 control_angle_y(s16 gameCameraAngle);
bool first_person();
bool hide_link_body();
void override_link_hands(daAlink_c* link);
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
inline void end_frame() {}
inline void before_sim_tick() {}
inline void after_sim_tick() {}
inline bool apply_eye_view(view_class*, float) { return false; }
inline s16 control_angle_y(s16 a) { return a; }
inline bool first_person() { return false; }
inline bool hide_link_body() { return false; }
inline void override_link_hands(daAlink_c*) {}
inline bool hand_aim(s16&, s16&) { return false; }
inline void draw_imgui_window(bool*) {}
inline void recenter() {}
#endif

}  // namespace dusk::vr
