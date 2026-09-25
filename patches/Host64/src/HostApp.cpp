#include "ChaosVrHost/HostApp.h"
#include "Protocol/PoseConvention.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include "Core/PresentCorrelationGuard.h"

namespace chaosvr::host {
namespace {
struct FindWindowCtx { HWND found{}; uint64_t bestArea{}; };
BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid{}; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    const bool match = QueryFullProcessImageNameW(p, 0, path, &n) &&
        (_wcsicmp(wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path, L"splintercell3.exe") == 0);
    CloseHandle(p);
    if (!match) return TRUE;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const auto width = static_cast<uint64_t>(std::max<LONG>(0, rc.right - rc.left));
    const auto height = static_cast<uint64_t>(std::max<LONG>(0, rc.bottom - rc.top));
    const uint64_t area = width * height;
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
    if (area > ctx->bestArea) { ctx->bestArea = area; ctx->found = hwnd; }
    return TRUE;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> SurfaceTexture(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
    return texture;
}

bool CameraSampleToFov(const ChaosVrCameraSampleV1& s, XrFovf& out) {
    // Protocol convention: all four values are positive tangent magnitudes from
    // the optical axis to the corresponding frustum side.
    const float vals[] = {s.fovLeftTan,s.fovRightTan,s.fovUpTan,s.fovDownTan};
    for (float v : vals) if (!(std::isfinite(v) && v > 0.001f && v < 100.0f)) return false;
    out.angleLeft  = -std::atan(s.fovLeftTan);
    out.angleRight =  std::atan(s.fovRightTan);
    out.angleUp    =  std::atan(s.fovUpTan);
    out.angleDown  = -std::atan(s.fovDownTan);
    return true;
}

struct EyeRects {
    XrRect2Di left{}, right{};
    float leftAspect{}, rightAspect{};
};

int64_t Qpc100nsToRaw(int64_t ticks100ns) {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0;
    constexpr int64_t k100nsPerSecond = 10'000'000;
    const int64_t sec = ticks100ns / k100nsPerSecond;
    const int64_t rem = ticks100ns % k100nsPerSecond;
    return sec * freq.QuadPart + (rem * freq.QuadPart) / k100nsPerSecond;
}

double QpcDeltaMs(int64_t newer, int64_t older) {
    LARGE_INTEGER freq{};
    if (newer < older || !QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 1e30;
    return (double(newer - older) * 1000.0) / double(freq.QuadPart);
}

EyeRects MakeEyeRects(SourceLayout layout, uint32_t w, uint32_t h) {
    EyeRects out{};
    const float originalAspect = h ? float(w) / float(h) : 1.0f;
    switch (layout) {
    case SourceLayout::Mono:
        out.left={{0,0},{static_cast<int32_t>(w),static_cast<int32_t>(h)}};
        out.right=out.left;
        out.leftAspect=out.rightAspect=originalAspect;
        break;
    case SourceLayout::TopBottomHalf:
    case SourceLayout::TopBottomFull: {
        const int32_t topH=static_cast<int32_t>(h/2);
        const int32_t bottomH=static_cast<int32_t>(h-h/2);
        out.left={{0,0},{static_cast<int32_t>(w),topH}};
        out.right={{0,topH},{static_cast<int32_t>(w),bottomH}};
        if (layout == SourceLayout::TopBottomHalf) {
            // Half-TAB keeps the original mono frame dimensions and compresses each
            // eye vertically. The display geometry must restore the mono aspect.
            out.leftAspect=out.rightAspect=originalAspect;
        } else {
            out.leftAspect=float(w)/float(topH);
            out.rightAspect=float(w)/float(bottomH);
        }
        break;
    }
    case SourceLayout::SideBySideHalf:
    case SourceLayout::SideBySideFull:
    default: {
        const int32_t leftW=static_cast<int32_t>(w/2);
        const int32_t rightW=static_cast<int32_t>(w-w/2);
        out.left={{0,0},{leftW,static_cast<int32_t>(h)}};
        out.right={{leftW,0},{rightW,static_cast<int32_t>(h)}};
        if (layout == SourceLayout::SideBySideHalf) {
            // Half-SBS (the common stereo-wrapper output) keeps the original mono
            // frame width, so each eye is horizontally squeezed by 2x. Restore the
            // original game aspect instead of using the half-width pixel rectangle.
            out.leftAspect=out.rightAspect=originalAspect;
        } else {
            out.leftAspect=float(leftW)/float(h);
            out.rightAspect=float(rightW)/float(h);
        }
        break;
    }}
    return out;
}

} // namespace

HWND HostApp::FindChaosTheoryWindow() {
    FindWindowCtx ctx; EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx)); return ctx.found;
}

void HostApp::PublishTracking(const TrackingFrame& t) {
    ChaosVrSharedStateV3 s{};
    s.magic=0x33565243u; s.version=3; s.structSize=sizeof(s);
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); s.qpcTicks=qpc.QuadPart;
    s.predictedDisplayTime=t.predictedDisplayTime;
    auto cvt=[](const XrPosef& p){
        ChaosVrPoseF raw{p.position.x,p.position.y,p.position.z,
            p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w};
        return ChaosVrCanonicalFromOpenXr(raw);
    };
    s.head=cvt(t.head); s.left=cvt(t.leftGrip); s.right=cvt(t.rightGrip);
    if(t.headValid)s.flags|=ChaosVrFlags::HeadTracked;
    if(t.leftGripValid)s.flags|=ChaosVrFlags::LeftTracked;
    if(t.rightGripValid)s.flags|=ChaosVrFlags::RightTracked;
    if(xr_.SessionFocused())s.flags|=ChaosVrFlags::SessionFocus;
    s.flags|=ChaosVrFlags::PoseSpaceCanonical;
    s.leftTrigger=t.leftTrigger; s.rightTrigger=t.rightTrigger;
    s.leftGrip=t.leftSqueeze; s.rightGrip=t.rightSqueeze;
    s.leftStickX=t.leftStick.x; s.leftStickY=t.leftStick.y;
    s.rightStickX=t.rightStick.x; s.rightStickY=t.rightStick.y;
    s.buttons=t.buttons;
    s.artificialYawRadians=artificialYaw_;
    s.worldScale=1.0f;
    s.integrityHash=ChaosVrTrackingIntegrityHash(s);
    lastTrackingSnapshot_=s;
    shared_.Publish(s);
}


void HostApp::PublishControllerExtras(const TrackingFrame& t) {
    ChaosVrControllerExtrasV1 e{};
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); e.qpcTicks=qpc.QuadPart;
    auto pose=[](const XrPosef& p){
        ChaosVrPoseF raw{p.position.x,p.position.y,p.position.z,
            p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w};
        return ChaosVrCanonicalFromOpenXr(raw);
    };
    auto lin=[](const XrVector3f& v){ float x,y,z; ChaosVrCanonicalLinearVelocity(v.x,v.y,v.z,x,y,z); return ChaosVrVec3F{x,y,z}; };
    auto ang=[](const XrVector3f& v){ float x,y,z; ChaosVrCanonicalAngularVelocity(v.x,v.y,v.z,x,y,z); return ChaosVrVec3F{x,y,z}; };
    e.left.grip=pose(t.leftGrip); e.left.aim=pose(t.leftAim);
    e.right.grip=pose(t.rightGrip); e.right.aim=pose(t.rightAim);
    e.left.linearVelocity=lin(t.leftLinearVelocity); e.left.angularVelocity=ang(t.leftAngularVelocity);
    e.right.linearVelocity=lin(t.rightLinearVelocity); e.right.angularVelocity=ang(t.rightAngularVelocity);
    if(t.leftGripValid)e.left.flags|=ChaosVrControllerFlags::GripValid;
    if(t.leftAimValid)e.left.flags|=ChaosVrControllerFlags::AimValid;
    if(t.leftLinearVelocityValid)e.left.flags|=ChaosVrControllerFlags::LinearVelValid;
    if(t.leftAngularVelocityValid)e.left.flags|=ChaosVrControllerFlags::AngularVelValid;
    if(t.rightGripValid)e.right.flags|=ChaosVrControllerFlags::GripValid;
    if(t.rightAimValid)e.right.flags|=ChaosVrControllerFlags::AimValid;
    if(t.rightLinearVelocityValid)e.right.flags|=ChaosVrControllerFlags::LinearVelValid;
    if(t.rightAngularVelocityValid)e.right.flags|=ChaosVrControllerFlags::AngularVelValid;
    extras_.Publish(e);
    if (replayRecording_) {
        if (!replayWriter_.Append(lastTrackingSnapshot_, e)) {
            std::cerr << "[replay] write failed; disabling tracking recording.\n";
            replayWriter_.Close(); replayRecording_=false;
        }
    }
}

void HostApp::SubmitCapturedFrame(XrTime displayTime, bool shouldRender) {
    if (!shouldRender) { xr_.EndFrame(displayTime, nullptr, 0); return; }
    auto item = mailbox_.TakeLatest();
    if (item && item->frame) {
        lastCaptureSerial_ = item->serial;
        XrTime sourceXr{};
        if (item->systemRelative100ns > 0 && xr_.TryConvertQpc100nsToXrTime(item->systemRelative100ns, sourceXr))
            lastCaptureXrTime_ = sourceXr;

        // Prefer the exact OpenXR tracking time Bridge32 reports using for the
        // game camera that most recently preceded this captured frame. WGC time
        // remains the fallback until Bridge32 camera timing is live.
        lastSourceTrackingXrTime_ = lastCaptureXrTime_;
        const int64_t captureQpc = Qpc100nsToRaw(item->systemRelative100ns);
        if (!cameraTiming_.IsOpen() && (item->serial % 60u) == 1u) cameraTiming_.Open();
        if (!gameTelemetry_.IsOpen() && (item->serial % 60u) == 1u) gameTelemetry_.Open();
        if (!presentTiming_.IsOpen() && (item->serial % 60u) == 1u) presentTiming_.Open();
        if (captureQpc > 0) {
            // Best path: identify the exact successful D3D9 Present that WGC captured,
            // then select camera/tracking telemetry by that presentId/gameFrame. This
            // avoids the ambiguous "latest camera write before capture" heuristic.
            uint64_t capturedFrameId = 0;
            bool havePresentBracket = false;
            {
                // WGC's compositor timestamp can land very close to a game Present
                // boundary. In that case the exact source Present is not trustworthy
                // enough for late reprojection, so display the frame but decline to
                // claim exact source-pose identity.
                const auto bracket = presentTiming_.FindBracket(captureQpc);
                LARGE_INTEGER qpcFreq{};
                havePresentBracket = bracket.before.has_value();
                if (bracket.before && QueryPerformanceFrequency(&qpcFreq) && qpcFreq.QuadPart > 0) {
                    std::optional<int64_t> afterQpc;
                    if (bracket.after) afterQpc = bracket.after->qpcPresentEnd;
                    const auto confidence = chaosvr::core::EvaluatePresentBoundary(
                        captureQpc, bracket.before->qpcPresentEnd, afterQpc, qpcFreq.QuadPart, 2.5, 100.0);
                    if (confidence.confident) capturedFrameId = bracket.before->presentId;
                }
            }

            // Once Present telemetry exists, HistoricalProjection requires a
            // confident Present->game-camera tracking match. An ambiguous boundary
            // intentionally falls back to diagnostic quads instead of pretending the
            // WGC compositor timestamp was the game's source HMD pose.
            if (havePresentBracket) lastSourceTrackingXrTime_ = 0;

            std::optional<ChaosVrCameraTimingSampleV1> timing;
            std::optional<ChaosVrCameraSampleV1> camera;
            if (capturedFrameId != 0) {
                timing = cameraTiming_.FindByGameFrame(capturedFrameId);
                camera = gameTelemetry_.FindByGameFrame(capturedFrameId);
            }
            // Timestamp-only source tracking is acceptable only when Present telemetry
            // is unavailable. If a Present bracket exists but the boundary guard says
            // it is ambiguous, intentionally withhold source-pose reprojection rather
            // than falling back to a guess. Camera/FOV telemetry may still use its
            // timestamp fallback because it does not drive late head-pose correction.
            if (!timing && !havePresentBracket) timing = cameraTiming_.FindLatestAppliedAtOrBefore(captureQpc);
            if (!camera) camera = gameTelemetry_.FindLatestAppliedAtOrBefore(captureQpc);

            if (timing && timing->sourceTrackingXrTime > 0)
                lastSourceTrackingXrTime_ = timing->sourceTrackingXrTime;
            if (camera) {
                XrFovf f{};
                if (CameraSampleToFov(*camera, f)) { lastGameFov_ = f; haveGameFov_ = true; }
                else haveGameFov_ = false;
            }
        }

        auto frame = std::move(item->frame); // checked out until copy completes
        auto content = frame.ContentSize();
        if (content.Width >= 2 && content.Height >= 1) {
            auto texture = SurfaceTexture(frame.Surface());
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            const uint32_t w = std::min<uint32_t>(desc.Width, static_cast<uint32_t>(content.Width));
            const uint32_t h = std::min<uint32_t>(desc.Height, static_cast<uint32_t>(content.Height));
            xr_.EnsureStereoSwapchain(w, h, desc.Format);
            RECT src{0,0,static_cast<LONG>(w),static_cast<LONG>(h)};
            uint32_t index{};
            haveStereoImage_ = xr_.CopyCapturedFrameToSwapchain(texture.Get(), src, index);
        }
    }
    // OpenXR uses the most recently released image from a swapchain at xrEndFrame.
    // Therefore a 60fps game can feed a 90Hz HMD without re-copying the same desktop
    // frame three times; the last image is simply re-submitted until a newer WGC
    // frame arrives.
    if (!haveStereoImage_ || !xr_.StereoSwapchain()) { xr_.EndFrame(displayTime, nullptr, 0); return; }

    if (lastCaptureXrTime_ > 0 && displayTime > lastCaptureXrTime_) {
        constexpr double kNsPerMs = 1'000'000.0;
        const double ageMs = double(displayTime - lastCaptureXrTime_) / kNsPerMs;
        if (config_.logFrameAge && ((++xrFrameCounter_ % 180u) == 0u || ageMs > 100.0)) {
            std::cout << "[capture] serial=" << lastCaptureSerial_
                      << " predicted-display age=" << std::fixed << std::setprecision(1)
                      << ageMs << " ms\n";
        }
        if (ageMs > config_.maxCapturedFrameAgeMs) {
            // A frozen head-locked world is worse than a blank compositor frame.
            // Do not destroy the swapchain: resume immediately if capture recovers.
            xr_.EndFrame(displayTime, nullptr, 0);
            return;
        }
    }

    const uint32_t w = xr_.StereoWidth();
    const uint32_t h = xr_.StereoHeight();
    EyeRects rects = MakeEyeRects(config_.layout, w, h);
    if (config_.swapEyes) {
        std::swap(rects.left, rects.right);
        std::swap(rects.leftAspect, rects.rightAspect);
    }

    if (config_.presentation == PresentationMode::HistoricalProjection &&
        config_.layout != SourceLayout::Mono && lastSourceTrackingXrTime_ > 0) {
        std::array<XrView, 2> sourceViews{};
        XrViewStateFlags viewFlags{};
        if (xr_.LocateStereoViews(lastSourceTrackingXrTime_, sourceViews, viewFlags)) {
            std::array<XrCompositionLayerProjectionView, 2> pv{
                XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
            };
            pv[0].pose = sourceViews[0].pose;
            pv[0].fov = haveGameFov_ ? lastGameFov_ : sourceViews[0].fov;
            pv[0].subImage.swapchain = xr_.StereoSwapchain();
            pv[0].subImage.imageRect = rects.left;
            pv[1].pose = sourceViews[1].pose;
            pv[1].fov = haveGameFov_ ? lastGameFov_ : sourceViews[1].fov;
            pv[1].subImage.swapchain = xr_.StereoSwapchain();
            pv[1].subImage.imageRect = rects.right;

            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = xr_.LocalSpace();
            projection.viewCount = static_cast<uint32_t>(pv.size());
            projection.views = pv.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)
            };
            xr_.EndFrame(displayTime, layers, 1);
            return;
        }
        // Historical locate may be outside a runtime's retained tracking window.
        // Fall back to the diagnostic eye quads rather than dropping the frame.
    }

    XrCompositionLayerQuad left{XR_TYPE_COMPOSITION_LAYER_QUAD};
    left.space=xr_.ViewSpace(); left.eyeVisibility=XR_EYE_VISIBILITY_LEFT;
    left.subImage.swapchain=xr_.StereoSwapchain(); left.subImage.imageRect=rects.left;
    left.pose.orientation.w=1.0f; left.pose.position.z=-config_.quadDistanceMeters;
    left.size={config_.quadHeightMeters*rects.leftAspect,config_.quadHeightMeters};

    XrCompositionLayerQuad right{XR_TYPE_COMPOSITION_LAYER_QUAD};
    right.space=xr_.ViewSpace(); right.eyeVisibility=XR_EYE_VISIBILITY_RIGHT;
    right.subImage.swapchain=xr_.StereoSwapchain(); right.subImage.imageRect=rects.right;
    right.pose.orientation.w=1.0f; right.pose.position.z=-config_.quadDistanceMeters;
    right.size={config_.quadHeightMeters*rects.rightAspect,config_.quadHeightMeters};

    const XrCompositionLayerBaseHeader* layers[]={
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&left),
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&right)};
    xr_.EndFrame(displayTime,layers,2);
}

int HostApp::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    shared_.Open();
    extras_.Open();
    cameraTiming_.Open(); // optional until Bridge32 camera hook is active
    gameTelemetry_.Open(); // optional until Bridge32 camera hook is active
    presentTiming_.Open(); // optional until Bridge32 D3D9 Present hook is active
    if (!config_.trackingReplayPath.empty()) {
        LARGE_INTEGER freq{};
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0 &&
            replayWriter_.Open(std::filesystem::path(config_.trackingReplayPath), static_cast<uint64_t>(freq.QuadPart))) {
            replayRecording_=true;
            std::wcout << L"[replay] recording canonical tracking to " << config_.trackingReplayPath << L"\n";
        } else {
            std::wcerr << L"[replay] could not open tracking capture: " << config_.trackingReplayPath << L"\n";
        }
    }

    HWND hwnd{};
    if (!config_.testPattern) {
        // Do not seize the OpenXR headset while waiting for the game. This avoids the
        // black-headset behavior seen during the earlier headless-session experiments.
        while (!(hwnd=FindChaosTheoryWindow()))
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    xr_.Initialize();
    if (config_.testPattern) {
        xr_.EnsureStereoSwapchain(2048, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
        uint32_t testIndex{};
        haveStereoImage_ = xr_.FillStereoTestPattern(testIndex);
        if (!haveStereoImage_) throw std::runtime_error("Failed to upload OpenXR stereo test pattern");
        std::cout << "[test] Close one eye at a time: LEFT should be red checker, RIGHT green checker.\n";
    } else {
        capture_.Start(hwnd, xr_.Device(), mailbox_);
    }

    bool running=true;
    while(running) {
        running=xr_.PollEvents();
        if(!running) break;
        if(!config_.testPattern && capture_.TargetClosed()) {
            std::cout << "[capture] Chaos Theory window closed; exiting Host64.\n";
            break;
        }
        if(!xr_.SessionRunning()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        const bool shouldRender=xr_.BeginFrame(fs);
        auto tracking=xr_.LocateTracking(fs.predictedDisplayTime);
        PublishTracking(tracking);
        PublishControllerExtras(tracking);
        SubmitCapturedFrame(fs.predictedDisplayTime,shouldRender);
    }
    return 0;
}
}