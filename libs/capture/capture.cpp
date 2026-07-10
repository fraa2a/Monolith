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

#include <algorithm>
#include <atomic>
#include <utility>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace capture {

// ── Impl ──────────────────────────────────────────────────────────────────────

class FrameGate {
public:
    void reset(int fps, int64_t qpc_freq)
    {
        fps_ = std::max(1, fps);
        qpc_freq_ = qpc_freq;
        next_qpc_ = 0;
    }

    bool should_accept(int64_t now_qpc)
    {
        if (fps_ <= 0 || qpc_freq_ <= 0) return true;
        const int64_t interval = std::max<int64_t>(1, qpc_freq_ / fps_);
        if (next_qpc_ == 0) {
            next_qpc_ = now_qpc + interval;
            return true;
        }
        if (now_qpc < next_qpc_) return false;
        if (now_qpc - next_qpc_ > interval * 2)
            next_qpc_ = now_qpc + interval;
        else
            next_qpc_ += interval;
        return true;
    }

private:
    int fps_ = 60;
    int64_t qpc_freq_ = 0;
    int64_t next_qpc_ = 0;
};

struct DisplayCapture::Impl {
    winrt::com_ptr<ID3D11Device>          d3d_device;
    winrt::com_ptr<ID3D11DeviceContext>   d3d_ctx;
    winrt::com_ptr<ID3D11Texture2D>       staging_tex;
    uint32_t                              staging_w = 0;
    uint32_t                              staging_h = 0;

    // GPU pre-readback downscale (see CaptureOptions::output_width/height).
    // Absent/null when the device doesn't expose D3D11 video support, or
    // while no downscale is configured — the CopyResource(native) path is
    // then used unconditionally, identical to pre-downscale behavior.
    winrt::com_ptr<ID3D11VideoDevice>              video_device;
    winrt::com_ptr<ID3D11VideoContext>             video_ctx;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> vp_enum;
    winrt::com_ptr<ID3D11VideoProcessor>           video_processor;
    winrt::com_ptr<ID3D11Texture2D>                scaled_tex;
    uint32_t                                       vp_src_w = 0;
    uint32_t                                       vp_src_h = 0;
    uint32_t                                       vp_dst_w = 0;
    uint32_t                                       vp_dst_h = 0;
    bool                                            vp_available = false;

    wgdx::Direct3D11::IDirect3DDevice     winrt_device{ nullptr };
    wgc::Direct3D11CaptureFramePool       pool{ nullptr };
    wgc::GraphicsCaptureSession           session{ nullptr };
    winrt::event_token                    frame_token{};
    winrt::Windows::Graphics::SizeInt32   last_size{};

    std::atomic<bool>     active{ false };
    std::atomic<bool>     border_suppressed{ false };
    std::atomic<uint32_t> seq{ 0 };
    std::atomic<uint64_t> frames_arrived{ 0 };
    std::atomic<uint64_t> frames_dropped_before_readback{ 0 };
    std::atomic<uint64_t> frames_readback{ 0 };
    std::atomic<uint64_t> readback_time_us_total{ 0 };
    FrameGate             frame_gate;
    CaptureOptions        options;
    int64_t               qpc_freq = 0;
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

static wgc::GraphicsCaptureItem item_from_window(HWND hwnd)
{
    static constexpr GUID kItemGuid =
        { 0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60} };

    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(factory->CreateForWindow(hwnd, kItemGuid, winrt::put_abi(item)));
    return item;
}

// (Re)creates the video processor and its scaled output texture for the
// given source -> destination size. No-op if already configured for the
// same sizes. Returns false (leaving impl in a "not scaled" state) on any
// failure, so callers fall back to native-resolution CopyResource.
static bool ensure_video_processor(DisplayCapture::Impl* impl,
                                    uint32_t src_w, uint32_t src_h,
                                    uint32_t dst_w, uint32_t dst_h)
{
    if (impl->video_processor && impl->scaled_tex &&
        impl->vp_src_w == src_w && impl->vp_src_h == src_h &&
        impl->vp_dst_w == dst_w && impl->vp_dst_h == dst_h)
        return true;

    impl->video_processor = nullptr;
    impl->vp_enum         = nullptr;
    impl->scaled_tex      = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat         = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator   = 60;
    content.InputFrameRate.Denominator = 1;
    content.InputWidth                = src_w;
    content.InputHeight               = src_h;
    content.OutputFrameRate.Numerator   = 60;
    content.OutputFrameRate.Denominator = 1;
    content.OutputWidth               = dst_w;
    content.OutputHeight              = dst_h;
    content.Usage                     = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(impl->video_device->CreateVideoProcessorEnumerator(
            &content, impl->vp_enum.put())))
        return false;
    if (FAILED(impl->video_device->CreateVideoProcessor(
            impl->vp_enum.get(), 0, impl->video_processor.put())))
        return false;

    D3D11_TEXTURE2D_DESC d{};
    d.Width            = dst_w;
    d.Height           = dst_h;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    d.BindFlags        = D3D11_BIND_RENDER_TARGET;
    if (FAILED(impl->d3d_device->CreateTexture2D(&d, nullptr, impl->scaled_tex.put())))
        return false;

    impl->vp_src_w = src_w;
    impl->vp_src_h = src_h;
    impl->vp_dst_w = dst_w;
    impl->vp_dst_h = dst_h;
    return true;
}

// Scales `src_tex` (src_w x src_h) into impl->scaled_tex (dst_w x dst_h) via
// the D3D11 video processor blit. Returns false on any failure; impl->scaled_tex
// content is undefined in that case and must not be used by the caller.
static bool gpu_downscale(DisplayCapture::Impl* impl, ID3D11Texture2D* src_tex,
                           uint32_t src_w, uint32_t src_h,
                           uint32_t dst_w, uint32_t dst_h)
{
    if (!ensure_video_processor(impl, src_w, src_h, dst_w, dst_h)) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.FourCC               = 0;
    in_desc.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_desc.Texture2D.MipSlice   = 0;
    in_desc.Texture2D.ArraySlice = 0;

    winrt::com_ptr<ID3D11VideoProcessorInputView> in_view;
    if (FAILED(impl->video_device->CreateVideoProcessorInputView(
            src_tex, impl->vp_enum.get(), &in_desc, in_view.put())))
        return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension      = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_desc.Texture2D.MipSlice = 0;

    winrt::com_ptr<ID3D11VideoProcessorOutputView> out_view;
    if (FAILED(impl->video_device->CreateVideoProcessorOutputView(
            impl->scaled_tex.get(), impl->vp_enum.get(), &out_desc, out_view.put())))
        return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable        = TRUE;
    stream.pInputSurface  = in_view.get();

    return SUCCEEDED(impl->video_ctx->VideoProcessorBlt(
        impl->video_processor.get(), out_view.get(), 0, 1, &stream));
}

// ── DisplayCapture ────────────────────────────────────────────────────────────

DisplayCapture::DisplayCapture() : impl_(new Impl()) {}
DisplayCapture::~DisplayCapture() { stop(); delete impl_; }
bool DisplayCapture::running() const { return impl_->active.load(std::memory_order_relaxed); }
bool DisplayCapture::border_suppressed() const { return impl_->border_suppressed.load(std::memory_order_relaxed); }

CaptureStats DisplayCapture::stats() const
{
    CaptureStats s;
    s.frames_arrived = impl_->frames_arrived.load(std::memory_order_relaxed);
    s.frames_dropped_before_readback = impl_->frames_dropped_before_readback.load(std::memory_order_relaxed);
    s.frames_readback = impl_->frames_readback.load(std::memory_order_relaxed);
    s.readback_time_us_total = impl_->readback_time_us_total.load(std::memory_order_relaxed);
    return s;
}

bool DisplayCapture::start(HMONITOR hmon, FrameCallback cb, bool show_border)
{
    CaptureOptions options;
    options.show_border = show_border;
    return start(hmon, std::move(cb), options);
}

bool DisplayCapture::start(HMONITOR hmon, FrameCallback cb, CaptureOptions options)
{
    if (impl_->active) return false;
    if (!is_supported()) return false;

    impl_->cb         = std::move(cb);
    impl_->options    = options;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    impl_->qpc_freq = freq.QuadPart;
    impl_->frame_gate.reset(options.max_readback_fps, impl_->qpc_freq);
    impl_->frames_arrived.store(0, std::memory_order_relaxed);
    impl_->frames_dropped_before_readback.store(0, std::memory_order_relaxed);
    impl_->frames_readback.store(0, std::memory_order_relaxed);
    impl_->readback_time_us_total.store(0, std::memory_order_relaxed);
    impl_->d3d_device = make_d3d_device();
    if (!impl_->d3d_device) return false;

    // Allow context access from any thread (WGC delivers on the thread pool).
    winrt::com_ptr<ID3D11Multithread> mt;
    impl_->d3d_device->QueryInterface(IID_PPV_ARGS(mt.put()));
    if (mt) mt->SetMultithreadProtected(TRUE);

    impl_->d3d_device->GetImmediateContext(impl_->d3d_ctx.put());

    // Best-effort: not all devices/drivers expose the D3D11 video processor
    // API. When absent, GPU downscale is silently skipped and readback stays
    // at native resolution (the pre-existing behavior).
    impl_->d3d_device->QueryInterface(IID_PPV_ARGS(impl_->video_device.put()));
    impl_->d3d_ctx->QueryInterface(IID_PPV_ARGS(impl_->video_ctx.put()));
    impl_->vp_available = impl_->video_device && impl_->video_ctx;

    try {
        impl_->winrt_device = wrap_d3d(impl_->d3d_device);
        auto item           = options.target_window
            ? item_from_window(options.target_window)
            : item_from_monitor(hmon);
        impl_->last_size    = item.Size();

        // CreateFreeThreaded: callbacks arrive on the WinRT thread pool regardless
        // of the calling thread's apartment — required for Win32 desktop apps.
        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->winrt_device,
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,              // double-buffer
            impl_->last_size);

        impl_->active = true;

        impl_->frame_token = impl_->pool.FrameArrived(
            [this](wgc::Direct3D11CaptureFramePool const& pool, auto const&)
            {
                if (!impl_->active.load(std::memory_order_acquire)) return;

                auto frame = pool.TryGetNextFrame();
                if (!frame) return;

                impl_->frames_arrived.fetch_add(1, std::memory_order_relaxed);

                LARGE_INTEGER qpc{};
                QueryPerformanceCounter(&qpc);

                if (!impl_->options.allow_unlimited_readback &&
                    !impl_->frame_gate.should_accept(qpc.QuadPart)) {
                    impl_->frames_dropped_before_readback.fetch_add(1, std::memory_order_relaxed);
                    frame.Close();
                    return;
                }

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

                    // GPU pre-readback downscale: when a smaller output size is
                    // configured, scale on the GPU into impl_->scaled_tex first and
                    // size the staging texture (and thus the CPU readback/memcpy)
                    // to that smaller size instead of native resolution. Falls back
                    // to the native-resolution path on any failure or when no
                    // downscale is configured, so behavior is unchanged by default.
                    const uint32_t want_w = static_cast<uint32_t>(std::max(0, impl_->options.output_width));
                    const uint32_t want_h = static_cast<uint32_t>(std::max(0, impl_->options.output_height));
                    const bool want_scale = impl_->vp_available && want_w > 0 && want_h > 0 &&
                        (want_w != fi.width || want_h != fi.height);

                    ID3D11Texture2D* copy_src = gpu_tex.get();
                    uint32_t         copy_w   = fi.width;
                    uint32_t         copy_h   = fi.height;
                    if (want_scale &&
                        gpu_downscale(impl_, gpu_tex.get(), fi.width, fi.height, want_w, want_h)) {
                        copy_src = impl_->scaled_tex.get();
                        copy_w   = want_w;
                        copy_h   = want_h;
                    }
                    fi.width  = copy_w;
                    fi.height = copy_h;

                    // Create / recreate staging texture when dimensions change.
                    if (!impl_->staging_tex ||
                        impl_->staging_w != copy_w ||
                        impl_->staging_h != copy_h) {
                        D3D11_TEXTURE2D_DESC d{};
                        d.Width            = copy_w;
                        d.Height           = copy_h;
                        d.MipLevels        = 1;
                        d.ArraySize        = 1;
                        d.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
                        d.SampleDesc.Count = 1;
                        d.Usage            = D3D11_USAGE_STAGING;
                        d.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
                        impl_->staging_tex = nullptr;
                        impl_->d3d_device->CreateTexture2D(&d, nullptr, impl_->staging_tex.put());
                        impl_->staging_w = copy_w;
                        impl_->staging_h = copy_h;
                    }

                    if (impl_->staging_tex) {
                        LARGE_INTEGER readback_start{};
                        QueryPerformanceCounter(&readback_start);

                        impl_->d3d_ctx->CopyResource(impl_->staging_tex.get(), copy_src);

                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(impl_->d3d_ctx->Map(
                                impl_->staging_tex.get(), 0,
                                D3D11_MAP_READ, 0, &mapped))) {
                            LARGE_INTEGER readback_end{};
                            QueryPerformanceCounter(&readback_end);
                            if (impl_->qpc_freq > 0) {
                                const uint64_t elapsed_us = static_cast<uint64_t>(
                                    (readback_end.QuadPart - readback_start.QuadPart) * 1000000ll /
                                    impl_->qpc_freq);
                                impl_->readback_time_us_total.fetch_add(elapsed_us, std::memory_order_relaxed);
                            }
                            impl_->frames_readback.fetch_add(1, std::memory_order_relaxed);
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

        int min_update_ms = 1;
        if (!impl_->options.allow_unlimited_readback && impl_->options.max_readback_fps > 0)
            min_update_ms = std::max(1, 1000 / impl_->options.max_readback_fps);
        impl_->session.MinUpdateInterval(
            std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                std::chrono::milliseconds(min_update_ms)));

        if (!impl_->options.show_border) {
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

    impl_->staging_tex     = nullptr;
    impl_->video_processor = nullptr;
    impl_->vp_enum         = nullptr;
    impl_->scaled_tex      = nullptr;
    impl_->video_ctx       = nullptr;
    impl_->video_device    = nullptr;
    impl_->d3d_ctx      = nullptr;
    impl_->winrt_device = nullptr;
    impl_->d3d_device   = nullptr;
    impl_->cb           = nullptr;
    impl_->staging_w    = 0;
    impl_->staging_h    = 0;
    impl_->vp_src_w = impl_->vp_src_h = impl_->vp_dst_w = impl_->vp_dst_h = 0;
    impl_->vp_available = false;
    impl_->border_suppressed.store(false, std::memory_order_relaxed);
    impl_->seq.store(0, std::memory_order_relaxed);
}

} // namespace capture
