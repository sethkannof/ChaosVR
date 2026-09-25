#include "ChaosVrHost/WgcCapture.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <stdexcept>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace chaosvr::host {
WgcCapture::~WgcCapture() { Stop(); }

wd3d11::IDirect3DDevice WgcCapture::WrapD3D11Device(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    return inspectable.as<wd3d11::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem WgcCapture::CreateItemForWindow(HWND hwnd) {
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        hwnd,
        winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

void WgcCapture::Start(HWND hwnd, ID3D11Device* d3dDevice, CaptureMailbox& mailbox) {
    Stop();
    if (!wgc::GraphicsCaptureSession::IsSupported())
        throw std::runtime_error("Windows Graphics Capture is not supported");

    mailbox_ = &mailbox;
    targetClosed_.store(false);
    item_ = CreateItemForWindow(hwnd);
    closedToken_ = item_.Closed([this](auto const&, auto const&) {
        targetClosed_.store(true);
        running_.store(false);
        if (mailbox_) mailbox_->Clear();
    });
    winrtDevice_ = WrapD3D11Device(d3dDevice);
    auto size = item_.Size();
    poolSize_ = size;
    if (size.Width <= 0 || size.Height <= 0)
        throw std::runtime_error("Capture target has invalid size");

    pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice_, pixelFormat_, 3, size);
    session_ = pool_.CreateCaptureSession(item_);
    // The system pointer is not part of the game render and becomes very distracting
    // in a head-mounted presentation. Treat suppression as optional: capture itself
    // is more important than failing startup if an older API surface rejects it.
    try { session_.IsCursorCaptureEnabled(false); } catch (const winrt::hresult_error&) {}

    frameToken_ = pool_.FrameArrived([this](auto const& sender, auto const&) {
        // Drain to the newest available frame. Older frames are immediately returned
        // to the pool; the newest complete frame remains checked out in the mailbox.
        wgc::Direct3D11CaptureFrame newest{nullptr};
        for (;;) {
            auto frame = sender.TryGetNextFrame();
            if (!frame) break;
            newest = std::move(frame);
        }
        if (!newest) return;

        const auto content = newest.ContentSize();
        if (content.Width > 0 && content.Height > 0 &&
            (content.Width != poolSize_.Width || content.Height != poolSize_.Height)) {
            // Return all checked-out surfaces before rebuilding the pool; Microsoft
            // documents ContentSize as the resize signal and Recreate as the remedy.
            if (mailbox_) mailbox_->Clear();
            newest.Close();
            newest = nullptr;
            poolSize_ = content;
            sender.Recreate(winrtDevice_, pixelFormat_, 3, poolSize_);
            return;
        }
        if (mailbox_) mailbox_->Publish(std::move(newest));
    });

    session_.StartCapture();
    running_.store(true);
}

void WgcCapture::Stop() {
    running_.store(false);
    if (pool_ && frameToken_.value) {
        pool_.FrameArrived(frameToken_);
        frameToken_ = {};
    }
    if (session_) { session_.Close(); session_ = nullptr; }
    if (pool_) { pool_.Close(); pool_ = nullptr; }
    if (item_ && closedToken_.value) {
        item_.Closed(closedToken_);
        closedToken_ = {};
    }
    item_ = nullptr;
    winrtDevice_ = nullptr;
    poolSize_ = {};
    if (mailbox_) mailbox_->Clear();
    mailbox_ = nullptr;
    targetClosed_.store(false);
}
}