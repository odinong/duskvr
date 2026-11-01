# Dusklight VR

Full 6DoF OpenXR support, built into the engine: the game renders each eye itself, first person at
Link's eyes with a third-person toggle, and motion controls for the sword, shield and aiming.

## Requirements

- Windows, an OpenXR runtime with D3D12 support (SteamVR, Meta Quest Link, Virtual Desktop, WMR).
- While VR is enabled the game uses the D3D12 backend, whatever the saved backend setting is (pass
  `--backend` to override).
- The headset should be on and the runtime started before launching. Without a headset the game
  runs flat as usual.

## Building

This branch also needs a few hooks in Aurora (`extern/aurora`). Apply them before configuring:

```sh
git -C extern/aurora apply ../../patches/aurora-vr.patch
cmake --preset windows-msvc-relwithdebinfo
cmake --build --preset windows-msvc-relwithdebinfo
```

CMake fetches the OpenXR loader (OpenXR-SDK release-1.1.62). Turn VR off with `-DDUSK_ENABLE_VR=OFF`.

## Controls

The sword hand is the right controller by default (Settings: left handed).

| Input | Action |
| --- | --- |
| Off-hand stick | Move (toward where you look) |
| Sword-hand stick | Snap/smooth turn (first person) or camera (third person) |
| Sword-hand stick click | Tap: switch first/third person. Hold 1 s: recenter |
| Off-hand stick click | Map |
| Swing the sword-hand controller | Sword attack (thrust forward while locked on to stab) |
| Full circle swing around you | Spin attack |
| Raise the off hand in front of your chest | Lock on and guard with the shield (L) |
| Punch forward with the shield while locked on | Shield bash |
| Crouch for real | Crouch (R) |
| Point the sword-hand controller while aiming | Aim bow, slingshot, clawshot, boomerang... |
| Sword-hand trigger | B (sword, cancel) |
| Sword-hand A / B | A (action) / Z (Midna) |
| Off-hand X / Y | X / Y items |
| Off-hand trigger / grip | L (lock on) / R |
| Menu | Start |

Room scale is on by default: walking in your room moves Link, and walls stop you.

## Settings

Tools > VR in the Dusklight menu bar (desktop window) has every option: perspective, world scale
(game units per meter, 100 by default), height offset, turning, hand-oriented movement, room scale,
physical crouch, motion controls, swing speed, resolution scale and recenter.

### Hand calibration

In first person Link's body and head are hidden and his hands (with the sword and shield) follow
your controllers. Link's hand bones are not oriented like a controller grip, so the sword may point
the wrong way until calibrated: adjust the hand pitch / yaw / roll offsets in Tools > VR while
holding the controller like a sword, until the blade lines up with the controller.

## How it works

- `src/dusk/vr/xr_session.cpp`: OpenXR instance and D3D12 session sharing Dawn's device and queue,
  per-eye swapchains, controller actions and haptics. Runtime swapchain images can't be imported
  into Dawn (no simultaneous-access flag), so each eye renders into a shareable staging texture
  that is copied into the swapchain image on the same queue.
- `src/dusk/vr/vr.cpp`: play-space mapping (yaw, recenter, room scale), the per-eye view and
  asymmetric projection, and the Link hooks (hidden body, controller-driven hands, head-relative
  movement, hand aiming).
- `src/dusk/vr/vr_input.cpp`: controller-to-GameCube-pad mapping and gesture detection, fed
  through the virtual pad (`PADSetVirtualStatus`).
- Each headset frame simulates once, then runs the frame-interpolation presentation once per eye;
  the eye camera replaces the interpolated game camera inside that presentation. Frame
  interpolation is forced on while VR is active.
- Aurora hooks (`patches/aurora-vr.patch`): a per-eye framebuffer size override, a per-frame hook
  that sees the finished frame before the window present, skipping the window present, and the
  D3D12 shared-texture device features.

## Known limitations

- The HUD and menus are drawn into each eye at a fixed spot in your view (no floating panel yet).
- Cutscenes and scripted events use the game camera with head tracking on top.
- The hands follow the controllers at the 30 Hz simulation rate (interpolated), so they lag
  slightly behind the controllers.
- Sword hits follow the attack animation's timing; a swing triggers the attack but the blade's
  hitbox only exists while that animation runs.
- Wolf Link in first person has no visible paws.
