#pragma once

#include "Settings.h"

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A Media Engine event forwarded from Media Foundation worker threads to the UI thread.
struct EngineEvent {
    unsigned generation;
    DWORD event;
    DWORD_PTR param1;
    DWORD param2;
};

enum class EngineStatus {
    Ok,
    Error,      // playback failed; see the HRESULT
    DeviceLost, // the GPU was reset; the renderer must be re-initialized
};

// Plays one video with the Media Foundation Media Engine in frame-server mode
// and draws every frame onto one swap chain per monitor.
//
// Why this is cheap:
//  * decoding runs on the GPU (DXVA) because the engine gets our D3D11 device;
//  * a decoded frame goes from the decoder to the screen without leaving the
//    GPU (TransferVideoFrame = one video-processor blit per monitor);
//  * one decode feeds all monitors;
//  * the render thread sleeps on the vertical blank and presents only when the
//    engine actually has a new frame; while paused it sleeps on a condition
//    variable and uses no CPU or GPU at all;
//  * when sound is off the audio track is deselected and never decoded.
class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();
    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    HRESULT Initialize(HWND notifyWindow);
    void Shutdown();
    bool IsInitialized() const { return m_device != nullptr; }

    // Surfaces are wallpaper windows (one per monitor). `mode` must not be
    // Auto; if it fails the next simpler model is tried (DComp -> Flip -> Blt).
    HRESULT AddSurface(HWND hwnd, PresentMode mode);
    void ClearSurfaces();
    void SetSurfaceVisible(size_t index, bool visible);

    HRESULT Open(const std::wstring& path, int volume, double startSeconds);
    void Close();
    double CurrentTime();

    void SetPlaying(bool playing);
    // Returns true if the video has to be reopened to bring the audio track back.
    bool SetVolume(int volume);
    void SetScaling(Scaling scaling);
    void SetFpsLimit(int fps);
    void RequestRedraw();

    EngineStatus HandleEngineEvent(const EngineEvent& event, HRESULT* error);

private:
    struct Surface {
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<IDCompositionTarget> target; // DComp mode only
        Microsoft::WRL::ComPtr<IDCompositionVisual> visual; // DComp mode only
        UINT width = 0;
        UINT height = 0;
        bool visible = true;
        HRESULT lastError = S_OK; // last logged failure, to avoid flooding the log
    };

    HRESULT CreateCompositionSwapChain(HWND hwnd, DXGI_SWAP_CHAIN_DESC1 desc, Surface& surface);

    HRESULT CreateDevice();
    void ReleaseDevice();
    void RenderLoop();
    void WaitForFrameBoundary(HANDLE timer);
    void DrawFrameLocked();
    void DeselectAudioLocked();

    HWND m_notifyWindow = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
    Microsoft::WRL::ComPtr<IDXGIOutput> m_output; // used to wait for vblank
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiManager;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcomp; // created on first use

    // Everything below is shared with the render thread and guarded by m_mutex.
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_quit = false;
    bool m_deviceLost = false;
    bool m_playing = false;
    bool m_redraw = false;
    Microsoft::WRL::ComPtr<IMFMediaEngine> m_engine;
    Microsoft::WRL::ComPtr<IMFMediaEngineEx> m_engineEx;
    std::vector<Surface> m_surfaces;
    Scaling m_scaling = Scaling::Fill;
    int m_fpsLimit = 0;
    LONGLONG m_lastPresent = 0;
    bool m_audioDeselected = false;
    bool m_wantAudio = false;
    double m_pendingSeek = 0.0;

    std::atomic<unsigned> m_generation{0};

    // Render thread only.
    int m_fastVblanks = 0;
    int m_vblankBackoff = 0;
    unsigned m_framesSinceReport = 0;
    LONGLONG m_lastReport = 0;
};
