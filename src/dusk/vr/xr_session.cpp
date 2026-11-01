#include "dusk/vr/xr_session.h"

#include "dusk/logging.h"
#include "dusk/settings.h"

#include <aurora/xr.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dawn/native/D3D12Backend.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dusk::vr::xr {
namespace {

constexpr borealis::Log Log{"vr"};

using Microsoft::WRL::ComPtr;

#define XR_TRY(call)                                                                               \
    do {                                                                                           \
        const XrResult _res = (call);                                                              \
        if (XR_FAILED(_res)) {                                                                     \
            Log.error("{} failed: {}", #call, result_string(_res));                                \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_appSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
bool g_floorRelative = false;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
bool g_exitRequested = false;

std::array<XrViewConfigurationView, 2> g_viewConfigs{};
std::array<XrView, 2> g_views{};
std::array<EyeView, 2> g_eyes{};
Pose g_head;
bool g_headValid = false;
XrFrameState g_frameState{XR_TYPE_FRAME_STATE};
bool g_frameBegun = false;
bool g_viewsValid = false;
std::array<bool, 2> g_eyeRendered{};

// Runtime swapchain images lack D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, which Dawn needs
// to import a resource. Each eye renders into a shareable staging texture of the same format,
// copied into the acquired swapchain image on the same queue.
struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    std::vector<ID3D12Resource*> images;  // owned by the runtime
    ComPtr<ID3D12Resource> staging;
    wgpu::SharedTextureMemory stagingMemory;
    wgpu::Texture stagingTexture;
    uint32_t acquired = UINT32_MAX;
};
std::array<Swapchain, 2> g_swapchains;

// HUD panel: a quad layer fed from the black/white HUD passes.
Swapchain g_hud;
wgpu::Texture g_hudBlack;  // first HUD pass, kept until the white pass composes
bool g_hudRendered = false;  // a new HUD image is waiting to be copied this frame
bool g_hudValid = false;     // the HUD swapchain holds an image
Pose g_hudPose;
float g_hudWidth = 1.3f;
float g_hudHeight = 0.73f;

// Menu pointer dot. Its image never changes, so it is drawn once.
Swapchain g_cursor;
bool g_cursorRendered = false;  // drawn this frame, waiting to be copied
bool g_cursorValid = false;
bool g_cursorVisible = false;
Pose g_cursorPose;
float g_cursorSize = 0.02f;

ComPtr<ID3D12Device> g_d3dDevice;
ComPtr<ID3D12CommandQueue> g_d3dQueue;
ComPtr<ID3D12Fence> g_copyFence;
uint64_t g_copyFenceValue = 0;
HANDLE g_copyEvent = nullptr;
struct CopyContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fenceValue = 0;
};
std::array<CopyContext, 3> g_copyContexts;
size_t g_copyIndex = 0;

// Actions
XrActionSet g_actionSet = XR_NULL_HANDLE;
XrAction g_gripPose = XR_NULL_HANDLE;
XrAction g_aimPose = XR_NULL_HANDLE;
XrAction g_stick = XR_NULL_HANDLE;
XrAction g_stickClick = XR_NULL_HANDLE;
XrAction g_trigger = XR_NULL_HANDLE;
XrAction g_squeeze = XR_NULL_HANDLE;
XrAction g_buttonLower = XR_NULL_HANDLE;
XrAction g_buttonUpper = XR_NULL_HANDLE;
XrAction g_menu = XR_NULL_HANDLE;
XrAction g_haptic = XR_NULL_HANDLE;
std::array<XrPath, 2> g_handPaths{};
std::array<XrSpace, 2> g_gripSpaces{};
std::array<XrSpace, 2> g_aimSpaces{};
InputState g_input;

// Blit pipelines, created on the render worker.
enum class BlitKind { Copy, HudCompose, Cursor, Eye };
struct BlitPipeline {
    BlitKind kind = BlitKind::Copy;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::RenderPipeline pipeline;
};
std::vector<BlitPipeline> g_blitPipelines;
wgpu::Sampler g_blitSampler;
wgpu::ShaderModule g_blitShader;
wgpu::ShaderModule g_hudShader;
wgpu::ShaderModule g_cursorShader;
wgpu::ShaderModule g_eyeShader;
wgpu::Buffer g_eyeParams;
float g_vignette = 0.f;

std::string result_string(XrResult res) {
    char buf[XR_MAX_RESULT_STRING_SIZE]{};
    if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, res, buf))) {
        return buf;
    }
    return std::to_string(static_cast<int>(res));
}

Pose to_pose(const XrPosef& p) {
    return {
        {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
        {p.position.x, p.position.y, p.position.z},
    };
}

XrPath path(const char* str) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_instance, str, &p);
    return p;
}

bool is_srgb(wgpu::TextureFormat format) {
    return format == wgpu::TextureFormat::RGBA8UnormSrgb ||
           format == wgpu::TextureFormat::BGRA8UnormSrgb;
}

wgpu::TextureFormat to_wgpu_format(int64_t dxgi) {
    switch (dxgi) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return wgpu::TextureFormat::RGBA8UnormSrgb;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return wgpu::TextureFormat::BGRA8UnormSrgb;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return wgpu::TextureFormat::RGBA8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return wgpu::TextureFormat::BGRA8Unorm;
    default:
        return wgpu::TextureFormat::Undefined;
    }
}

constexpr char kBlitShader[] = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    var o: VsOut;
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    o.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

fn to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + vec3f(0.055)) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

// The game writes display-referred (gamma encoded) values into a UNORM target. An sRGB swapchain
// re-encodes on write, so decode first to store the same bytes.
@fragment
fn fs_srgb(in: VsOut) -> @location(0) vec4f {
    let c = textureSample(src, samp, in.uv);
    return vec4f(to_linear(c.rgb), 1.0);
}

@fragment
fn fs_unorm(in: VsOut) -> @location(0) vec4f {
    let c = textureSample(src, samp, in.uv);
    return vec4f(c.rgb, 1.0);
}
)";

// Eye blit: the plain copy plus an optional comfort vignette.
constexpr char kEyeShader[] = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;
@group(0) @binding(2) var<uniform> params: vec4f;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    var o: VsOut;
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    o.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

fn to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + vec3f(0.055)) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn vignette(uv: vec2f) -> f32 {
    let d = length((uv - vec2f(0.5)) * 2.0);
    let inner = mix(1.2, 0.35, params.x);
    return 1.0 - smoothstep(inner, inner + 0.45, d) * min(params.x * 1.5, 1.0);
}

@fragment
fn fs_srgb(in: VsOut) -> @location(0) vec4f {
    let c = textureSample(src, samp, in.uv);
    return vec4f(to_linear(c.rgb) * vignette(in.uv), 1.0);
}

@fragment
fn fs_unorm(in: VsOut) -> @location(0) vec4f {
    let c = textureSample(src, samp, in.uv);
    return vec4f(c.rgb * vignette(in.uv), 1.0);
}
)";

constexpr char kCursorShader[] = R"(
struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    var o: VsOut;
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    o.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// A white dot with a dark rim, premultiplied. Linear and sRGB targets look the same for 0 and 1.
fn dot(uv: vec2f) -> vec4f {
    let d = length(uv - vec2f(0.5)) * 2.0;
    let fill = 1.0 - smoothstep(0.55, 0.65, d);
    let rim = 1.0 - smoothstep(0.85, 0.95, d);
    return vec4f(vec3f(fill), rim);
}

@fragment
fn fs_srgb(in: VsOut) -> @location(0) vec4f {
    return dot(in.uv);
}

@fragment
fn fs_unorm(in: VsOut) -> @location(0) vec4f {
    return dot(in.uv);
}
)";

// The HUD is drawn twice, over black and over white. Where it is opaque both agree; where it is
// see-through the white pass is brighter by exactly (1 - alpha). Outputs premultiplied alpha.
constexpr char kHudShader[] = R"(
@group(0) @binding(0) var black: texture_2d<f32>;
@group(0) @binding(1) var white: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VsOut {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VsOut {
    var o: VsOut;
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    o.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

fn to_linear(c: vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((c + vec3f(0.055)) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn compose(uv: vec2f) -> vec4f {
    let b = textureSample(black, samp, uv).rgb;
    let w = textureSample(white, samp, uv).rgb;
    let d = w - b;
    let a = clamp(1.0 - max(d.r, max(d.g, d.b)), 0.0, 1.0);
    let c = select(vec3f(0.0), clamp(b / a, vec3f(0.0), vec3f(1.0)), a > 0.004);
    return vec4f(c, a);
}

@fragment
fn fs_srgb(in: VsOut) -> @location(0) vec4f {
    let c = compose(in.uv);
    return vec4f(to_linear(c.rgb) * c.a, c.a);
}

@fragment
fn fs_unorm(in: VsOut) -> @location(0) vec4f {
    let c = compose(in.uv);
    return vec4f(c.rgb * c.a, c.a);
}
)";

const wgpu::RenderPipeline& blit_pipeline(
    const wgpu::Device& device, wgpu::TextureFormat format, BlitKind kind = BlitKind::Copy) {
    for (const auto& p : g_blitPipelines) {
        if (p.format == format && p.kind == kind) {
            return p.pipeline;
        }
    }
    if (!g_blitShader) {
        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = kBlitShader;
        const wgpu::ShaderModuleDescriptor desc{.nextInChain = &wgsl, .label = "VR eye blit"};
        g_blitShader = device.CreateShaderModule(&desc);
        wgpu::ShaderSourceWGSL hudWgsl;
        hudWgsl.code = kHudShader;
        const wgpu::ShaderModuleDescriptor hudDesc{.nextInChain = &hudWgsl, .label = "VR HUD"};
        g_hudShader = device.CreateShaderModule(&hudDesc);
        wgpu::ShaderSourceWGSL cursorWgsl;
        cursorWgsl.code = kCursorShader;
        const wgpu::ShaderModuleDescriptor cursorDesc{
            .nextInChain = &cursorWgsl, .label = "VR cursor"};
        g_cursorShader = device.CreateShaderModule(&cursorDesc);
        wgpu::ShaderSourceWGSL eyeWgsl;
        eyeWgsl.code = kEyeShader;
        const wgpu::ShaderModuleDescriptor eyeDesc{.nextInChain = &eyeWgsl, .label = "VR eye"};
        g_eyeShader = device.CreateShaderModule(&eyeDesc);
        const wgpu::BufferDescriptor paramsDesc{
            .label = "VR eye params",
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = 16,
        };
        g_eyeParams = device.CreateBuffer(&paramsDesc);
        const wgpu::SamplerDescriptor samplerDesc{
            .label = "VR eye blit sampler",
            .magFilter = wgpu::FilterMode::Linear,
            .minFilter = wgpu::FilterMode::Linear,
        };
        g_blitSampler = device.CreateSampler(&samplerDesc);
    }
    const wgpu::ShaderModule& shader = kind == BlitKind::HudCompose ? g_hudShader :
                                       kind == BlitKind::Cursor     ? g_cursorShader :
                                       kind == BlitKind::Eye        ? g_eyeShader :
                                                                      g_blitShader;
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fragment{
        .module = shader,
        .entryPoint = is_srgb(format) ? "fs_srgb" : "fs_unorm",
        .targetCount = 1,
        .targets = &target,
    };
    const wgpu::RenderPipelineDescriptor desc{
        .label = "VR blit",
        .vertex = {.module = shader, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .fragment = &fragment,
    };
    g_blitPipelines.push_back({kind, format, device.CreateRenderPipeline(&desc)});
    return g_blitPipelines.back().pipeline;
}

bool create_actions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "gameplay");
    std::strcpy(setInfo.localizedActionSetName, "Gameplay");
    XR_TRY(xrCreateActionSet(g_instance, &setInfo, &g_actionSet));

    g_handPaths[kLeft] = path("/user/hand/left");
    g_handPaths[kRight] = path("/user/hand/right");

    const auto create = [](XrAction& out, const char* name, const char* localized,
                            XrActionType type) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, localized);
        info.actionType = type;
        info.countSubactionPaths = static_cast<uint32_t>(g_handPaths.size());
        info.subactionPaths = g_handPaths.data();
        return XR_SUCCEEDED(xrCreateAction(g_actionSet, &info, &out));
    };
    if (!create(g_gripPose, "grip_pose", "Hand grip", XR_ACTION_TYPE_POSE_INPUT) ||
        !create(g_aimPose, "aim_pose", "Hand aim", XR_ACTION_TYPE_POSE_INPUT) ||
        !create(g_stick, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
        !create(g_stickClick, "stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !create(g_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !create(g_squeeze, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !create(g_buttonLower, "button_lower", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !create(g_buttonUpper, "button_upper", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !create(g_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
        !create(g_haptic, "haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT))
    {
        Log.error("Failed to create OpenXR actions");
        return false;
    }

    struct Binding {
        XrAction action;
        const char* path;
    };
    const auto suggest = [](const char* profile, std::initializer_list<Binding> bindings) {
        std::vector<XrActionSuggestedBinding> suggested;
        for (const auto& b : bindings) {
            suggested.push_back({b.action, path(b.path)});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = path(profile);
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult res = xrSuggestInteractionProfileBindings(g_instance, &info);
        if (XR_FAILED(res)) {
            Log.warn("Bindings for {} rejected: {}", profile, result_string(res));
        }
    };

    suggest("/interaction_profiles/valve/index_controller",
        {
            {g_gripPose, "/user/hand/left/input/grip/pose"},
            {g_gripPose, "/user/hand/right/input/grip/pose"},
            {g_aimPose, "/user/hand/left/input/aim/pose"},
            {g_aimPose, "/user/hand/right/input/aim/pose"},
            {g_stick, "/user/hand/left/input/thumbstick"},
            {g_stick, "/user/hand/right/input/thumbstick"},
            {g_stickClick, "/user/hand/left/input/thumbstick/click"},
            {g_stickClick, "/user/hand/right/input/thumbstick/click"},
            {g_trigger, "/user/hand/left/input/trigger/value"},
            {g_trigger, "/user/hand/right/input/trigger/value"},
            {g_squeeze, "/user/hand/left/input/squeeze/value"},
            {g_squeeze, "/user/hand/right/input/squeeze/value"},
            {g_buttonLower, "/user/hand/left/input/a/click"},
            {g_buttonLower, "/user/hand/right/input/a/click"},
            {g_buttonUpper, "/user/hand/left/input/b/click"},
            {g_buttonUpper, "/user/hand/right/input/b/click"},
            {g_menu, "/user/hand/left/input/system/click"},
            {g_haptic, "/user/hand/left/output/haptic"},
            {g_haptic, "/user/hand/right/output/haptic"},
        });
    suggest("/interaction_profiles/oculus/touch_controller",
        {
            {g_gripPose, "/user/hand/left/input/grip/pose"},
            {g_gripPose, "/user/hand/right/input/grip/pose"},
            {g_aimPose, "/user/hand/left/input/aim/pose"},
            {g_aimPose, "/user/hand/right/input/aim/pose"},
            {g_stick, "/user/hand/left/input/thumbstick"},
            {g_stick, "/user/hand/right/input/thumbstick"},
            {g_stickClick, "/user/hand/left/input/thumbstick/click"},
            {g_stickClick, "/user/hand/right/input/thumbstick/click"},
            {g_trigger, "/user/hand/left/input/trigger/value"},
            {g_trigger, "/user/hand/right/input/trigger/value"},
            {g_squeeze, "/user/hand/left/input/squeeze/value"},
            {g_squeeze, "/user/hand/right/input/squeeze/value"},
            {g_buttonLower, "/user/hand/left/input/x/click"},
            {g_buttonLower, "/user/hand/right/input/a/click"},
            {g_buttonUpper, "/user/hand/left/input/y/click"},
            {g_buttonUpper, "/user/hand/right/input/b/click"},
            {g_menu, "/user/hand/left/input/menu/click"},
            {g_haptic, "/user/hand/left/output/haptic"},
            {g_haptic, "/user/hand/right/output/haptic"},
        });
    suggest("/interaction_profiles/htc/vive_controller",
        {
            {g_gripPose, "/user/hand/left/input/grip/pose"},
            {g_gripPose, "/user/hand/right/input/grip/pose"},
            {g_aimPose, "/user/hand/left/input/aim/pose"},
            {g_aimPose, "/user/hand/right/input/aim/pose"},
            {g_stick, "/user/hand/left/input/trackpad"},
            {g_stick, "/user/hand/right/input/trackpad"},
            {g_stickClick, "/user/hand/left/input/trackpad/click"},
            {g_stickClick, "/user/hand/right/input/trackpad/click"},
            {g_trigger, "/user/hand/left/input/trigger/value"},
            {g_trigger, "/user/hand/right/input/trigger/value"},
            {g_squeeze, "/user/hand/left/input/squeeze/click"},
            {g_squeeze, "/user/hand/right/input/squeeze/click"},
            {g_buttonUpper, "/user/hand/left/input/menu/click"},
            {g_buttonUpper, "/user/hand/right/input/menu/click"},
            {g_haptic, "/user/hand/left/output/haptic"},
            {g_haptic, "/user/hand/right/output/haptic"},
        });
    suggest("/interaction_profiles/microsoft/motion_controller",
        {
            {g_gripPose, "/user/hand/left/input/grip/pose"},
            {g_gripPose, "/user/hand/right/input/grip/pose"},
            {g_aimPose, "/user/hand/left/input/aim/pose"},
            {g_aimPose, "/user/hand/right/input/aim/pose"},
            {g_stick, "/user/hand/left/input/thumbstick"},
            {g_stick, "/user/hand/right/input/thumbstick"},
            {g_stickClick, "/user/hand/left/input/thumbstick/click"},
            {g_stickClick, "/user/hand/right/input/thumbstick/click"},
            {g_trigger, "/user/hand/left/input/trigger/value"},
            {g_trigger, "/user/hand/right/input/trigger/value"},
            {g_squeeze, "/user/hand/left/input/squeeze/click"},
            {g_squeeze, "/user/hand/right/input/squeeze/click"},
            {g_buttonLower, "/user/hand/left/input/trackpad/click"},
            {g_buttonLower, "/user/hand/right/input/trackpad/click"},
            {g_buttonUpper, "/user/hand/left/input/menu/click"},
            {g_buttonUpper, "/user/hand/right/input/menu/click"},
            {g_haptic, "/user/hand/left/output/haptic"},
            {g_haptic, "/user/hand/right/output/haptic"},
        });
    suggest("/interaction_profiles/khr/simple_controller",
        {
            {g_gripPose, "/user/hand/left/input/grip/pose"},
            {g_gripPose, "/user/hand/right/input/grip/pose"},
            {g_aimPose, "/user/hand/left/input/aim/pose"},
            {g_aimPose, "/user/hand/right/input/aim/pose"},
            {g_buttonLower, "/user/hand/left/input/select/click"},
            {g_buttonLower, "/user/hand/right/input/select/click"},
            {g_menu, "/user/hand/left/input/menu/click"},
            {g_haptic, "/user/hand/left/output/haptic"},
            {g_haptic, "/user/hand/right/output/haptic"},
        });
    return true;
}

bool create_action_spaces() {
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.subactionPath = g_handPaths[h];
        info.poseInActionSpace.orientation.w = 1.f;
        info.action = g_gripPose;
        XR_TRY(xrCreateActionSpace(g_session, &info, &g_gripSpaces[h]));
        info.action = g_aimPose;
        XR_TRY(xrCreateActionSpace(g_session, &info, &g_aimSpaces[h]));
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    XR_TRY(xrAttachSessionActionSets(g_session, &attach));
    return true;
}

bool create_swapchain(Swapchain& sc, uint32_t width, uint32_t height, int64_t format,
    const char* label) {
    sc.width = width;
    sc.height = height;
    sc.dxgiFormat = static_cast<DXGI_FORMAT>(format);
    sc.format = to_wgpu_format(format);

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XR_TRY(xrCreateSwapchain(g_session, &info, &sc.handle));

    uint32_t imageCount = 0;
    XR_TRY(xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr));
    std::vector<XrSwapchainImageD3D12KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    XR_TRY(xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));
    for (const auto& image : images) {
        sc.images.push_back(image.texture);
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = sc.dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (FAILED(g_d3dDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sc.staging))))
    {
        Log.error("Failed to create the {} staging texture", label);
        return false;
    }

    dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor d3dDesc;
    d3dDesc.resource = sc.staging;
    wgpu::SharedTextureMemoryDescriptor memDesc{};
    memDesc.nextInChain = &d3dDesc;
    memDesc.label = label;
    sc.stagingMemory = aurora::xr::device().ImportSharedTextureMemory(&memDesc);
    if (!sc.stagingMemory) {
        Log.error("Failed to import the {} staging texture into WebGPU", label);
        return false;
    }
    const wgpu::TextureDescriptor texDesc{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment,
        .size = {width, height, 1},
        .format = sc.format,
    };
    sc.stagingTexture = sc.stagingMemory.CreateTexture(&texDesc);
    Log.info("{} swapchain: {}x{}, {} images, format {}", label, width, height, imageCount,
        static_cast<int>(format));
    return true;
}

bool create_swapchains() {
    uint32_t count = 0;
    XR_TRY(xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr));
    std::vector<int64_t> formats(count);
    XR_TRY(xrEnumerateSwapchainFormats(g_session, count, &count, formats.data()));

    int64_t chosen = 0;
    for (const int64_t preferred :
         {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
             DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM})
    {
        if (std::ranges::find(formats, preferred) != formats.end()) {
            chosen = preferred;
            break;
        }
    }
    if (chosen == 0) {
        Log.error("Runtime offers no supported 8-bit color swapchain format");
        return false;
    }

    const float scale = std::clamp(getSettings().vr.resolutionScale.getValue(), 0.3f, 2.0f);
    for (int eye = 0; eye < 2; ++eye) {
        const auto& view = g_viewConfigs[eye];
        const uint32_t width = std::min(view.maxImageRectWidth,
            std::max(16u, static_cast<uint32_t>(view.recommendedImageRectWidth * scale)));
        const uint32_t height = std::min(view.maxImageRectHeight,
            std::max(16u, static_cast<uint32_t>(view.recommendedImageRectHeight * scale)));
        if (!create_swapchain(g_swapchains[eye], width, height, chosen,
                eye == 0 ? "VR left eye" : "VR right eye"))
        {
            return false;
        }
    }
    // The HUD is drawn into the eye-sized framebuffer, so its panel matches that size.
    if (!create_swapchain(g_hud, g_swapchains[0].width, g_swapchains[0].height, chosen, "VR HUD") ||
        !create_swapchain(g_cursor, 64, 64, chosen, "VR cursor"))
    {
        return false;
    }

    if (FAILED(g_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence)))) {
        return false;
    }
    g_copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto& ctx : g_copyContexts) {
        if (FAILED(g_d3dDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.allocator))) ||
            FAILED(g_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                ctx.allocator.Get(), nullptr, IID_PPV_ARGS(&ctx.list))))
        {
            return false;
        }
        ctx.list->Close();
    }
    return true;
}

void record_copy(ID3D12GraphicsCommandList* list, Swapchain& sc) {
    ID3D12Resource* image = sc.images[sc.acquired];
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &barrier);
    // The staging texture allows simultaneous access, so it is promoted to COPY_SOURCE implicitly.
    list->CopyResource(image, sc.staging.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
}

// Copies each rendered staging texture into its acquired swapchain image. Must run after the
// frames that rendered them were submitted, since it shares Dawn's queue.
void copy_to_swapchains() {
    auto& ctx = g_copyContexts[g_copyIndex];
    g_copyIndex = (g_copyIndex + 1) % g_copyContexts.size();
    if (g_copyFence->GetCompletedValue() < ctx.fenceValue) {
        g_copyFence->SetEventOnCompletion(ctx.fenceValue, g_copyEvent);
        WaitForSingleObject(g_copyEvent, INFINITE);
    }
    ctx.allocator->Reset();
    ctx.list->Reset(ctx.allocator.Get(), nullptr);

    bool any = false;
    for (int eye = 0; eye < 2; ++eye) {
        auto& sc = g_swapchains[eye];
        if (sc.acquired != UINT32_MAX && g_eyeRendered[eye]) {
            record_copy(ctx.list.Get(), sc);
            any = true;
        }
    }
    if (g_hud.acquired != UINT32_MAX && g_hudRendered) {
        record_copy(ctx.list.Get(), g_hud);
        any = true;
    }
    if (g_cursor.acquired != UINT32_MAX && g_cursorRendered) {
        record_copy(ctx.list.Get(), g_cursor);
        any = true;
    }
    ctx.list->Close();
    if (!any) {
        return;
    }
    ID3D12CommandList* lists[] = {ctx.list.Get()};
    g_d3dQueue->ExecuteCommandLists(1, lists);
    ctx.fenceValue = ++g_copyFenceValue;
    g_d3dQueue->Signal(g_copyFence.Get(), ctx.fenceValue);
}

void read_input() {
    const XrActiveActionSet active{g_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(g_session, &sync))) {
        return;
    }

    InputState in;
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = g_handPaths[h];

        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        get.action = g_trigger;
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &get, &f))) {
            in.trigger[h] = f.currentState;
        }
        get.action = g_squeeze;
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &get, &f))) {
            in.squeeze[h] = f.currentState;
        }

        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = g_stick;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &get, &v))) {
            in.stickX[h] = v.currentState.x;
            in.stickY[h] = v.currentState.y;
        }

        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        get.action = g_stickClick;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &b))) {
            in.stickClick[h] = b.currentState;
        }
        get.action = g_buttonLower;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &b))) {
            in.buttonLower[h] = b.currentState;
        }
        get.action = g_buttonUpper;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &b))) {
            in.buttonUpper[h] = b.currentState;
        }
        get.action = g_menu;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &b)) && b.currentState) {
            in.menu = true;
        }

        auto& hand = in.hands[h];
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
        const XrTime time = g_frameState.predictedDisplayTime;
        if (XR_SUCCEEDED(xrLocateSpace(g_gripSpaces[h], g_appSpace, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            hand.active = true;
            hand.grip = to_pose(loc.pose);
            if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                hand.velocity = {vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z};
                hand.velocityValid = true;
            }
        }
        if (hand.active != g_input.hands[h].active) {
            Log.info("{} controller {}", h == kLeft ? "Left" : "Right",
                hand.active ? "tracked" : "lost");
        }
        XrSpaceLocation aimLoc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_aimSpaces[h], g_appSpace, time, &aimLoc)) &&
            (aimLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            hand.aim = to_pose(aimLoc.pose);
        } else {
            hand.aim = hand.grip;
        }
    }
    g_input = in;
}

void destroy_session() {
    aurora::xr::synchronize();
    const auto destroy = [](Swapchain& sc) {
        sc.stagingTexture = {};
        sc.stagingMemory = {};
        sc.staging.Reset();
        sc.images.clear();
        if (sc.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(sc.handle);
        }
        sc = {};
    };
    for (auto& sc : g_swapchains) {
        destroy(sc);
    }
    destroy(g_hud);
    destroy(g_cursor);
    g_cursorValid = false;
    g_cursorRendered = false;
    g_hudBlack = {};
    g_hudValid = false;
    g_hudRendered = false;
    for (int h = 0; h < 2; ++h) {
        if (g_gripSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(g_gripSpaces[h]);
        }
        if (g_aimSpaces[h] != XR_NULL_HANDLE) {
            xrDestroySpace(g_aimSpaces[h]);
        }
        g_gripSpaces[h] = g_aimSpaces[h] = XR_NULL_HANDLE;
    }
    if (g_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(g_viewSpace);
        g_viewSpace = XR_NULL_HANDLE;
    }
    if (g_appSpace != XR_NULL_HANDLE) {
        xrDestroySpace(g_appSpace);
        g_appSpace = XR_NULL_HANDLE;
    }
    if (g_copyFence && g_copyFence->GetCompletedValue() < g_copyFenceValue) {
        g_copyFence->SetEventOnCompletion(g_copyFenceValue, g_copyEvent);
        WaitForSingleObject(g_copyEvent, INFINITE);
    }
    g_copyContexts = {};
    g_copyFence.Reset();
    if (g_copyEvent != nullptr) {
        CloseHandle(g_copyEvent);
        g_copyEvent = nullptr;
    }
    if (g_session != XR_NULL_HANDLE) {
        xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    }
    g_d3dQueue.Reset();
    g_d3dDevice.Reset();
    g_running = false;
}

bool create_session() {
    if (aurora::xr::backend_type() != wgpu::BackendType::D3D12) {
        Log.error("VR needs the D3D12 graphics backend (Settings > Graphics > Backend)");
        return false;
    }

    auto getRequirements = reinterpret_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(nullptr);
    XR_TRY(xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XR_TRY(getRequirements(g_instance, g_system, &requirements));

    const wgpu::Device device = aurora::xr::device();
    g_d3dDevice = dawn::native::d3d12::GetD3D12Device(device.Get());
    g_d3dQueue = dawn::native::d3d12::GetD3D12CommandQueue(device.Get());
    if (!g_d3dDevice || !g_d3dQueue) {
        Log.error("Could not get the D3D12 device from Dawn");
        return false;
    }
    const LUID luid = g_d3dDevice->GetAdapterLuid();
    if (std::memcmp(&luid, &requirements.adapterLuid, sizeof(LUID)) != 0) {
        Log.warn("The headset is connected to a different GPU than the one rendering the game");
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = g_d3dDevice.Get();
    binding.queue = g_d3dQueue.Get();
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, &binding};
    info.systemId = g_system;
    XR_TRY(xrCreateSession(g_instance, &info, &g_session));

    uint32_t spaceCount = 0;
    XR_TRY(xrEnumerateReferenceSpaces(g_session, 0, &spaceCount, nullptr));
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    XR_TRY(xrEnumerateReferenceSpaces(g_session, spaceCount, &spaceCount, spaces.data()));
    g_floorRelative =
        std::ranges::find(spaces, XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
    spaceInfo.referenceSpaceType =
        g_floorRelative ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_TRY(xrCreateReferenceSpace(g_session, &spaceInfo, &g_appSpace));
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_TRY(xrCreateReferenceSpace(g_session, &spaceInfo, &g_viewSpace));

    if (!create_action_spaces()) {
        return false;
    }
    aurora::xr::synchronize();
    return create_swapchains();
}

}  // namespace

bool initialize() {
    uint32_t extCount = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
        Log.info("No OpenXR runtime installed; VR disabled");
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    const bool hasD3D12 = std::ranges::any_of(exts, [](const XrExtensionProperties& e) {
        return std::strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
    });
    if (!hasD3D12) {
        Log.warn("The OpenXR runtime does not support D3D12; VR disabled");
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(info.applicationInfo.applicationName, "Dusklight VR");
    info.applicationInfo.applicationVersion = 1;
    std::strcpy(info.applicationInfo.engineName, "Aurora");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
    XR_TRY(xrCreateInstance(&info, &g_instance));

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props))) {
        Log.info("OpenXR runtime: {} {}.{}.{}", props.runtimeName,
            XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
            XR_VERSION_PATCH(props.runtimeVersion));
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (const XrResult res = xrGetSystem(g_instance, &systemInfo, &g_system); XR_FAILED(res)) {
        Log.info("No headset found ({}); VR disabled", result_string(res));
        shutdown();
        return false;
    }

    uint32_t viewCount = 0;
    XR_TRY(xrEnumerateViewConfigurationViews(g_instance, g_system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    if (viewCount != 2) {
        Log.error("Expected 2 stereo views, runtime reports {}", viewCount);
        shutdown();
        return false;
    }
    g_viewConfigs.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_TRY(xrEnumerateViewConfigurationViews(g_instance, g_system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, g_viewConfigs.data()));

    if (!create_actions() || !create_session()) {
        shutdown();
        return false;
    }
    Log.info("VR session created");
    return true;
}

void shutdown() {
    destroy_session();
    if (g_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(g_actionSet);
        g_actionSet = XR_NULL_HANDLE;
    }
    if (g_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
    }
    g_blitPipelines.clear();
    g_blitShader = {};
    g_hudShader = {};
    g_cursorShader = {};
    g_eyeShader = {};
    g_eyeParams = {};
    g_blitSampler = {};
}

bool poll_events() {
    if (g_instance == XR_NULL_HANDLE) {
        return false;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            g_state = changed.state;
            switch (g_state) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &begin))) {
                    g_running = true;
                    Log.info("VR session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(g_session);
                g_running = false;
                Log.info("VR session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                g_exitRequested = true;
                g_running = false;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            g_exitRequested = true;
            g_running = false;
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !g_exitRequested;
}

bool session_running() { return g_running; }

bool begin_frame() {
    g_frameBegun = false;
    g_viewsValid = false;
    g_eyeRendered = {};
    if (!g_running) {
        return false;
    }

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    g_frameState = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &waitInfo, &g_frameState))) {
        return false;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &beginInfo))) {
        return false;
    }
    g_frameBegun = true;

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_frameState.predictedDisplayTime;
    locate.space = g_appSpace;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    g_views.fill({XR_TYPE_VIEW});
    uint32_t count = 0;
    if (XR_SUCCEEDED(xrLocateViews(g_session, &locate, &viewState, 2, &count, g_views.data())) &&
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    {
        g_viewsValid = true;
        for (int i = 0; i < 2; ++i) {
            g_eyes[i].pose = to_pose(g_views[i].pose);
            g_eyes[i].angleLeft = g_views[i].fov.angleLeft;
            g_eyes[i].angleRight = g_views[i].fov.angleRight;
            g_eyes[i].angleUp = g_views[i].fov.angleUp;
            g_eyes[i].angleDown = g_views[i].fov.angleDown;
        }
    }

    XrSpaceLocation headLoc{XR_TYPE_SPACE_LOCATION};
    g_headValid =
        XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_appSpace, g_frameState.predictedDisplayTime,
            &headLoc)) &&
        (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (g_headValid) {
        g_head = to_pose(headLoc.pose);
    }

    read_input();
    return true;
}

bool should_render() { return g_frameBegun && g_frameState.shouldRender && g_viewsValid; }
bool views_valid() { return g_frameBegun && g_viewsValid; }

const EyeView& eye(int index) { return g_eyes[index]; }
const Pose& head() { return g_head; }
bool head_valid() { return g_headValid; }
uint32_t eye_width() { return g_swapchains[0].width; }
uint32_t eye_height() { return g_swapchains[0].height; }
bool floor_relative() { return g_floorRelative; }

void render_eye(int index) {
    auto& sc = g_swapchains[index];
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(sc.handle, &acquire, &image))) {
        return;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &wait))) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc.handle, &release);
        return;
    }
    sc.acquired = image;
    g_eyeRendered[index] = true;

    aurora::xr::set_next_frame_hook(
        [target = sc.stagingTexture, memory = sc.stagingMemory, format = sc.format,
            vignette = g_vignette](
            wgpu::CommandEncoder& encoder, const aurora::xr::PresentSource& source,
            std::vector<aurora::xr::AfterSubmitCallback>& afterSubmit) {
            const wgpu::Device device = aurora::xr::device();
            wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
            begin.initialized = false;
            begin.fenceCount = 0;
            if (memory.BeginAccess(target, &begin) != wgpu::Status::Success) {
                Log.error("BeginAccess on VR swapchain image failed");
                return;
            }

            const auto& pipeline = blit_pipeline(device, format, BlitKind::Eye);
            const float params[4] = {vignette, 0.f, 0.f, 0.f};
            aurora::xr::queue().WriteBuffer(g_eyeParams, 0, params, sizeof(params));
            const wgpu::BindGroupEntry entries[] = {
                {.binding = 0, .textureView = source.view},
                {.binding = 1, .sampler = g_blitSampler},
                {.binding = 2, .buffer = g_eyeParams, .size = 16},
            };
            const wgpu::BindGroupDescriptor bindDesc{
                .layout = pipeline.GetBindGroupLayout(0),
                .entryCount = 3,
                .entries = entries,
            };
            const wgpu::BindGroup bindGroup = device.CreateBindGroup(&bindDesc);
            const wgpu::RenderPassColorAttachment attachment{
                .view = target.CreateView(),
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
                .clearValue = {0.0, 0.0, 0.0, 1.0},
            };
            const wgpu::RenderPassDescriptor passDesc{
                .label = "VR eye blit",
                .colorAttachmentCount = 1,
                .colorAttachments = &attachment,
            };
            const auto pass = encoder.BeginRenderPass(&passDesc);
            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, bindGroup);
            pass.Draw(3);
            pass.End();

            afterSubmit.push_back([target, memory] {
                wgpu::SharedTextureMemoryEndAccessState state;
                memory.EndAccess(target, &state);
            });
        });
}

void render_hud(int phase) {
    if (phase == 0) {
        // Keep the over-black pass for the compose step.
        aurora::xr::set_next_frame_hook([](wgpu::CommandEncoder& encoder,
                                             const aurora::xr::PresentSource& source,
                                             std::vector<aurora::xr::AfterSubmitCallback>&) {
            const wgpu::Device device = aurora::xr::device();
            if (!g_hudBlack || g_hudBlack.GetWidth() != source.width ||
                g_hudBlack.GetHeight() != source.height || g_hudBlack.GetFormat() != source.format)
            {
                const wgpu::TextureDescriptor desc{
                    .label = "VR HUD over black",
                    .usage = wgpu::TextureUsage::RenderAttachment |
                             wgpu::TextureUsage::TextureBinding,
                    .size = {source.width, source.height, 1},
                    .format = source.format,
                };
                g_hudBlack = device.CreateTexture(&desc);
            }
            const auto& pipeline = blit_pipeline(device, source.format);
            const wgpu::BindGroupEntry entries[] = {
                {.binding = 0, .textureView = source.view},
                {.binding = 1, .sampler = g_blitSampler},
            };
            const wgpu::BindGroupDescriptor bindDesc{
                .layout = pipeline.GetBindGroupLayout(0),
                .entryCount = 2,
                .entries = entries,
            };
            const wgpu::RenderPassColorAttachment attachment{
                .view = g_hudBlack.CreateView(),
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            };
            const wgpu::RenderPassDescriptor passDesc{
                .label = "VR HUD capture",
                .colorAttachmentCount = 1,
                .colorAttachments = &attachment,
            };
            const auto pass = encoder.BeginRenderPass(&passDesc);
            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, device.CreateBindGroup(&bindDesc));
            pass.Draw(3);
            pass.End();
        });
        return;
    }

    if (g_hud.handle == XR_NULL_HANDLE || g_hud.acquired != UINT32_MAX) {
        return;
    }
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_hud.handle, &acquire, &image))) {
        return;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g_hud.handle, &wait))) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_hud.handle, &release);
        return;
    }
    g_hud.acquired = image;
    g_hudRendered = true;

    // The cursor image is drawn once, alongside the first HUD.
    bool drawCursor = false;
    if (!g_cursorValid && g_cursor.handle != XR_NULL_HANDLE && g_cursor.acquired == UINT32_MAX) {
        uint32_t cursorImage = 0;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_cursor.handle, &acquire, &cursorImage)) &&
            XR_SUCCEEDED(xrWaitSwapchainImage(g_cursor.handle, &wait)))
        {
            g_cursor.acquired = cursorImage;
            g_cursorRendered = true;
            drawCursor = true;
        }
    }

    aurora::xr::set_next_frame_hook(
        [target = g_hud.stagingTexture, memory = g_hud.stagingMemory, format = g_hud.format,
            drawCursor, cursorTarget = g_cursor.stagingTexture,
            cursorMemory = g_cursor.stagingMemory, cursorFormat = g_cursor.format](
            wgpu::CommandEncoder& encoder, const aurora::xr::PresentSource& source,
            std::vector<aurora::xr::AfterSubmitCallback>& afterSubmit) {
            const wgpu::Device device = aurora::xr::device();
            if (drawCursor) {
                wgpu::SharedTextureMemoryBeginAccessDescriptor cursorBegin{};
                cursorBegin.initialized = false;
                cursorBegin.fenceCount = 0;
                if (cursorMemory.BeginAccess(cursorTarget, &cursorBegin) == wgpu::Status::Success) {
                    const wgpu::RenderPassColorAttachment attachment{
                        .view = cursorTarget.CreateView(),
                        .loadOp = wgpu::LoadOp::Clear,
                        .storeOp = wgpu::StoreOp::Store,
                        .clearValue = {0.0, 0.0, 0.0, 0.0},
                    };
                    const wgpu::RenderPassDescriptor passDesc{
                        .label = "VR cursor",
                        .colorAttachmentCount = 1,
                        .colorAttachments = &attachment,
                    };
                    const auto pass = encoder.BeginRenderPass(&passDesc);
                    pass.SetPipeline(blit_pipeline(device, cursorFormat, BlitKind::Cursor));
                    pass.Draw(3);
                    pass.End();
                    afterSubmit.push_back([cursorTarget, cursorMemory] {
                        wgpu::SharedTextureMemoryEndAccessState state;
                        cursorMemory.EndAccess(cursorTarget, &state);
                    });
                }
            }
            if (!g_hudBlack) {
                return;
            }
            wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
            begin.initialized = false;
            begin.fenceCount = 0;
            if (memory.BeginAccess(target, &begin) != wgpu::Status::Success) {
                Log.error("BeginAccess on the VR HUD texture failed");
                return;
            }
            const auto& pipeline = blit_pipeline(device, format, BlitKind::HudCompose);
            const wgpu::BindGroupEntry entries[] = {
                {.binding = 0, .textureView = g_hudBlack.CreateView()},
                {.binding = 1, .textureView = source.view},
                {.binding = 2, .sampler = g_blitSampler},
            };
            const wgpu::BindGroupDescriptor bindDesc{
                .layout = pipeline.GetBindGroupLayout(0),
                .entryCount = 3,
                .entries = entries,
            };
            const wgpu::RenderPassColorAttachment attachment{
                .view = target.CreateView(),
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
                .clearValue = {0.0, 0.0, 0.0, 0.0},
            };
            const wgpu::RenderPassDescriptor passDesc{
                .label = "VR HUD compose",
                .colorAttachmentCount = 1,
                .colorAttachments = &attachment,
            };
            const auto pass = encoder.BeginRenderPass(&passDesc);
            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, device.CreateBindGroup(&bindDesc));
            pass.Draw(3);
            pass.End();
            afterSubmit.push_back([target, memory] {
                wgpu::SharedTextureMemoryEndAccessState state;
                memory.EndAccess(target, &state);
            });
        });
}

void set_vignette(float amount) { g_vignette = std::clamp(amount, 0.f, 1.f); }

void set_cursor(bool visible, const Pose& pose, float sizeMeters) {
    g_cursorVisible = visible;
    g_cursorPose = pose;
    g_cursorSize = sizeMeters;
}

void set_hud_pose(const Pose& pose, float widthMeters, float heightMeters) {
    g_hudPose = pose;
    g_hudWidth = widthMeters;
    g_hudHeight = heightMeters;
}

void end_frame() {
    if (!g_frameBegun) {
        return;
    }
    // Swapchain images must be released only after the frames that wrote them are submitted.
    aurora::xr::synchronize();
    copy_to_swapchains();
    if (g_hud.acquired != UINT32_MAX) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_hud.handle, &release);
        g_hud.acquired = UINT32_MAX;
        g_hudValid = g_hudValid || g_hudRendered;
        g_hudRendered = false;
    }
    if (g_cursor.acquired != UINT32_MAX) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_cursor.handle, &release);
        g_cursor.acquired = UINT32_MAX;
        g_cursorValid = g_cursorValid || g_cursorRendered;
        g_cursorRendered = false;
    }

    std::array<XrCompositionLayerProjectionView, 2> projViews{};
    bool allRendered = true;
    for (int eye = 0; eye < 2; ++eye) {
        auto& sc = g_swapchains[eye];
        if (sc.acquired != UINT32_MAX) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(sc.handle, &release);
            sc.acquired = UINT32_MAX;
        }
        allRendered = allRendered && g_eyeRendered[eye];
        projViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        projViews[eye].pose = g_views[eye].pose;
        projViews[eye].fov = g_views[eye].fov;
        projViews[eye].subImage.swapchain = sc.handle;
        projViews[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height)};
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_appSpace;
    layer.viewCount = 2;
    layer.views = projViews.data();
    XrCompositionLayerQuad hudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.space = g_appSpace;
    hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    hudLayer.subImage.swapchain = g_hud.handle;
    hudLayer.subImage.imageRect.extent = {
        static_cast<int32_t>(g_hud.width), static_cast<int32_t>(g_hud.height)};
    hudLayer.pose.orientation = {g_hudPose.orientation.x, g_hudPose.orientation.y,
        g_hudPose.orientation.z, g_hudPose.orientation.w};
    hudLayer.pose.position = {g_hudPose.position.x, g_hudPose.position.y, g_hudPose.position.z};
    hudLayer.size = {g_hudWidth, g_hudHeight};

    XrCompositionLayerQuad cursorLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    cursorLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    cursorLayer.space = g_appSpace;
    cursorLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    cursorLayer.subImage.swapchain = g_cursor.handle;
    cursorLayer.subImage.imageRect.extent = {
        static_cast<int32_t>(g_cursor.width), static_cast<int32_t>(g_cursor.height)};
    cursorLayer.pose.orientation = {g_cursorPose.orientation.x, g_cursorPose.orientation.y,
        g_cursorPose.orientation.z, g_cursorPose.orientation.w};
    cursorLayer.pose.position = {
        g_cursorPose.position.x, g_cursorPose.position.y, g_cursorPose.position.z};
    cursorLayer.size = {g_cursorSize, g_cursorSize};

    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    if (g_hudValid) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudLayer);
        if (g_cursorValid && g_cursorVisible) {
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cursorLayer);
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const bool submit = should_render() && allRendered;
    endInfo.layerCount = submit ? layerCount : 0;
    endInfo.layers = submit ? layers : nullptr;
    if (const XrResult res = xrEndFrame(g_session, &endInfo); XR_FAILED(res)) {
        Log.warn("xrEndFrame failed: {}", result_string(res));
    }
    g_frameBegun = false;
}

const InputState& input() { return g_input; }

void haptic(Hand hand, float amplitude, float seconds) {
    if (!g_running || g_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.f, 1.f);
    vibration.duration = static_cast<XrDuration>(seconds * 1e9);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = g_haptic;
    info.subactionPath = g_handPaths[hand];
    xrApplyHapticFeedback(
        g_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

double predicted_display_seconds() { return g_frameState.predictedDisplayTime * 1e-9; }

}  // namespace dusk::vr::xr
