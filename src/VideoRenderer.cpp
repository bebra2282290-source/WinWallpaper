#include "VideoRenderer.h"

#include "Log.h"
#include "Messages.h"

#include <d3d10.h> // ID3D10Multithread
#include <mferror.h>

#include <new>

// winbase.h defines GetCurrentTime() as a macro, which clashes with IMFMediaEngine::GetCurrentTime.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

using Microsoft::WRL::ComPtr;

namespace {

// Receives Media Engine events on Media Foundation worker threads. It never
// blocks: events are posted to the UI thread and handled there.
class EngineNotify final : public IMFMediaEngineNotify {
public:
    EngineNotify(HWND hwnd, unsigned generation) : m_hwnd(hwnd), m_generation(generation) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG refs = --m_refs;
        if (refs == 0) delete this;
        return refs;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            SetEvent(reinterpret_cast<HANDLE>(param1));
            return S_OK;
        }
        auto* e = new (std::nothrow) EngineEvent{m_generation, event, param1, param2};
        if (e && !PostMessageW(m_hwnd, WM_APP_ENGINE_EVENT, 0, reinterpret_cast<LPARAM>(e))) delete e;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    HWND m_hwnd;
    unsigned m_generation;
};

LONGLONG QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

LONGLONG QpcFrequency() {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}

// Works out which part of the video goes where on a surface of w x h pixels.
// Returns true if parts of the surface stay uncovered (letterboxing).
bool ComputeRects(Scaling mode, double videoAspect, UINT w, UINT h, MFVideoNormalizedRect& src, RECT& dst) {
    src = {0.0f, 0.0f, 1.0f, 1.0f};
    dst = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    const double screenAspect = static_cast<double>(w) / h;
    if (mode == Scaling::Fill) {
        if (videoAspect > screenAspect) {
            float visible = static_cast<float>(screenAspect / videoAspect);
            src.left = (1.0f - visible) / 2.0f;
            src.right = src.left + visible;
        } else {
            float visible = static_cast<float>(videoAspect / screenAspect);
            src.top = (1.0f - visible) / 2.0f;
            src.bottom = src.top + visible;
        }
        return false;
    }
    if (mode == Scaling::Fit) {
        if (videoAspect > screenAspect) {
            LONG height = static_cast<LONG>(w / videoAspect + 0.5);
            dst.top = (static_cast<LONG>(h) - height) / 2;
            dst.bottom = dst.top + height;
        } else {
            LONG width = static_cast<LONG>(h * videoAspect + 0.5);
            dst.left = (static_cast<LONG>(w) - width) / 2;
            dst.right = dst.left + width;
        }
        return dst.left > 0 || dst.top > 0;
    }
    return false;
}

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

} // namespace

VideoRenderer::~VideoRenderer() {
    Shutdown();
}

HRESULT VideoRenderer::Initialize(HWND notifyWindow) {
    Shutdown();
    m_notifyWindow = notifyWindow;
    HRESULT hr = CreateDevice();
    if (FAILED(hr)) {
        ReleaseDevice();
        return hr;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = false;
        m_deviceLost = false;
    }
    m_thread = std::thread([this] { RenderLoop(); });
    return S_OK;
}

void VideoRenderer::Shutdown() {
    Close();
    ClearSurfaces();
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    ReleaseDevice();
}

HRESULT VideoRenderer::CreateDevice() {
    // Prefer the GPU that drives the primary monitor: on laptops with two GPUs
    // that is normally the power-efficient integrated one, and frames never
    // have to be copied between adapters.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    const POINT origin{0, 0};
    const HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    {
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; !adapter && factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            ComPtr<IDXGIOutput> output;
            for (UINT j = 0; candidate->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == primary) {
                    adapter = candidate;
                    break;
                }
            }
        }
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    const UINT videoFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    auto create = [&](IDXGIAdapter* a, D3D_DRIVER_TYPE type, UINT flags) {
        m_device.Reset();
        m_context.Reset();
        return D3D11CreateDevice(a, type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device,
                                 nullptr, &m_context);
    };
    hr = E_FAIL;
    if (adapter) hr = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, videoFlags);
    if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, videoFlags);
    if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    if (FAILED(hr)) hr = create(nullptr, D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    if (FAILED(hr)) {
        Log::Write("D3D11CreateDevice failed: 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    // The Media Engine uses the device from its own threads.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(m_device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return hr;
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> deviceAdapter;
    hr = dxgiDevice->GetAdapter(&deviceAdapter);
    if (FAILED(hr)) return hr;
    hr = deviceAdapter->GetParent(IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) return hr;

    // Output used to pace the render loop on the vertical blank.
    ComPtr<IDXGIOutput> output;
    for (UINT j = 0; deviceAdapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
        DXGI_OUTPUT_DESC od{};
        if (!m_output) m_output = output; // first output as a fallback
        if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == primary) {
            m_output = output;
            break;
        }
    }

    if (Log::Enabled()) {
        DXGI_ADAPTER_DESC desc{};
        deviceAdapter->GetDesc(&desc);
        Log::Write("GPU: %s, feature level 0x%X, video support %s, vblank output %s",
                   Log::Utf8(desc.Description).c_str(), static_cast<unsigned>(m_device->GetFeatureLevel()),
                   (m_device->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) ? "yes" : "no",
                   m_output ? "yes" : "no");
    }

    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &m_dxgiManager);
    if (FAILED(hr)) return hr;
    return m_dxgiManager->ResetDevice(m_device.Get(), token);
}

void VideoRenderer::ReleaseDevice() {
    m_dcomp.Reset();
    m_dxgiManager.Reset();
    m_output.Reset();
    m_factory.Reset();
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_device.Reset();
}

HRESULT VideoRenderer::CreateCompositionSwapChain(HWND hwnd, DXGI_SWAP_CHAIN_DESC1 desc, Surface& surface) {
    HRESULT hr = S_OK;
    if (!m_dcomp) {
        ComPtr<IDXGIDevice> dxgiDevice;
        hr = m_device.As(&dxgiDevice);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcomp));
        if (FAILED(hr)) return hr;
    }
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    hr = m_factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr, &surface.swapChain);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateTargetForHwnd(hwnd, TRUE, &surface.target);
    if (SUCCEEDED(hr)) hr = m_dcomp->CreateVisual(&surface.visual);
    if (SUCCEEDED(hr)) hr = surface.visual->SetContent(surface.swapChain.Get());
    if (SUCCEEDED(hr)) hr = surface.target->SetRoot(surface.visual.Get());
    if (SUCCEEDED(hr)) hr = m_dcomp->Commit();
    if (FAILED(hr)) {
        surface.target.Reset();
        surface.visual.Reset();
        surface.swapChain.Reset();
    }
    return hr;
}

HRESULT VideoRenderer::AddSurface(HWND hwnd, PresentMode mode) {
    if (!m_device) return E_UNEXPECTED;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    Surface surface;
    surface.hwnd = hwnd;
    surface.width = static_cast<UINT>(rc.right > 1 ? rc.right : 1);
    surface.height = static_cast<UINT>(rc.bottom > 1 ? rc.bottom : 1);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = surface.width;
    desc.Height = surface.height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    HRESULT hr = E_FAIL;
    const char* used = "none";
    if (mode == PresentMode::DComp) {
        hr = CreateCompositionSwapChain(hwnd, desc, surface);
        Log::Write("DirectComposition swap chain: 0x%08lX", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) used = "DirectComposition";
    }
    if (FAILED(hr) && mode != PresentMode::Blt) {
        // Flip model: DWM takes our buffer directly, no extra copy.
        desc.BufferCount = 2;
        for (DXGI_SWAP_EFFECT effect : {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL}) {
            desc.SwapEffect = effect;
            hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc, nullptr, nullptr, &surface.swapChain);
            Log::Write("Flip swap chain (effect %d): 0x%08lX", static_cast<int>(effect), static_cast<unsigned long>(hr));
            if (SUCCEEDED(hr)) {
                used = "flip";
                break;
            }
        }
    }
    if (FAILED(hr)) {
        desc.BufferCount = 1;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc, nullptr, nullptr, &surface.swapChain);
        Log::Write("Blt swap chain: 0x%08lX", static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) used = "blt";
    }
    if (FAILED(hr)) return hr;
    if (!surface.target) m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);

    // In D3D11 buffer 0 always refers to the current back buffer, so one view is enough.
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(surface.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &surface.rtv);
    }
    // Start black instead of showing garbage until the first frame arrives.
    if (surface.rtv) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->ClearRenderTargetView(surface.rtv.Get(), black);
        hr = surface.swapChain->Present(0, 0);
        Log::Write("Surface %p %ux%u uses %s presentation, first present 0x%08lX", static_cast<void*>(hwnd),
                   surface.width, surface.height, used, static_cast<unsigned long>(hr));
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_surfaces.push_back(std::move(surface));
    m_redraw = true;
    m_wake.notify_all();
    return S_OK;
}

void VideoRenderer::ClearSurfaces() {
    std::vector<Surface> old;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        old.swap(m_surfaces);
    }
    old.clear();
    if (m_dcomp) m_dcomp->Commit();
    if (m_context) m_context->Flush(); // swap chain destruction is deferred until a flush
}

void VideoRenderer::SetSurfaceVisible(size_t index, bool visible) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_surfaces.size() || m_surfaces[index].visible == visible) return;
    m_surfaces[index].visible = visible;
    if (visible) {
        m_redraw = true;
        m_wake.notify_all();
    }
}

HRESULT VideoRenderer::Open(const std::wstring& path, int volume, double startSeconds) {
    Close();
    if (!m_dxgiManager) return E_UNEXPECTED;

    ComPtr<IMFMediaEngineClassFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    const unsigned generation = ++m_generation;
    ComPtr<IMFMediaEngineNotify> notify;
    notify.Attach(new (std::nothrow) EngineNotify(m_notifyWindow, generation));
    if (!notify) return E_OUTOFMEMORY;

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 3);
    if (FAILED(hr)) return hr;
    // Giving the engine our device is what enables hardware (DXVA) decoding.
    attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, m_dxgiManager.Get());
    attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    ComPtr<IMFMediaEngine> engine;
    hr = factory->CreateInstance(0, attributes.Get(), &engine);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaEngineEx> engineEx;
    engine.As(&engineEx);

    const bool wantAudio = volume > 0;
    engine->SetLoop(TRUE);
    engine->SetAutoPlay(FALSE);
    engine->SetMuted(wantAudio ? FALSE : TRUE);
    engine->SetVolume(volume / 100.0);

    // Opening through a byte stream avoids URL parsing problems with
    // characters such as '#' or '%' in file names.
    hr = E_FAIL;
    BSTR url = SysAllocString(path.c_str());
    if (!url) {
        engine->Shutdown();
        return E_OUTOFMEMORY;
    }
    if (engineEx) {
        ComPtr<IMFByteStream> stream;
        if (SUCCEEDED(MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
                                   &stream))) {
            hr = engineEx->SetSourceFromByteStream(stream.Get(), url);
        }
    }
    if (FAILED(hr)) hr = engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) {
        engine->Shutdown();
        return hr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_engine = engine;
    m_engineEx = engineEx;
    m_wantAudio = wantAudio;
    m_audioDeselected = false;
    m_pendingSeek = startSeconds > 0.0 ? startSeconds : 0.0;
    m_redraw = false;
    if (m_playing) m_engine->Play();
    m_wake.notify_all();
    return S_OK;
}

void VideoRenderer::Close() {
    ComPtr<IMFMediaEngine> engine;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        engine.Swap(m_engine);
        m_engineEx.Reset();
        ++m_generation; // events still queued from this engine will be ignored
    }
    if (engine) engine->Shutdown();
}

double VideoRenderer::CurrentTime() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_engine ? m_engine->GetCurrentTime() : 0.0;
}

void VideoRenderer::SetPlaying(bool playing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playing == playing) return;
    m_playing = playing;
    if (m_engine) {
        if (playing) {
            m_engine->Play();
        } else {
            m_engine->Pause();
        }
    }
    m_wake.notify_all();
}

bool VideoRenderer::SetVolume(int volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool wantAudio = volume > 0;
    if (!m_engine) {
        m_wantAudio = wantAudio;
        return false;
    }
    m_engine->SetMuted(wantAudio ? FALSE : TRUE);
    if (wantAudio) m_engine->SetVolume(volume / 100.0);
    m_wantAudio = wantAudio;
    // The audio track was deselected when the video was opened silently;
    // re-selecting it while playing is unreliable, so ask for a reopen.
    return wantAudio && m_audioDeselected;
}

void VideoRenderer::SetScaling(Scaling scaling) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_scaling = scaling;
    m_redraw = true;
    m_wake.notify_all();
}

void VideoRenderer::SetFpsLimit(int fps) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fpsLimit = fps;
}

void VideoRenderer::RequestRedraw() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_redraw = true;
    m_wake.notify_all();
}

void VideoRenderer::DeselectAudioLocked() {
    if (!m_engineEx || m_wantAudio) return;
    DWORD count = 0;
    if (FAILED(m_engineEx->GetNumberOfStreams(&count))) return;
    bool changed = false;
    for (DWORD i = 0; i < count; ++i) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(m_engineEx->GetStreamAttribute(i, MF_MT_MAJOR_TYPE, &value)) && value.vt == VT_CLSID &&
            value.puuid && *value.puuid == MFMediaType_Audio) {
            changed = SUCCEEDED(m_engineEx->SetStreamSelection(i, FALSE)) || changed;
        }
        PropVariantClear(&value);
    }
    if (changed && SUCCEEDED(m_engineEx->ApplyStreamSelections())) m_audioDeselected = true;
    Log::Write("Audio track %s", m_audioDeselected ? "deselected (not decoded)" : "kept (muted)");
}

EngineStatus VideoRenderer::HandleEngineEvent(const EngineEvent& event, HRESULT* error) {
    if (event.generation != m_generation) return EngineStatus::Ok; // from an engine that is already gone
    switch (event.event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engine) break;
        if (Log::Enabled()) {
            DWORD w = 0, h = 0;
            m_engine->GetNativeVideoSize(&w, &h);
            Log::Write("Video loaded: %lux%lu, duration %.2f s", static_cast<unsigned long>(w),
                       static_cast<unsigned long>(h), m_engine->GetDuration());
        }
        DeselectAudioLocked();
        if (m_pendingSeek > 0.0) {
            m_engine->SetCurrentTime(m_pendingSeek);
            m_pendingSeek = 0.0;
        }
        break;
    }
    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
        RequestRedraw();
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        HRESULT hr = static_cast<HRESULT>(event.param2);
        Log::Write("Media Engine error %lu, 0x%08lX", static_cast<unsigned long>(event.param1),
                   static_cast<unsigned long>(hr));
        if (error) *error = hr;
        return IsDeviceLost(hr) ? EngineStatus::DeviceLost : EngineStatus::Error;
    }
    default:
        break;
    }
    return EngineStatus::Ok;
}

void VideoRenderer::RenderLoop() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // A high-resolution timer does not raise the system timer resolution
    // (unlike timeBeginPeriod), so it does not cost battery.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    for (;;) {
        bool playing;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_quit || (m_engine && !m_surfaces.empty() && !m_deviceLost && (m_playing || m_redraw));
            });
            if (m_quit) break;
            playing = m_playing;
        }

        if (playing) WaitForFrameBoundary(timer);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_quit) break;
        if (!m_engine || m_surfaces.empty() || m_deviceLost) continue;

        LONGLONG pts = 0;
        const bool newFrame = m_playing && m_engine->OnVideoStreamTick(&pts) == S_OK;
        const bool forced = m_redraw;
        m_redraw = false;
        if (!newFrame && !forced) continue;
        if (newFrame && !forced && m_fpsLimit > 0) {
            const LONGLONG minInterval = QpcFrequency() * 9 / (10 * m_fpsLimit);
            if (QpcNow() - m_lastPresent < minInterval) continue; // drop this frame
        }
        DrawFrameLocked();
    }

    if (timer) CloseHandle(timer);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

void VideoRenderer::WaitForFrameBoundary(HANDLE timer) {
    if (m_output && m_vblankBackoff == 0) {
        const LONGLONG start = QpcNow();
        if (SUCCEEDED(m_output->WaitForVBlank())) {
            // Some drivers return immediately (e.g. while the display is
            // asleep). Fall back to the timer instead of spinning.
            if ((QpcNow() - start) * 1000 >= QpcFrequency()) {
                m_fastVblanks = 0;
                return;
            }
            if (++m_fastVblanks < 3) return;
        }
        m_fastVblanks = 0;
        m_vblankBackoff = 120;
    }
    if (m_vblankBackoff > 0) --m_vblankBackoff;

    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -100000; // 10 ms
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, 100);
            return;
        }
    }
    Sleep(10);
}

void VideoRenderer::DrawFrameLocked() {
    DWORD videoWidth = 0, videoHeight = 0;
    if (FAILED(m_engine->GetNativeVideoSize(&videoWidth, &videoHeight)) || videoWidth == 0 || videoHeight == 0) return;
    DWORD aspectX = 1, aspectY = 1;
    if (FAILED(m_engine->GetVideoAspectRatio(&aspectX, &aspectY)) || aspectX == 0 || aspectY == 0) aspectX = aspectY = 1;
    const double videoAspect =
        (static_cast<double>(videoWidth) * aspectX) / (static_cast<double>(videoHeight) * aspectY);

    MFARGB border{};
    border.rgbAlpha = 255;

    for (Surface& surface : m_surfaces) {
        if (!surface.visible) continue;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(surface.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) continue;

        MFVideoNormalizedRect src;
        RECT dst;
        if (ComputeRects(m_scaling, videoAspect, surface.width, surface.height, src, dst) && surface.rtv) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            m_context->ClearRenderTargetView(surface.rtv.Get(), black);
        }
        HRESULT hr = m_engine->TransferVideoFrame(backBuffer.Get(), &src, &dst, &border);
        const bool transferred = SUCCEEDED(hr);
        if (transferred) hr = surface.swapChain->Present(0, 0);
        if (FAILED(hr) && hr != surface.lastError) {
            Log::Write("%s failed on surface %p: 0x%08lX", transferred ? "Present" : "TransferVideoFrame",
                       static_cast<void*>(surface.hwnd), static_cast<unsigned long>(hr));
        }
        surface.lastError = FAILED(hr) ? hr : S_OK;
        if (IsDeviceLost(hr)) {
            m_deviceLost = true;
            PostMessageW(m_notifyWindow, WM_APP_DEVICE_LOST, 0, 0);
            return;
        }
    }
    m_lastPresent = QpcNow();

    if (Log::Enabled()) {
        ++m_framesSinceReport;
        if (m_lastPresent - m_lastReport >= 5 * QpcFrequency()) {
            Log::Write("Presented %u frames in the last %.1f s", m_framesSinceReport,
                       m_lastReport ? static_cast<double>(m_lastPresent - m_lastReport) / QpcFrequency() : 0.0);
            m_framesSinceReport = 0;
            m_lastReport = m_lastPresent;
        }
    }
}
