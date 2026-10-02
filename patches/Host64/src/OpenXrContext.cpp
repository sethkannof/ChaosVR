#include "ChaosVrHost/OpenXrContext.h"
#include "ChaosVrHost/XrCheck.h"
#include "Protocol/ChaosVrSharedState.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace chaosvr::host {

static XrPath ToPath(XrInstance instance, const char* s) {
    XrPath p{}; XrCheck(xrStringToPath(instance, s, &p), "xrStringToPath"); return p;
}

OpenXrContext::~OpenXrContext() { Shutdown(); }

bool OpenXrContext::HasExtension(const char* name) const {
    return std::any_of(extensions_.begin(), extensions_.end(), [name](auto const& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

void OpenXrContext::Initialize() {
    uint32_t extCount{};
    XrCheck(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr), "xrEnumerateInstanceExtensionProperties(count)");
    extensions_.assign(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XrCheck(xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extensions_.data()), "xrEnumerateInstanceExtensionProperties");
    if (!HasExtension(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
        throw std::runtime_error("OpenXR runtime does not expose XR_KHR_D3D11_enable");

    std::vector<const char*> enabled{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (HasExtension(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME))
        enabled.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ici.applicationInfo.applicationName, "ChaosVR Host64", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 3;
    std::strncpy(ici.applicationInfo.engineName, "ChaosVR", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.engineVersion = 3;
    // ChaosVR currently uses only OpenXR 1.0 core APIs plus extensions.
    // Request 1.0 explicitly for maximum runtime compatibility even when built
    // against newer 1.1 headers/loaders.
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    std::cout << "[OpenXR] requesting API 1.0 with " << enabled.size() << " extensions\n";
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();
    XrCheck(xrCreateInstance(&ici, &instance_), "xrCreateInstance");

    if (HasExtension(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME)) {
        PFN_xrVoidFunction fn{};
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance_, "xrConvertWin32PerformanceCounterToTimeKHR", &fn)))
            pfnConvertQpcToTime_ = reinterpret_cast<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(fn);
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrCheck(xrGetSystem(instance_, &sgi, &systemId_), "xrGetSystem");

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    XrCheck(xrGetSystemProperties(instance_, systemId_, &props), "xrGetSystemProperties");
    maxLayerCount_ = props.graphicsProperties.maxLayerCount;
    if (maxLayerCount_ < 2) throw std::runtime_error("OpenXR runtime exposes fewer than 2 composition layers");
    ChooseEnvironmentBlendMode();

    XrCheck(xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetD3D11Requirements_)), "xrGetInstanceProcAddr(D3D11 requirements)");
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XrCheck(pfnGetD3D11Requirements_(instance_, systemId_, &req), "xrGetD3D11GraphicsRequirementsKHR");
    CreateD3D11Device(req);
    LogRuntimeDiagnostics(props, actualFeatureLevel_);

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = systemId_;
    XrCheck(xrCreateSession(instance_, &sci, &session_), "xrCreateSession");

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrCheck(xrCreateReferenceSpace(session_, &rs, &localSpace_), "xrCreateReferenceSpace(LOCAL)");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrCheck(xrCreateReferenceSpace(session_, &rs, &viewSpace_), "xrCreateReferenceSpace(VIEW)");

    CreateActions();
}

void OpenXrContext::CreateD3D11Device(const XrGraphicsRequirementsD3D11KHR& req) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("CreateDXGIFactory1 failed");
    if (FAILED(factory->EnumAdapterByLuid(req.adapterLuid, IID_PPV_ARGS(&adapter_))))
        throw std::runtime_error("OpenXR-required DXGI adapter was not found");

    const D3D_FEATURE_LEVEL candidates[] = {
#ifdef D3D_FEATURE_LEVEL_12_2
        D3D_FEATURE_LEVEL_12_2,
#endif
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    std::vector<D3D_FEATURE_LEVEL> levels;
    for (auto l : candidates) if (l >= req.minFeatureLevel) levels.push_back(l);
    if (levels.empty()) levels.push_back(req.minFeatureLevel);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
        &device_, &actual, &context_);
#ifndef NDEBUG
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
            &device_, &actual, &context_);
    }
#endif
    if (FAILED(hr) || actual < req.minFeatureLevel)
        throw std::runtime_error("D3D11CreateDevice did not satisfy OpenXR graphics requirements");
    actualFeatureLevel_ = actual;
}

void OpenXrContext::CreateActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    asci.priority = 0;
    XrCheck(xrCreateActionSet(instance_, &asci, &actionSet_), "xrCreateActionSet");

    auto makeAction = [&](XrActionType type, const char* name, const char* localized, XrAction& out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        std::strcpy(aci.actionName, name);
        std::strcpy(aci.localizedActionName, localized);
        XrCheck(xrCreateAction(actionSet_, &aci, &out), "xrCreateAction");
    };
    makeAction(XR_ACTION_TYPE_POSE_INPUT, "left_grip_pose", "Left Grip Pose", leftGripPoseAction_);
    makeAction(XR_ACTION_TYPE_POSE_INPUT, "right_grip_pose", "Right Grip Pose", rightGripPoseAction_);
    makeAction(XR_ACTION_TYPE_POSE_INPUT, "left_aim", "Left Aim", leftAimAction_);
    makeAction(XR_ACTION_TYPE_POSE_INPUT, "right_aim", "Right Aim", rightAimAction_);
    makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Left Trigger", leftTriggerAction_);
    makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "Right Trigger", rightTriggerAction_);
    makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Left Grip", leftGripAction_);
    makeAction(XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "Right Grip", rightGripAction_);
    makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Left Stick", leftStickAction_);
    makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Right Stick", rightStickAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "A Button", buttonAAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "B Button", buttonBAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "X Button", buttonXAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Y Button", buttonYAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Left Stick Click", leftStickClickAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "right_stick_click", "Right Stick Click", rightStickClickAction_);
    makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu Button", menuAction_);

    XrActionSpaceCreateInfo sp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    sp.poseInActionSpace.orientation.w = 1.0f;
    sp.action = leftGripPoseAction_; XrCheck(xrCreateActionSpace(session_, &sp, &leftGripSpace_), "xrCreateActionSpace(left grip)");
    sp.action = rightGripPoseAction_; XrCheck(xrCreateActionSpace(session_, &sp, &rightGripSpace_), "xrCreateActionSpace(right grip)");
    sp.action = leftAimAction_; XrCheck(xrCreateActionSpace(session_, &sp, &leftAimSpace_), "xrCreateActionSpace(left aim)");
    sp.action = rightAimAction_; XrCheck(xrCreateActionSpace(session_, &sp, &rightAimSpace_), "xrCreateActionSpace(right aim)");

    std::vector<XrActionSuggestedBinding> bindings = {
        {leftGripPoseAction_, ToPath(instance_, "/user/hand/left/input/grip/pose")},
        {rightGripPoseAction_, ToPath(instance_, "/user/hand/right/input/grip/pose")},
        {leftAimAction_, ToPath(instance_, "/user/hand/left/input/aim/pose")},
        {rightAimAction_, ToPath(instance_, "/user/hand/right/input/aim/pose")},
        {leftTriggerAction_, ToPath(instance_, "/user/hand/left/input/trigger/value")},
        {rightTriggerAction_, ToPath(instance_, "/user/hand/right/input/trigger/value")},
        {leftGripAction_, ToPath(instance_, "/user/hand/left/input/squeeze/value")},
        {rightGripAction_, ToPath(instance_, "/user/hand/right/input/squeeze/value")},
        {leftStickAction_, ToPath(instance_, "/user/hand/left/input/thumbstick")},
        {rightStickAction_, ToPath(instance_, "/user/hand/right/input/thumbstick")},
        {buttonAAction_, ToPath(instance_, "/user/hand/right/input/a/click")},
        {buttonBAction_, ToPath(instance_, "/user/hand/right/input/b/click")},
        {buttonXAction_, ToPath(instance_, "/user/hand/left/input/x/click")},
        {buttonYAction_, ToPath(instance_, "/user/hand/left/input/y/click")},
        {leftStickClickAction_, ToPath(instance_, "/user/hand/left/input/thumbstick/click")},
        {rightStickClickAction_, ToPath(instance_, "/user/hand/right/input/thumbstick/click")},
        {menuAction_, ToPath(instance_, "/user/hand/left/input/menu/click")},
    };
    auto suggest = [&](const char* profilePath) {
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = ToPath(instance_, profilePath);
        suggestion.suggestedBindings = bindings.data();
        suggestion.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        // Unsupported profiles are harmless: a runtime selects the best profile it implements.
        xrSuggestInteractionProfileBindings(instance_, &suggestion);
    };
    suggest("/interaction_profiles/oculus/touch_controller");
    // Quest 3 ships Touch Plus; OpenXR 1.1 provides a dedicated profile with the same core paths.
    suggest("/interaction_profiles/meta/touch_plus_controller");

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1; attach.actionSets = &actionSet_;
    XrCheck(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets");
}

bool OpenXrContext::PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) return false;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            sessionState_ = changed->state;
            sessionFocused_ = sessionState_ == XR_SESSION_STATE_FOCUSED;
            if (sessionState_ == XR_SESSION_STATE_READY && !sessionRunning_) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrCheck(xrBeginSession(session_, &bi), "xrBeginSession");
                sessionRunning_ = true;
            } else if (sessionState_ == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                XrCheck(xrEndSession(session_), "xrEndSession");
                sessionRunning_ = false;
            } else if (sessionState_ == XR_SESSION_STATE_EXITING || sessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
                return false;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool OpenXrContext::BeginFrame(XrFrameState& state) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrCheck(xrWaitFrame(session_, &wi, &state), "xrWaitFrame");
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XrCheck(xrBeginFrame(session_, &bi), "xrBeginFrame");
    return state.shouldRender == XR_TRUE;
}

bool OpenXrContext::LocateStereoViews(XrTime time, std::array<XrView, 2>& views, XrViewStateFlags& flags) {
    for (auto& v : views) v = XrView{XR_TYPE_VIEW};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = time;
    li.space = localSpace_;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count{};
    const XrResult r = xrLocateViews(session_, &li, &state,
                                     static_cast<uint32_t>(views.size()), &count, views.data());
    flags = state.viewStateFlags;
    if (XR_FAILED(r) || count != 2) return false;
    const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    return (flags & needed) == needed;
}

TrackingFrame OpenXrContext::LocateTracking(XrTime predictedDisplayTime) {
    TrackingFrame out{}; out.predictedDisplayTime = predictedDisplayTime;
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets = 1; sync.activeActionSets = &active;
    XrCheck(xrSyncActions(session_, &sync), "xrSyncActions");

    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, predictedDisplayTime, &head))) {
        const auto good = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        out.headValid = (head.locationFlags & good) == good;
        if (out.headValid) out.head = head.pose;
    }
    auto locate = [&](XrSpace space, XrPosef& pose, bool& valid,
                      XrVector3f* linear, bool* linearValid,
                      XrVector3f* angular, bool* angularValid) {
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION}; loc.next = &vel;
        if (XR_SUCCEEDED(xrLocateSpace(space, localSpace_, predictedDisplayTime, &loc))) {
            const auto good = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            valid = (loc.locationFlags & good) == good;
            if (valid) pose = loc.pose;
            if (linear && linearValid) {
                *linearValid = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
                if (*linearValid) *linear = vel.linearVelocity;
            }
            if (angular && angularValid) {
                *angularValid = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
                if (*angularValid) *angular = vel.angularVelocity;
            }
        }
    };
    locate(leftGripSpace_, out.leftGrip, out.leftGripValid,
           &out.leftLinearVelocity, &out.leftLinearVelocityValid,
           &out.leftAngularVelocity, &out.leftAngularVelocityValid);
    locate(rightGripSpace_, out.rightGrip, out.rightGripValid,
           &out.rightLinearVelocity, &out.rightLinearVelocityValid,
           &out.rightAngularVelocity, &out.rightAngularVelocityValid);
    locate(leftAimSpace_, out.leftAim, out.leftAimValid, nullptr, nullptr, nullptr, nullptr);
    locate(rightAimSpace_, out.rightAim, out.rightAimValid, nullptr, nullptr, nullptr, nullptr);

    auto f = [&](XrAction a) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &s)) && s.isActive) return s.currentState;
        return 0.0f;
    };
    auto v2 = [&](XrAction a) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &s)) && s.isActive) return s.currentState;
        return XrVector2f{};
    };
    auto b = [&](XrAction a) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive && s.currentState;
    };
    out.leftTrigger=f(leftTriggerAction_); out.rightTrigger=f(rightTriggerAction_);
    out.leftSqueeze=f(leftGripAction_); out.rightSqueeze=f(rightGripAction_);
    out.leftStick=v2(leftStickAction_); out.rightStick=v2(rightStickAction_);
    if (b(buttonAAction_)) out.buttons |= ChaosVrButtons::A;
    if (b(buttonBAction_)) out.buttons |= ChaosVrButtons::B;
    if (b(buttonXAction_)) out.buttons |= ChaosVrButtons::X;
    if (b(buttonYAction_)) out.buttons |= ChaosVrButtons::Y;
    if (b(leftStickClickAction_)) out.buttons |= ChaosVrButtons::LeftStick;
    if (b(rightStickClickAction_)) out.buttons |= ChaosVrButtons::RightStick;
    if (b(menuAction_)) out.buttons |= ChaosVrButtons::Menu;
    return out;
}

bool OpenXrContext::CopyCompatible(DXGI_FORMAT source, DXGI_FORMAT dest) noexcept {
    if (source == dest) return true;
    auto bgra8 = [](DXGI_FORMAT f) {
        return f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
               f == DXGI_FORMAT_B8G8R8A8_UNORM ||
               f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
    auto rgba8 = [](DXGI_FORMAT f) {
        return f == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM ||
               f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
    return (bgra8(source) && bgra8(dest)) || (rgba8(source) && rgba8(dest));
}

int64_t OpenXrContext::ChooseSwapchainFormat(DXGI_FORMAT sourceFormat) {
    uint32_t n{};
    XrCheck(xrEnumerateSwapchainFormats(session_, 0, &n, nullptr), "xrEnumerateSwapchainFormats(count)");
    std::vector<int64_t> formats(n);
    XrCheck(xrEnumerateSwapchainFormats(session_, n, &n, formats.data()), "xrEnumerateSwapchainFormats");
    if (formats.empty()) throw std::runtime_error("OpenXR runtime exposed no swapchain formats");

    auto has = [&](DXGI_FORMAT f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };

    // WGC captures ordinary SDR desktop/window pixels in BGRA UNORM. Prefer an
    // sRGB swapchain when possible so the XR compositor knows the bytes are
    // non-linear display-encoded and performs the correct sRGB->linear sampling.
    // D3D11 permits copies between UNORM and UNORM_SRGB within the same typeless group.
    if (sourceFormat == DXGI_FORMAT_B8G8R8A8_UNORM && has(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (sourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM && has(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (has(sourceFormat)) return static_cast<int64_t>(sourceFormat);

    const DXGI_FORMAT preferred[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM
    };
    for (auto p : preferred) {
        if (has(p) && CopyCompatible(sourceFormat, p)) return static_cast<int64_t>(p);
    }
    throw std::runtime_error("OpenXR runtime exposes no copy-compatible 8-bit swapchain format");
}

void OpenXrContext::EnsureStereoSwapchain(uint32_t width, uint32_t height, DXGI_FORMAT sourceFormat) {
    if (stereoSwapchain_ && width == stereoWidth_ && height == stereoHeight_ &&
        sourceFormat == stereoSourceFormat_) return;
    DestroySwapchain();
    stereoFormat_ = ChooseSwapchainFormat(sourceFormat);
    stereoSourceFormat_ = sourceFormat;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = stereoFormat_; ci.sampleCount = 1; ci.width = width; ci.height = height;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrCheck(xrCreateSwapchain(session_, &ci, &stereoSwapchain_), "xrCreateSwapchain");
    uint32_t n{};
    XrCheck(xrEnumerateSwapchainImages(stereoSwapchain_, 0, &n, nullptr), "xrEnumerateSwapchainImages(count)");
    stereoImages_.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XrCheck(xrEnumerateSwapchainImages(stereoSwapchain_, n, &n,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(stereoImages_.data())), "xrEnumerateSwapchainImages");
    stereoWidth_ = width; stereoHeight_ = height;
    std::cout << "[XR] stereo swapchain " << width << "x" << height
              << " source DXGI=" << static_cast<int>(sourceFormat)
              << " XR DXGI=" << stereoFormat_ << " images=" << n << "\n";
}

bool OpenXrContext::CopyCapturedFrameToSwapchain(ID3D11Texture2D* source, const RECT& sourceRect, uint32_t& acquiredIndex) {
    if (!stereoSwapchain_ || !source) return false;
    D3D11_TEXTURE2D_DESC srcDesc{}; source->GetDesc(&srcDesc);
    const auto destFormat = static_cast<DXGI_FORMAT>(stereoFormat_);
    if (!CopyCompatible(srcDesc.Format, destFormat)) return false;
    const UINT copyWidth = static_cast<UINT>(sourceRect.right - sourceRect.left);
    const UINT copyHeight = static_cast<UINT>(sourceRect.bottom - sourceRect.top);
    if (copyWidth != stereoWidth_ || copyHeight != stereoHeight_) return false;

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrCheck(xrAcquireSwapchainImage(stereoSwapchain_, &ai, &acquiredIndex), "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    XrCheck(xrWaitSwapchainImage(stereoSwapchain_, &wi), "xrWaitSwapchainImage");

    D3D11_BOX box{static_cast<UINT>(sourceRect.left), static_cast<UINT>(sourceRect.top), 0,
                  static_cast<UINT>(sourceRect.right), static_cast<UINT>(sourceRect.bottom), 1};
    context_->CopySubresourceRegion(stereoImages_.at(acquiredIndex).texture, 0, 0, 0, 0, source, 0, &box);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrCheck(xrReleaseSwapchainImage(stereoSwapchain_, &ri), "xrReleaseSwapchainImage");
    return true;
}

bool OpenXrContext::FillStereoTestPattern(uint32_t& acquiredIndex) {
    if (!stereoSwapchain_ || stereoWidth_ < 4 || stereoHeight_ < 2) return false;
    const auto fmt = static_cast<DXGI_FORMAT>(stereoFormat_);
    const bool rgba = fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
                      fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
                      fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!rgba && !bgra) return false;

    auto pack = [rgba](uint8_t r, uint8_t g, uint8_t b, uint8_t a) -> uint32_t {
        if (rgba) return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
        return uint32_t(b) | (uint32_t(g) << 8) | (uint32_t(r) << 16) | (uint32_t(a) << 24);
    };

    std::vector<uint32_t> pixels(size_t(stereoWidth_) * size_t(stereoHeight_));
    const uint32_t half = stereoWidth_ / 2u;
    for (uint32_t y = 0; y < stereoHeight_; ++y) {
        for (uint32_t x = 0; x < stereoWidth_; ++x) {
            const bool left = x < half;
            const uint32_t localX = left ? x : x - half;
            const uint32_t eyeW = left ? half : (stereoWidth_ - half);
            const bool checker = ((localX / 64u) ^ (y / 64u)) & 1u;
            const bool centerV = localX > eyeW / 2u - 4u && localX < eyeW / 2u + 4u;
            const bool centerH = y > stereoHeight_ / 2u - 4u && y < stereoHeight_ / 2u + 4u;
            const bool border = localX < 8u || localX + 8u >= eyeW || y < 8u || y + 8u >= stereoHeight_;
            uint8_t r = 0, g = 0, b = 0;
            if (left) {
                r = checker ? 220 : 90; g = checker ? 35 : 10; b = checker ? 35 : 10;
            } else {
                r = checker ? 35 : 10; g = checker ? 220 : 90; b = checker ? 35 : 10;
            }
            if (centerV || centerH || border) r = g = b = 255;
            pixels[size_t(y) * stereoWidth_ + x] = pack(r, g, b, 255);
        }
    }

    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrCheck(xrAcquireSwapchainImage(stereoSwapchain_, &ai, &acquiredIndex), "xrAcquireSwapchainImage(test-pattern)");
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    XrCheck(xrWaitSwapchainImage(stereoSwapchain_, &wi), "xrWaitSwapchainImage(test-pattern)");
    context_->UpdateSubresource(stereoImages_.at(acquiredIndex).texture, 0, nullptr,
                                pixels.data(), stereoWidth_ * sizeof(uint32_t), 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrCheck(xrReleaseSwapchainImage(stereoSwapchain_, &ri), "xrReleaseSwapchainImage(test-pattern)");
    std::cout << "[XR] synthetic SBS test pattern uploaded: left=red, right=green\n";
    return true;
}

void OpenXrContext::ChooseEnvironmentBlendMode() {
    uint32_t count{};
    XrCheck(xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              0, &count, nullptr),
            "xrEnumerateEnvironmentBlendModes(count)");
    std::vector<XrEnvironmentBlendMode> modes(count);
    XrCheck(xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              count, &count, modes.data()),
            "xrEnumerateEnvironmentBlendModes");
    if (modes.empty()) throw std::runtime_error("OpenXR runtime exposed no environment blend modes");
    auto it = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    environmentBlendMode_ = it != modes.end() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes.front();
}

void OpenXrContext::LogRuntimeDiagnostics(const XrSystemProperties& props, D3D_FEATURE_LEVEL actualFeatureLevel) {
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip))) {
        std::cout << "[XR] runtime: " << ip.runtimeName << " "
                  << XR_VERSION_MAJOR(ip.runtimeVersion) << "."
                  << XR_VERSION_MINOR(ip.runtimeVersion) << "."
                  << XR_VERSION_PATCH(ip.runtimeVersion) << "\n";
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter_ && SUCCEEDED(adapter_->GetDesc1(&ad))) {
        std::wcout << L"[XR] adapter: " << ad.Description << L"\n";
    }
    std::cout << "[XR] system: " << props.systemName
              << " maxLayers=" << props.graphicsProperties.maxLayerCount
              << " maxSwapchain=" << props.graphicsProperties.maxSwapchainImageWidth << "x"
              << props.graphicsProperties.maxSwapchainImageHeight
              << " featureLevel=0x" << std::hex << static_cast<unsigned>(actualFeatureLevel)
              << std::dec << " qpcConversion=" << (pfnConvertQpcToTime_ ? "yes" : "no") << "\n";
}

bool OpenXrContext::TryConvertQpc100nsToXrTime(int64_t qpcTime100ns, XrTime& out) const noexcept {
    if (!pfnConvertQpcToTime_) return false;
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return false;
    constexpr int64_t kTicksPerSecond = 10'000'000;
    const int64_t sec = qpcTime100ns / kTicksPerSecond;
    const int64_t rem = qpcTime100ns % kTicksPerSecond;
    LARGE_INTEGER qpc{};
    // Split the operation to avoid overflowing int64 after long uptimes.
    qpc.QuadPart = sec * freq.QuadPart + (rem * freq.QuadPart) / kTicksPerSecond;
    return XR_SUCCEEDED(pfnConvertQpcToTime_(instance_, &qpc, &out));
}

void OpenXrContext::EndFrame(XrTime displayTime, const XrCompositionLayerBaseHeader* const* layers, uint32_t layerCount) {
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime; ei.environmentBlendMode = environmentBlendMode_;
    ei.layerCount = layerCount; ei.layers = layers;
    XrCheck(xrEndFrame(session_, &ei), "xrEndFrame");
}

void OpenXrContext::DestroySwapchain() {
    stereoImages_.clear();
    if (stereoSwapchain_) { xrDestroySwapchain(stereoSwapchain_); stereoSwapchain_=XR_NULL_HANDLE; }
    stereoWidth_=stereoHeight_=0; stereoFormat_=0; stereoSourceFormat_=DXGI_FORMAT_UNKNOWN;
}

void OpenXrContext::Shutdown() {
    DestroySwapchain();
    if (leftGripSpace_) xrDestroySpace(leftGripSpace_), leftGripSpace_=XR_NULL_HANDLE;
    if (rightGripSpace_) xrDestroySpace(rightGripSpace_), rightGripSpace_=XR_NULL_HANDLE;
    if (leftAimSpace_) xrDestroySpace(leftAimSpace_), leftAimSpace_=XR_NULL_HANDLE;
    if (rightAimSpace_) xrDestroySpace(rightAimSpace_), rightAimSpace_=XR_NULL_HANDLE;
    if (actionSet_) xrDestroyActionSet(actionSet_), actionSet_=XR_NULL_HANDLE;
    if (viewSpace_) xrDestroySpace(viewSpace_), viewSpace_=XR_NULL_HANDLE;
    if (localSpace_) xrDestroySpace(localSpace_), localSpace_=XR_NULL_HANDLE;
    if (session_) xrDestroySession(session_), session_=XR_NULL_HANDLE;
    context_.Reset(); device_.Reset(); adapter_.Reset();
    if (instance_) xrDestroyInstance(instance_), instance_=XR_NULL_HANDLE;
    sessionRunning_=sessionFocused_=false;
}
}