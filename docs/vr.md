# Dusklight VR

Full 6DoF OpenXR support, built into the engine: the game renders each eye itself, first person
inside Link's full body (arms reaching for your controllers, legs walking), Wolf Link on all fours,
a third-person toggle, the HUD and menus on a floating panel you point at, and motion controls for
the sword, shield, bow, climbing, swimming and riding.

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
| Bow / slingshot out: grab the string near the bow with the sword hand (grip or trigger), pull back, let go | Draw and shoot; the arrow flies from the string through the bow |
| Point the sword-hand controller while aiming | Aim clawshot, boomerang, other aimed items |
| In menus: point at the panel, pull the sword-hand trigger | Click (laser pointer) |
| Sword-hand trigger | B (sword, cancel) |
| Sword-hand A / B | A (action) / Z (Midna) |
| Off-hand X / Y | X / Y items |
| Off-hand trigger / grip | L (lock on) / R |
| Menu (tap) | Start (pause / collection) |
| Menu (hold) | Dusklight's own menu |

| Wolf: swipe a controller / lunge your head forward | Claw / bite (B) |
| Wolf: both hands low, scrabbling | Dig (A) |
| Ladder, vines, ledge, ceiling: hold grip and pull | Climb (pull down to go up, sideways to shuffle) |
| Swimming: sweep both arms back | Swim forward |
| On Epona: flick both hands down | Spur (A) |
| Carrying a pot, rock or bomb: toss it forward | Throw (A) |
| Curl your fingers (Index finger tracking, or trigger/grip) | Link's hand opens, hooks, makes a fist or pinches |

Room scale is on by default: walking in your room moves Link, and walls stop you. Standing still,
Link's body turns to face where you look once you look far enough away, pivoting around his eyes so
the view never swings.

The controllers vibrate when the sword connects, when you get hurt, while drawing the bow (rising
tension) and when you raise the shield.

## Settings

Tools > VR in the Dusklight menu bar (desktop window) has every option: perspective, world scale
(game units per meter, 100 by default), height offset, HUD panel distance / width / height,
turning, hand-oriented movement, room scale, physical crouch, motion controls, swing speed,
sword and shield angle offsets, what the desktop window mirrors, resolution scale and recenter.
Any of them can also be set for one run with `--cvar`, e.g. `--cvar vr.desktopView=2`.

The comfort vignette (off by default) darkens the edges of your view while the stick, smooth
turning, climbing, swimming or riding moves you.

### Desktop window and streaming

The desktop window is a proper 16:9 picture, chosen in Tools > VR ("Desktop window shows"):

- **Headset view** (default): what your right eye sees, as a 16:9 band. Free: nothing is drawn twice.
- **Smooth first person**: a separate 16:9 camera at your eyes that follows your head's full rotation
  with adjustable smoothing (0 = exact), optionally keeping the horizon level.
- **Third person**: the game's own follow camera (it avoids walls), showing your whole posed body,
  head included, the way LIV shows an avatar.
- **Selfie camera**: in front of you, looking back at Link.
- **Raw right eye** and **HUD panel**, for debugging.

The separate cameras can include the HUD, have their own field of view, and are drawn every Nth
headset frame (2 by default). On an RTX-class GPU with the Index at 144 Hz they cost a few frames
per second.

### Body

In first person you are inside Link's body: looking down you see your chest, arms and legs, and his
shadow moves with you. Only the head and neck are left out of your own view. Each controller drives
the arm on its side through two-bone IK (elbows hang down and out like a relaxed arm), the skin at
the elbows and shoulders follows, and the legs keep the game's walking and running animations.
Turn the body off ("Full body" in Tools > VR) for floating hands only.

Your head drives Link's: the head takes the headset's position and orientation, and the spine and
neck bend and twist to reach it, so leaning or looking around moves his chest and shoulders (and
the arms reaching from them). His feet stay planted while you lean or look around within 30 cm;
step further and he takes a step to stay under you.

Two toggles keep the body from fighting you (both on by default):

- **No idle animations**: standing still, Link holds a neutral stance (feet planted, arms at his
  sides) instead of fidgeting, stretching or looking around.
- **Walking animates the legs only**: moving, the spine, head and arms stay steady (riding the hips'
  height) and only the legs (and the tunic's skirt) run the walk and run animations. The camera no
  longer picks up the animation's head bob.

The neutral stance is the model's own rest pose with the arms lowered; it is worked out from the
skeleton, so it applies to Wolf Link as well (his legs keep moving, his back and head stay level).

Link holds the sword in your sword hand: in VR the game's sword and shield attachment points follow
the "Left handed" setting.

As Wolf Link the camera sits at the wolf's eyes and you see his body and paws below you (and Midna
on your back, when she rides).

### Hands

His hands, and whatever they hold, follow your controllers. Link's model has no finger bones; it
has a set of hand shapes, and an empty hand picks one from your fingers: open, a climbing hook, a
fist, or a thumb-and-index pinch. With Index controllers the curl of each finger comes from
SteamVR's finger tracking; on other controllers the trigger is the index finger and the grip the
rest. A hand holding the sword or an item makes a fist; the shield hand grips the shield. The controller holds the item joint the game attaches weapons to, oriented so the
blade points along the controller like a held sword. Everything held is moved to the controller's
pose again at draw time, so hands track at the headset's refresh rate rather than the game's 30 Hz.
If a weapon sits at an odd angle in your grip, the sword / shield pitch, yaw and roll offsets in
Tools > VR adjust it.

### Dusklight's menus

The launcher, settings, mod manager and other Dusklight windows appear on the floating panel too,
live every frame. Point and pull the sword-hand trigger to click, or use either stick to move,
A to confirm, B to go back, the grips to switch tabs and Menu to close.

### HUD panel

Hearts, buttons, map, text boxes and menus are drawn on a 16:9 panel 1.5 m in front of you. It
turns lazily to follow your gaze once you look more than 35 degrees away and holds still while the
game is paused. In menus the sword-hand controller becomes a laser pointer with a dot on the panel;
the trigger clicks.

### Items

Every item attaches through the game's own "left item" and "right item" hand joints, which follow
your controllers, so held items sit in your hands:

| Item | In VR |
| --- | --- |
| Sword | In your sword hand; swing, thrust, spin |
| Shield | In your off hand; raise to guard, punch to bash |
| Bow, slingshot | In your off hand; pull the string with the sword hand |
| Clawshot (single and double), Gale Boomerang, Dominion Rod | Aim by pointing; fire with the item button |
| Lantern | In your sword hand while in use, on the belt otherwise |
| Bottles, bombs | Held in hand; carried bombs and pots are thrown with a toss |
| Ball and Chain | Handle in your right hand, chain in both |
| Hawkeye | Worn on the head and hidden from your own view; the zoom is not applied in the headset |
| Iron Boots, Spinner, fishing rod | Work with their buttons as usual |
| Epona's reins | Follow your hands; flick both hands to spur |

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
- The painter (`mDoGph_Painter`) skips the 2D in eye passes. After each simulation tick two more
  frames draw only the 2D, over black and over white; the HUD's alpha is recovered from the
  difference (`1 - (white - black)`) and the result goes to an OpenXR quad layer.
- Held items are late-latched through `dusk::interp::set_presentation_override`: at draw time each
  matrix of the hand, sword, shield or held item is moved from the controller pose of the last sim
  tick to the current one.
- Aurora hooks (`patches/aurora-vr.patch`): a per-eye framebuffer size override, a per-frame hook
  that sees the finished frame before the window present, skipping the window present, and the
  D3D12 shared-texture device features.

## Known limitations

- Cutscenes and scripted events use the game camera with head tracking on top.
- Sword hits follow the attack animation's timing: a swing triggers the attack, and the blade's
  hitbox (which follows your controller) is live while that attack runs.
- 2D markers that point at things in the world (lock-on arrows) sit on the HUD panel where the flat
  game would draw them, not over the target.
- The arms don't twist the forearm to follow wrist roll; extreme wrist angles can look stiff.
- Wolf Link, climbing, swimming and riding gestures were written against the game's code but not yet
  playtested.
- The Hawkeye's zoom isn't applied in the headset (changing your field of view in VR is
  uncomfortable); its scope overlay shows on the HUD panel.
- Only tested on SteamVR with a Valve Index; other runtimes should work through OpenXR.
