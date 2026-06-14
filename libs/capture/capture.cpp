#include "capture.h"

// Suppress macro redefinition warnings from Windows/WinRT headers
#pragma warning(push)
#pragma warning(disable: 4005)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#pragma warning(pop)

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include <atomic>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace capture {

// ── Impl ──────────────────────────────────────────────────────────────────────

struct DisplayCapture::Impl {
    winrt::com_ptr<ID3D11Device>          d3d_device;
    winrt::com_ptr<ID3D11DeviceContext>   d3d_ctx;
    winrt::com_ptr<ID3D11Texture2D>       staging_tex;
    uint32_t                              staging_w = 0;
    uint32_t                              staging_h = 0;

    wgdx::Direct3D11::IDirect3DDevice     winrt_device{ nullptr };
    wgc::Direct3D11CaptureFramePool       pool{ nullptr };
    wgc::GraphicsCaptureSession           session{ nullptr };
    winrt::event_token                    frame_token{};
    winrt::Windows::Graphics::SizeInt32   last_size{};

    std::atomic<bool>     active{ false };
    std::atomic<bool>     border_suppressed{ false };
    std::atomic<uint32_t> seq{ 0 };
    FrameCallback         cb;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

bool is_supported()
{
    return wgc::GraphicsCaptureSession::IsSupported();
}

static winrt::com_ptr<ID3D11Device> make_d3d_device()
{
    winrt::com_ptr<ID3D11Device> dev;
    D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        dev.put(), nullptr, nullptr);
    return dev;
}

static wgdx::Direct3D11::IDirect3DDevice
wrap_d3d(winrt::com_ptr<ID3D11Device> const& d3d)
{
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> insp;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
    return insp.as<wgdx::Direct3D11::IDirect3DDevice>();
}

static wgc::GraphicsCaptureItem item_from_monitor(HMONITOR hmon)
{
    // IGraphicsCaptureItem default interface GUID
    static constexpr GUID kItemGuid =
        { 0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60} };

    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(factory->CreateForMonitor(hmon, kItemGuid, winrt::put_abi(item)));
    return item;
}

// ── DisplayCapture ────────────────────────────────────────────────────────────

DisplayCapture::DisplayCapture() : impl_(new Impl()) {}
DisplayCapture::~DisplayCapture() { stop(); delete impl_; }
bool DisplayCapture::running() const { return impl_->active.load(std::memory_order_relaxed); }
bool DisplayCapture::border_suppressed() const { return impl_->border_suppressed.load(std::memory_order_relaxed); }

bool DisplayCapture::start(HMONITOR hmon, FrameCallback cb, bool show_border)
{
    if (impl_->active) return false;
    if (!is_supported()) return false;

    impl_->cb         = std::move(cb);
    impl_->d3d_device = make_d3d_device();
    if (!impl_->d3d_device) return false;

    // Allow context access from any thread (WGC delivers on the thread pool).
    winrt::com_ptr<ID3D11Multithread> mt;
    impl_->d3d_device->QueryInterface(IID_PPV_ARGS(mt.put()));
    if (mt) mt->SetMultithreadProtected(TRUE);

    impl_->d3d_device->GetImmediateContext(impl_->d3d_ctx.put());

    try {
        impl_->winrt_device = wrap_d3d(impl_->d3d_device);
        auto item           = item_from_monitor(hmon);
        impl_->last_size    = item.Size();

        // CreateFreeThreaded: callbacks arrive on the WinRT thread pool regardless
        // of the calling thread's apartment — required for Win32 desktop apps.
        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->winrt_device,
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,              // double-buffer
            impl_->last_size);

        // Remove WGC's default frame-rate throttle (~15 FPS) by setting the
        // minimum update interval to 1 ms — the application-side pacing in
        // main.cpp handles throttling/duplication to match the configured FPS.
        impl_->pool.MinUpdateInterval(
            std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                std::chrono::milliseconds(1)));

        impl_->active = true;

        impl_->frame_token = impl_->pool.FrameArrived(
            [this](wgc::Direct3D11CaptureFramePool const& pool, auto const&)
            {
                if (!impl_->active.load(std::memory_order_acquire)) return;

                auto frame = pool.TryGetNextFrame();
                if (!frame) return;

                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);

                auto sz = frame.ContentSize();

                if (sz.Width  != impl_->last_size.Width ||
                    sz.Height != impl_->last_size.Height) {
                    impl_->last_size    = sz;
                    impl_->staging_tex  = nullptr; // force recreate below
                    pool.Recreate(
                        impl_->winrt_device,
                        wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        2, sz);
                }

                FrameInfo fi{};
                fi.timestamp_qpc = qpc.QuadPart;
                fi.width         = static_cast<uint32_t>(sz.Width);
                fi.height        = static_cast<uint32_t>(sz.Height);
                fi.seq           = impl_->seq.fetch_add(1, std::memory_order_relaxed);
                fi.bgra_data     = nullptr;
                fi.bgra_stride   = 0;

                // Get the GPU texture from this frame.
                auto surface = frame.Surface();
                auto access  = surface.try_as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> gpu_tex;
                if (access && SUCCEEDED(access->GetInterface(IID_PPV_ARGS(gpu_tex.put())))) {

                    // Create / recreate staging texture when dimensions change.
                    if (!impl_->staging_tex ||
                        impl_->staging_w != fi.width ||
                        impl_->staging_h != fi.height) {
                        D3D11_TEXTURE2D_DESC d{};
                        d.Width            = fi.width;
                        d.Height           = fi.height;
                        d.MipLevels        = 1;
                        d.ArraySize        = 1;
                        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                        d.SampleDesc.Count = 1;
                        d.Usage            = D3D11_USAGE_STAGING;
                        d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
                        impl_->staging_tex = nullptr;
                        impl_->d3d_device->CreateTexture2D(&d, nullptr, impl_->staging_tex.put());
                        impl_->staging_w = fi.width;
                        impl_->staging_h = fi.height;
                    }

                    if (impl_->staging_tex) {
                        impl_->d3d_ctx->CopyResource(impl_->staging_tex.get(), gpu_tex.get());

                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(impl_->d3d_ctx->Map(
                                impl_->staging_tex.get(), 0,
                                D3D11_MAP_READ, 0, &mapped))) {
                            fi.bgra_data   = static_cast<const uint8_t*>(mapped.pData);
                            fi.bgra_stride = mapped.RowPitch;
                            if (impl_->cb) impl_->cb(fi);
                            impl_->d3d_ctx->Unmap(impl_->staging_tex.get(), 0);
                        }
                    }
                }

                // Fall through: call cb even if readback failed (bgra_data = nullptr).
                if (fi.bgra_data == nullptr && impl_->cb) impl_->cb(fi);

                frame.Close();
            });

        impl_->session = impl_->pool.CreateCaptureSession(item);
        impl_->border_suppressed = false;
        if (!show_border) {
            try {
                impl_->session.IsBorderRequired(false);
                impl_->border_suppressed = true;
            } catch (...) {
                // Capture must continue even when the OS denies border suppression.
            }
        }
        impl_->session.StartCapture();
    }
    catch (...) {
        impl_->active = false;
        impl_->cb     = nullptr;
        impl_->d3d_device = nullptr;
        return false;
    }

    return true;
}

void DisplayCapture::stop()
{
    if (!impl_->active) return;

    // Signal the callback to abort immediately on next invocation.
    impl_->active.store(false, std::memory_order_release);

    // Give any in-flight callback time to see active=false and return.
    // Spike-quality sync: 30ms >> max callback duration at 60fps (16ms).
    Sleep(30);

    // Revoke the handler — no new invocations after this returns.
    impl_->pool.FrameArrived(impl_->frame_token);

    if (impl_->session) { impl_->session.Close(); impl_->session = nullptr; }
    if (impl_->pool)    { impl_->pool.Close();    impl_->pool    = nullptr; }

    impl_->staging_tex  = nullptr;
    impl_->d3d_ctx      = nullptr;
    impl_->winrt_device = nullptr;
    impl_->d3d_device   = nullptr;
    impl_->cb           = nullptr;
    impl_->staging_w    = 0;
    impl_->staging_h    = 0;
    impl_->border_suppressed.store(false, std::memory_order_relaxed);
    impl_->seq.store(0, std::memory_order_relaxed);
}

} // namespace capture
