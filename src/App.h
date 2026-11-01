#pragma once

#include "Desktop.h"
#include "Settings.h"
#include "VideoRenderer.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

// Class of the hidden main window; a second instance finds it to pass on its command line.
extern const wchar_t kMainWindowClass[];

// WM_COPYDATA commands sent by a second instance.
enum : ULONG_PTR {
    kCopySetVideo = 1, // lpData: UTF-16 path
    kCopyActivate = 2, // opens the "choose video" dialog
    kCopyExit = 3,
};

class App {
public:
    int Run(HINSTANCE instance, const std::wstring& video, bool autostart);

    // Called from the WinEvent hook: something changed on screen.
    void CheckSoon();

private:
    enum class PauseReason { None, User, Locked, DisplayOff, Battery, Fullscreen, Covered, NoDesktop };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCommand(UINT id);
    void OnTimer(UINT_PTR id);
    void OnCopyData(const COPYDATASTRUCT* data);
    void OnPowerBroadcast(WPARAM type, LPARAM data);
    void OnEngineEvent(const EngineEvent& event);

    void StartWallpaper(double startSeconds);
    void StopWallpaper();
    bool RebuildSurfaces();
    void DestroySurfaces();
    void ScheduleRebuild(UINT delayMs);
    void UpdatePlayback();
    void ReopenAtCurrentTime();
    void RecoverDevice();
    void SelectVideo();
    void SetVideo(const std::wstring& path);
    void ReportError(const std::wstring& text, HRESULT hr);

    std::wstring StatusText() const;
    void AddTrayIcon();
    void UpdateTray();
    void Notify(const std::wstring& text, bool error);
    void ShowMenu(POINT anchor);

    void RegisterSystemNotifications();
    void UnregisterSystemNotifications();
    void Cleanup();

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_taskbarCreatedMsg = 0;
    NOTIFYICONDATAW m_nid{};
    bool m_trayAdded = false;
    HICON m_icon = nullptr;

    Settings m_settings;
    VideoRenderer m_renderer;
    DesktopHost m_host;
    std::vector<HWND> m_windows;      // one wallpaper window per monitor
    std::vector<RECT> m_monitorRects; // matching monitor rectangles (screen coordinates)

    bool m_active = false;  // a video is supposed to be on the desktop
    bool m_playing = false; // the renderer is currently playing
    bool m_userPaused = false;
    bool m_locked = false;
    bool m_displayOff = false;
    PauseReason m_reason = PauseReason::None;
    int m_desktopFailures = 0;
    std::wstring m_pendingVideo; // received from a second instance

    bool m_menuOpen = false;
    bool m_dialogOpen = false;
    bool m_cleanedUp = false;

    DWORD m_lastDeviceReset = 0;
    int m_deviceResets = 0;

    HWINEVENTHOOK m_hooks[3] = {};
    HPOWERNOTIFY m_powerNotify[3] = {};
};
