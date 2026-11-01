#pragma once

#include <array>

#include "dusk/config_var.hpp"
#include "dusk/ui/controls.hpp"

namespace dusk {

using config::ConfigVar;
using config::ActionBindConfigVar;

enum class BloomMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class DepthOfFieldMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class Resampler : int {
    Bilinear = 0,
    Area = 1,
};

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
    Japanese = 6,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

enum class FrameInterpMode : u8 {
    Off = 0,
    Capped = 1,
    Unlimited = 2,
};

enum class LetterboxMode : u8 {
    Off = 0,
    On = 1,
    GameplayOnly = 2,
    CutsceneOnly = 3,
};

enum class TouchTargeting : u8 {
    Hybrid = 0,
    Hold = 1,
    Switch = 2,
};

enum class MenuScaling : u8 {
    GameCube = 0,
    Wii = 1,
    Dusklight = 2,
};

enum class AlwaysGreatspinMode : u8 {
    OFF = 0,
    AFTER_SKILL = 1,
    ALWAYS = 2,
};

enum class MagicArmorMode : u8 {
    NORMAL = 0,
    ON_DAMAGE = 1,
    DOUBLE_DEFENSE = 2,
    INVINCIBLE = 3,
    COSMETIC = 4,
};

enum class AudioOutputMode : u8 {
    StereoSpeakers = 0,
    StereoHeadphones = 1,   // spatial audio
    Surround6ch = 2,        // discrete 5.1
    Surround8ch = 3,        // discrete 7.1
};

namespace config {
template <>
struct ConfigEnumRange<BloomMode> {
    static constexpr auto min = BloomMode::Off;
    static constexpr auto max = BloomMode::Dusk;
};

template <>
struct ConfigEnumRange<DepthOfFieldMode> {
    static constexpr auto min = DepthOfFieldMode::Off;
    static constexpr auto max = DepthOfFieldMode::Dusk;
};

template <>
struct ConfigEnumRange<Resampler> {
    static constexpr auto min = Resampler::Bilinear;
    static constexpr auto max = Resampler::Area;
};

template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Japanese;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

template <>
struct ConfigEnumRange<FrameInterpMode> {
    static constexpr auto min = FrameInterpMode::Off;
    static constexpr auto max = FrameInterpMode::Unlimited;
};

template <>
struct ConfigEnumRange<LetterboxMode> {
    static constexpr auto min = LetterboxMode::Off;
    static constexpr auto max = LetterboxMode::CutsceneOnly;
};

template <>
struct ConfigEnumRange<TouchTargeting> {
    static constexpr auto min = TouchTargeting::Hybrid;
    static constexpr auto max = TouchTargeting::Switch;
};

template <>
struct ConfigEnumRange<MenuScaling> {
    static constexpr auto min = MenuScaling::GameCube;
    static constexpr auto max = MenuScaling::Dusklight;
};

template <>
struct ConfigEnumRange<AlwaysGreatspinMode> {
    static constexpr auto min = AlwaysGreatspinMode::OFF;
    static constexpr auto max = AlwaysGreatspinMode::ALWAYS;
};

template <>
struct ConfigEnumRange<MagicArmorMode> {
    static constexpr auto min = MagicArmorMode::NORMAL;
    static constexpr auto max = MagicArmorMode::COSMETIC;
};

template <>
struct ConfigEnumRange<AudioOutputMode> {
    static constexpr auto min = AudioOutputMode::StereoSpeakers;
    static constexpr auto max = AudioOutputMode::Surround8ch;
};

template <>
struct ConfigValueTraits<ui::ControlLayout> {
    static constexpr bool enabled = true;
};
}  // namespace config

// Persistent user settings

struct UserSettings {
    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        ConfigVar<int> maxFrameRate;
        ConfigVar<bool> rememberWindowSize;
        ConfigVar<int> lastWindowWidth;
        ConfigVar<int> lastWindowHeight;
        ConfigVar<int> uiScale;
    } video;

    struct {
        // Audio
        ConfigVar<AudioOutputMode> outputMode;
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;
        ConfigVar<bool> hideTvSettingsScreen;
        ConfigVar<bool> biggerWallets;
        ConfigVar<bool> noReturnRupees;
        ConfigVar<bool> disableRupeeCutscenes;
        ConfigVar<bool> fastTransitions;
        ConfigVar<bool> noSwordRecoil;
        ConfigVar<int> damageMultiplier;
        ConfigVar<bool> noHeartDrops;
        ConfigVar<bool> instantDeath;
        ConfigVar<bool> fastClimbing;
        ConfigVar<bool> noMissClimbing;
        ConfigVar<bool> fastTears;
        ConfigVar<bool> no2ndFishForCat;
        ConfigVar<bool> buttonFishing;
        ConfigVar<bool> instantSaves;
        ConfigVar<bool> instantText;
        ConfigVar<bool> holdToMash;
        ConfigVar<bool> sunsSong;
        ConfigVar<bool> autoSave;
        ConfigVar<bool> enhancedMapMenus;
        ConfigVar<bool> aimingReticle;
        ConfigVar<bool> noHitStun;

        // Preferences
        ConfigVar<bool> enableMirrorMode;
        ConfigVar<bool> minimalHUD;
        ConfigVar<float> hudScale;
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableLinkDollRotation;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;
        ConfigVar<bool> enableDiscordPresence;
        ConfigVar<MenuScaling> menuScalingMode;

        // Graphics
        ConfigVar<BloomMode> bloomMode;
        ConfigVar<float> bloomMultiplier;
        ConfigVar<DepthOfFieldMode> depthOfFieldMode;
        ConfigVar<bool> disableWaterRefraction;
        ConfigVar<bool> enableTextureReplacements;
        ConfigVar<FrameInterpMode> enableFrameInterpolation;
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<Resampler> resampler;
        ConfigVar<bool> enableMapBackground;
        ConfigVar<bool> disableCutscenePillarboxing;
        ConfigVar<LetterboxMode> disableLetterboxing;
        ConfigVar<bool> enableHighQualityMinimapTextures;

        // Audio
        ConfigVar<bool> noLowHpSound;
        ConfigVar<bool> midnasLamentNonStop;

        // Input
        ConfigVar<bool> enableGyroAim;
        ConfigVar<bool> enableGyroRollgoal;
        ConfigVar<float> gyroSensitivityX;
        ConfigVar<float> gyroSensitivityY;
        ConfigVar<float> gyroSensitivityRollgoal;
        ConfigVar<float> gyroSmoothing;
        ConfigVar<float> gyroDeadband;
        ConfigVar<bool> gyroInvertPitch;
        ConfigVar<bool> gyroInvertYaw;
        ConfigVar<bool> enableMouseCamera;
        ConfigVar<bool> enableMouseAim;
        ConfigVar<float> mouseAimSensitivity;
        ConfigVar<float> mouseCameraSensitivity;
        ConfigVar<bool> invertMouseY;
        ConfigVar<bool> freeCamera;
        ConfigVar<bool> enableTouchControls;
        ConfigVar<TouchTargeting> touchTargeting;
        ConfigVar<bool> enableMenuPointer;
        ConfigVar<ui::ControlLayout> touchControlsLayout;
        ConfigVar<bool> invertCameraXAxis;
        ConfigVar<bool> invertCameraYAxis;
        ConfigVar<bool> invertFirstPersonXAxis;
        ConfigVar<bool> invertFirstPersonYAxis;
        ConfigVar<bool> invertAirSwimX;
        ConfigVar<bool> invertAirSwimY;
        ConfigVar<float> freeCameraXSensitivity;
        ConfigVar<float> freeCameraYSensitivity;
        ConfigVar<float> touchCameraXSensitivity;
        ConfigVar<float> touchCameraYSensitivity;
        ConfigVar<bool> debugFlyCam;
        ConfigVar<bool> debugFlyCamLockEvents;
        ConfigVar<bool> allowBackgroundInput;
        std::array<ConfigVar<bool>, 4> enableLED;
        ConfigVar<bool> swapDirectSelect;

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> infiniteArrows;
        ConfigVar<bool> infiniteSeeds;
        ConfigVar<bool> infiniteBombs;
        ConfigVar<bool> infiniteOil;
        ConfigVar<bool> infiniteOxygen;
        ConfigVar<bool> infiniteRupees;
        ConfigVar<bool> enableIndefiniteItemDrops;
        ConfigVar<bool> moonJump;
        ConfigVar<bool> superClawshot;
        ConfigVar<AlwaysGreatspinMode> alwaysGreatspin;
        ConfigVar<bool> enableFastIronBoots;
        ConfigVar<bool> canTransformAnywhere;
        ConfigVar<bool> fastRoll;
        ConfigVar<bool> fastSpinner;
        ConfigVar<MagicArmorMode> armorRupeeDrain;
        ConfigVar<bool> invincibleEnemies;
        ConfigVar<bool> easyQuickSpin;
        ConfigVar<bool> infiniteEponaStamina;

        // Technical
        ConfigVar<bool> restoreWiiGlitches;

        // Controls
        ConfigVar<bool> enableTurboKeybind;
        ConfigVar<bool> enableResetKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> liveSplitEnabled;
        ConfigVar<bool> showSpeedrunRTATimer;
        ConfigVar<bool> recordingMode;
        ConfigVar<bool> removeQuestMapMarkers;
        ConfigVar<bool> showInputViewer;
        ConfigVar<bool> showInputViewerGyro;
        ConfigVar<bool> enableMoveLinkCombo;
        ConfigVar<bool> enableTeleportCombo;

        ConfigVar<std::string> lastSelectedGameModeId;
    } game;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> showPipelineCompilation;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<bool> checkForModUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
    } backend;

    struct {
        // VR (OpenXR)
        ConfigVar<bool> enabled;
        ConfigVar<int> viewMode;          // 0 = first person, 1 = third person
        ConfigVar<float> worldScale;      // game units per real-world meter
        ConfigVar<float> heightOffset;    // extra eye height, in game units
        ConfigVar<int> turnMode;          // 0 = snap, 1 = smooth, 2 = none
        ConfigVar<int> snapTurnDegrees;
        ConfigVar<float> smoothTurnSpeed; // degrees per second
        ConfigVar<bool> handOrientedMovement;
        ConfigVar<bool> leftHanded;       // sword in the left controller
        ConfigVar<bool> motionControls;
        ConfigVar<float> swingSpeed;      // m/s of the sword tip needed to swing
        ConfigVar<bool> physicalCrouch;
        ConfigVar<bool> roomScale;
        ConfigVar<bool> levelHorizon;     // third person / cutscenes: drop camera pitch and roll
        ConfigVar<float> resolutionScale;
        ConfigVar<float> handOffsetPitch; // degrees, extra rotation of the sword in the hand
        ConfigVar<float> handOffsetYaw;
        ConfigVar<float> handOffsetRoll;
        ConfigVar<float> shieldOffsetPitch; // degrees, extra rotation of the shield
        ConfigVar<float> shieldOffsetYaw;
        ConfigVar<float> shieldOffsetRoll;
        ConfigVar<float> hudDistance;     // meters
        ConfigVar<float> hudWidth;        // meters
        ConfigVar<float> hudHeight;       // meters, relative to the eyes
        ConfigVar<int> desktopView;       // 0 headset view (16:9), 1 smooth first person,
                                          // 2 third person, 3 selfie, 4 raw right eye, 5 HUD
        ConfigVar<bool> spectatorHud;     // draw the game's HUD on the spectator view
        ConfigVar<float> spectatorFov;    // degrees, vertical
        ConfigVar<int> spectatorRate;     // render the spectator view every Nth headset frame
        ConfigVar<float> spectatorSmoothing; // 0 = follows the headset exactly, 1 = very smooth
        ConfigVar<bool> spectatorLevel;   // keep the first-person desktop view's horizon level
        ConfigVar<bool> fullBody;         // first person shows Link's body, arms reaching the controllers
        ConfigVar<bool> stillIdle;        // first person: no idle animations, Link stands still
        ConfigVar<bool> legsOnlyWalk;     // first person: walking animates the legs only
        ConfigVar<bool> comfortVignette;  // darken the edges while moving with the stick
    } vr;

    // Arrays of size 4 for 4 ports
    struct {
        std::array<ActionBindConfigVar, 4> firstPersonCamera;
        std::array<ActionBindConfigVar, 4> callMidna;
        std::array<ActionBindConfigVar, 4> openMapScreen;
        std::array<ActionBindConfigVar, 4> toggleMinimap;
        std::array<ActionBindConfigVar, 4> openDusklightMenu;
        std::array<ActionBindConfigVar, 4> turboSpeedButton;
    } actionBindings;
};

UserSettings& getSettings();

void registerSettings();

void applyInternalResolutionScale(int scale);
void applyResampler(Resampler resampler);

inline bool isLetterboxingDisabled(bool inCutscene) {
    const auto mode = getSettings().game.disableLetterboxing.getValue();
    return mode == LetterboxMode::On ||
           (mode == LetterboxMode::CutsceneOnly && inCutscene) ||
           (mode == LetterboxMode::GameplayOnly && !inCutscene);
}

// Transient settings

struct CollisionViewSettings {
    bool enableTerrainView;
    bool enableWireframe;
    bool enableAtView;
    bool enableTgView;
    bool enableCoView;
    float terrainViewOpacity;
    float colliderViewOpacity;
    float drawRange;
};

struct TriggerViewSettings {
    bool loadZones;
    bool eventAreas;
    bool switchAreas;
    bool eventTags;
    bool midnaStops;
    bool twilightGates;
    bool checkpoints;
    bool paths;
    bool transformDists;
    bool attentionDists;
    bool purpleMistAvoid;
    bool leevers;
    float opacity;
};

struct TransientSettings {
    CollisionViewSettings collisionView;
    TriggerViewSettings triggerView;
    bool turboMode;
    bool moveLinkActive;
    bool stateShareLoadActive;
};

TransientSettings& getTransientSettings();

}  // namespace dusk
