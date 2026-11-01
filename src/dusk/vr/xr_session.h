#pragma once

#include "dusk/vr/vr_math.h"

#include <cstdint>

// Thin OpenXR wrapper: instance, D3D12 session, per-eye swapchains imported into WebGPU, and
// controller actions.
namespace dusk::vr::xr {

enum Hand { kLeft = 0, kRight = 1 };

struct EyeView {
    Pose pose;  // tracking space
    float angleLeft = 0.f, angleRight = 0.f, angleUp = 0.f, angleDown = 0.f;
};

struct HandState {
    bool active = false;
    Pose grip;
    Pose aim;
    Vec3 velocity;  // grip linear velocity, tracking space, m/s
    bool velocityValid = false;
};

struct InputState {
    HandState hands[2];
    float stickX[2]{}, stickY[2]{};
    bool stickClick[2]{};
    float trigger[2]{}, squeeze[2]{};
    bool buttonLower[2]{};  // A (right) / X (left)
    bool buttonUpper[2]{};  // B (right) / Y (left)
    bool menu = false;
};

bool initialize();
void shutdown();

// Polls OpenXR events. Returns false once the session is gone for good (exit requested).
bool poll_events();
bool session_running();

// xrWaitFrame + xrBeginFrame + view/action location. Returns false if no frame should be drawn.
bool begin_frame();
bool should_render();
// Tracking is valid this frame (it may still be hidden, e.g. headset not worn).
bool views_valid();
const EyeView& eye(int index);
const Pose& head();
bool head_valid();
uint32_t eye_width();
uint32_t eye_height();
bool floor_relative();

// Queues the blit of the next aurora frame's output into this eye's swapchain image.
void render_eye(int index);
// HUD panel: phase 0 keeps the next aurora frame (HUD over black), phase 1 composes it with the
// next frame (HUD over white) into the panel's swapchain.
void render_hud(int phase);
void set_hud_pose(const Pose& pose, float widthMeters, float heightMeters);
// Menu pointer dot, drawn as its own small quad so it moves at the headset's rate.
void set_cursor(bool visible, const Pose& pose, float sizeMeters);
// Waits for submission, releases images and ends the OpenXR frame.
void end_frame();

const InputState& input();
void haptic(Hand hand, float amplitude, float seconds);
double predicted_display_seconds();

}  // namespace dusk::vr::xr
