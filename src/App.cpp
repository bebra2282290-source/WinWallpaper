#include "App.h"

#include "Activity.h"
#include "Log.h"
#include "Messages.h"
#include "Strings.h"
#include "resource.h"

#include <commdlg.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

const wchar_t kMainWindowClass[] = L"WinWallpaper.Main";

namespace {

constexpr UINT kTrayId = 1;

enum TimerId : UINT_PTR {
    kTimerPoll = 1,  // periodic activity check
    kTimerCheckSoon, // activity check shortly after a window event
    kTimerWatchdog,  // is Explorer still hosting our windows?
    kTimerRebuild,   // recreate the wallpaper windows (display change, Explorer restart)
    kTimerTrayRetry, // the taskbar was not ready yet
};

constexpr UINT kPollIntervalMs = 500;
constexpr UINT kWatchdogIntervalMs = 3000;
constexpr UINT kCheckSoonDelayMs = 150;

enum CommandId : UINT {
    kCmdSelect = 100,
    kCmdOpenPending,
    kCmdPause,
    kCmdAutostart,
    kCmdRemove,
    kCmdExit,
    kCmdPauseFullscreen,
    kCmdPauseCovered,
    kCmdPauseBattery,
    kCmdScaleBase = 200,  // + Scaling
    kCmdVolumeBase = 300, // + index in kVolumes
    kCmdFpsBase = 400,    // + index in kFpsLimits
};

constexpr int kVolumes[] = {0, 10, 25, 50, 75, 100};
constexpr int kFpsLimits[] = {0, 30, 15};
constexpr UINT kVolumeCount = static_cast<UINT>(sizeof(kVolumes) / sizeof(kVolumes[0]));
constexpr UINT kFpsCount = static_cast<UINT>(sizeof(kFpsLimits) / sizeof(kFpsLimits[0]));

// Power setting GUIDs from winnt.h, spelled out to avoid depending on INITGUID.
constexpr GUID kGuidConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
constexpr GUID kGuidAcDcPowerSource = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
constexpr GUID kGuidPowerSavingStatus = {0xe00958c0, 0xc213, 0x4ace, {0xac, 0x77, 0xfe, 0xcc, 0xed, 0x2e, 0xee, 0xa5}};

App* g_app = nullptr;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (g_app && hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) g_app->CheckSoon();
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FolderOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

template <size_t N>
void CopyTruncated(wchar_t (&dst)[N], const std::wstring& src) {
    size_t n = std::min(src.size(), N - 1);
    std::copy_n(src.c_str(), n, dst);
    dst[n] = L'\0';
}

std::wstring HResultText(HRESULT hr) {
    wchar_t code[16];
    swprintf(code, sizeof(code) / sizeof(code[0]), L"0x%08lX", static_cast<unsigned long>(hr));
    return std::wstring(T(S::ErrCode)) + L": " + code;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, &mi)) reinterpret_cast<std::vector<RECT>*>(param)->push_back(mi.rcMonitor);
    return TRUE;
}

void LogSystemInfo() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))) {
            rtlGetVersion(&version);
        }
    }
    Log::Write("WinWallpaper 1.0.0, Windows %lu.%lu build %lu", version.dwMajorVersion, version.dwMinorVersion,
               version.dwBuildNumber);
}

void ApplyProcessPolicies(bool efficiencyMode) {
    if (!efficiencyMode) return;
    // The wallpaper must always yield to whatever the user is doing.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    // Windows 11 "Efficiency mode" (EcoQoS): efficient cores, low clocks.
    // Ignored by older Windows versions.
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

} // namespace

// ---------------------------------------------------------------------------
// Startup and the message loop

int App::Run(HINSTANCE instance, const std::wstring& video, bool autostart, bool log) {
    m_instance = instance;
    g_app = this;

    m_settings.Load();
    if (log || m_settings.log) {
        Log::Enable(Settings::Directory() + L"WinWallpaper.log");
        LogSystemInfo();
    }
    if (!video.empty()) {
        m_settings.video = video;
        m_settings.Save();
    }
    Autostart::RefreshPath();
    ApplyProcessPolicies(m_settings.efficiencyMode);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    // A hidden top-level window rather than a message-only one: only top-level
    // windows receive broadcasts such as TaskbarCreated and WM_DISPLAYCHANGE.
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kMainWindowClass, L"WinWallpaper", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                         instance, this)) {
        return 1;
    }
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (m_taskbarCreatedMsg) ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    AddTrayIcon();
    RegisterSystemNotifications();

    HRESULT hr = m_renderer.Initialize(m_hwnd);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, (std::wstring(T(S::ErrD3D)) + L"\n" + HResultText(hr)).c_str(), T(S::AppName),
                    MB_ICONERROR);
        Cleanup();
        DestroyWindow(m_hwnd);
        g_app = nullptr;
        return 1;
    }

    if (!m_settings.video.empty()) {
        StartWallpaper(0.0);
    } else if (!autostart) {
        Notify(T(S::HintFirstRun), false);
        PostMessageW(m_hwnd, WM_COMMAND, kCmdSelect, 0);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Cleanup();
    g_app = nullptr;
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_CONTEXTMENU: {
            POINT anchor{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            ShowMenu(anchor);
            break;
        }
        default:
            break;
        }
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;

    case WM_TIMER:
        OnTimer(wParam);
        return 0;

    case WM_APP_ENGINE_EVENT: {
        std::unique_ptr<EngineEvent> event(reinterpret_cast<EngineEvent*>(lParam));
        if (event) OnEngineEvent(*event);
        return 0;
    }

    case WM_APP_DEVICE_LOST:
        RecoverDevice();
        return 0;

    case WM_APP_SURFACE_PAINT:
        m_renderer.RequestRedraw();
        return 0;

    case WM_COPYDATA:
        OnCopyData(reinterpret_cast<const COPYDATASTRUCT*>(lParam));
        return TRUE;

    case WM_DISPLAYCHANGE:
        if (m_active) ScheduleRebuild(800);
        return 0;

    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) CheckSoon();
        break;

    case WM_WTSSESSION_CHANGE:
        switch (wParam) {
        case WTS_SESSION_LOCK:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
            m_locked = true;
            break;
        case WTS_SESSION_UNLOCK:
            m_locked = false;
            break;
        default:
            break;
        }
        UpdatePlayback();
        return 0;

    case WM_POWERBROADCAST:
        OnPowerBroadcast(wParam, lParam);
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) Cleanup();
        return 0;

    case WM_CLOSE:
        DestroyWindow(m_hwnd);
        return 0;

    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;

    default:
        if (m_taskbarCreatedMsg != 0 && msg == m_taskbarCreatedMsg) {
            // Explorer restarted: the tray icon is gone and so are the windows
            // we were attached to.
            m_trayAdded = false;
            AddTrayIcon();
            if (m_active) ScheduleRebuild(1500);
            return 0;
        }
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Commands, timers and notifications

void App::OnCommand(UINT id) {
    if (id >= kCmdScaleBase && id <= kCmdScaleBase + static_cast<UINT>(Scaling::Stretch)) {
        m_settings.scaling = static_cast<Scaling>(id - kCmdScaleBase);
        m_settings.Save();
        m_renderer.SetScaling(m_settings.scaling);
        return;
    }
    if (id >= kCmdVolumeBase && id < kCmdVolumeBase + kVolumeCount) {
        m_settings.volume = kVolumes[id - kCmdVolumeBase];
        m_settings.Save();
        if (m_renderer.SetVolume(m_settings.volume) && m_active) ReopenAtCurrentTime();
        return;
    }
    if (id >= kCmdFpsBase && id < kCmdFpsBase + kFpsCount) {
        m_settings.fpsLimit = kFpsLimits[id - kCmdFpsBase];
        m_settings.Save();
        m_renderer.SetFpsLimit(m_settings.fpsLimit);
        return;
    }

    switch (id) {
    case kCmdSelect:
        SelectVideo();
        break;
    case kCmdOpenPending:
        if (!m_pendingVideo.empty()) {
            std::wstring path;
            path.swap(m_pendingVideo);
            SetVideo(path);
        }
        break;
    case kCmdPause:
        m_userPaused = !m_userPaused;
        UpdatePlayback();
        break;
    case kCmdPauseFullscreen:
        m_settings.pauseFullscreen = !m_settings.pauseFullscreen;
        m_settings.Save();
        UpdatePlayback();
        break;
    case kCmdPauseCovered:
        m_settings.pauseCovered = !m_settings.pauseCovered;
        m_settings.Save();
        UpdatePlayback();
        break;
    case kCmdPauseBattery:
        m_settings.pauseOnBattery = !m_settings.pauseOnBattery;
        m_settings.Save();
        UpdatePlayback();
        break;
    case kCmdAutostart:
        if (!Autostart::SetEnabled(!Autostart::IsEnabled())) MessageBeep(MB_ICONWARNING);
        break;
    case kCmdRemove:
        StopWallpaper();
        m_settings.video.clear();
        m_settings.Save();
        UpdateTray();
        break;
    case kCmdExit:
        DestroyWindow(m_hwnd);
        break;
    default:
        break;
    }
}

void App::OnTimer(UINT_PTR id) {
    switch (id) {
    case kTimerPoll:
        UpdatePlayback();
        break;
    case kTimerCheckSoon:
        KillTimer(m_hwnd, kTimerCheckSoon);
        UpdatePlayback();
        break;
    case kTimerWatchdog:
        if (!m_active) break;
        if (m_windows.empty() || !IsHostIntact(m_host, m_windows)) {
            RebuildSurfaces();
        } else if (m_host.layout == DesktopLayout::ProgmanFallback) {
            DesktopHost proper;
            if (ProbeDesktopHost(proper)) RebuildSurfaces(); // Explorer has finished starting up
        } else {
            EnsureZOrder(m_host, m_windows);
        }
        break;
    case kTimerRebuild:
        KillTimer(m_hwnd, kTimerRebuild);
        if (m_active) RebuildSurfaces();
        break;
    case kTimerTrayRetry:
        KillTimer(m_hwnd, kTimerTrayRetry);
        AddTrayIcon();
        break;
    default:
        break;
    }
}

void App::OnCopyData(const COPYDATASTRUCT* data) {
    if (!data) return;
    switch (data->dwData) {
    case kCopySetVideo:
        if (data->lpData && data->cbData >= sizeof(wchar_t)) {
            std::wstring path(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            path.resize(std::min(path.size(), path.find(L'\0')));
            if (!path.empty()) {
                // Do the work after returning, so the sending process is not kept waiting.
                m_pendingVideo = path;
                PostMessageW(m_hwnd, WM_COMMAND, kCmdOpenPending, 0);
            }
        }
        break;
    case kCopyActivate:
        PostMessageW(m_hwnd, WM_COMMAND, kCmdSelect, 0);
        break;
    case kCopyExit:
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

void App::OnPowerBroadcast(WPARAM type, LPARAM data) {
    if (type == PBT_POWERSETTINGCHANGE && data) {
        const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(data);
        if (setting->PowerSetting == kGuidConsoleDisplayState && setting->DataLength >= sizeof(DWORD)) {
            DWORD state = 0;
            memcpy(&state, setting->Data, sizeof(state));
            m_displayOff = state == 0; // 0 = off, 1 = on, 2 = dimmed
        }
    }
    if (type == PBT_POWERSETTINGCHANGE || type == PBT_APMPOWERSTATUSCHANGE || type == PBT_APMRESUMEAUTOMATIC) {
        UpdatePlayback();
    }
}

void App::OnEngineEvent(const EngineEvent& event) {
    HRESULT hr = S_OK;
    switch (m_renderer.HandleEngineEvent(event, &hr)) {
    case EngineStatus::DeviceLost:
        RecoverDevice();
        break;
    case EngineStatus::Error: {
        std::wstring name = FileName(m_settings.video);
        StopWallpaper();
        ReportError(std::wstring(T(S::ErrPlayback)) + L"\n" + name, hr);
        break;
    }
    default:
        break;
    }
}

void App::CheckSoon() {
    if (m_active) SetTimer(m_hwnd, kTimerCheckSoon, kCheckSoonDelayMs, nullptr);
}

void App::RegisterSystemNotifications() {
    WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION);

    // Windows sends the current value of each setting right after registration.
    const GUID* settings[] = {&kGuidConsoleDisplayState, &kGuidAcDcPowerSource, &kGuidPowerSavingStatus};
    for (size_t i = 0; i < 3; ++i) {
        m_powerNotify[i] = RegisterPowerSettingNotification(m_hwnd, settings[i], DEVICE_NOTIFY_WINDOW_HANDLE);
    }

    // React quickly when windows are activated, moved or minimized; the
    // periodic poll catches everything else (e.g. maximize/restore).
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    m_hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0, flags);
    m_hooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, WinEventProc, 0, 0, flags);
    m_hooks[2] =
        SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEventProc, 0, 0, flags);
}

void App::UnregisterSystemNotifications() {
    for (HWINEVENTHOOK& hook : m_hooks) {
        if (hook) UnhookWinEvent(hook);
        hook = nullptr;
    }
    for (HPOWERNOTIFY& notify : m_powerNotify) {
        if (notify) UnregisterPowerSettingNotification(notify);
        notify = nullptr;
    }
    if (m_hwnd) WTSUnRegisterSessionNotification(m_hwnd);
}

void App::Cleanup() {
    if (m_cleanedUp) return;
    m_cleanedUp = true;
    StopWallpaper();
    m_renderer.Shutdown();
    UnregisterSystemNotifications();
    if (m_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayAdded = false;
    }
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Wallpaper lifecycle

void App::StartWallpaper(double startSeconds) {
    StopWallpaper();
    if (m_settings.video.empty()) return;
    if (GetFileAttributesW(m_settings.video.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ReportError(std::wstring(T(S::ErrFileMissing)) + L"\n" + m_settings.video, S_OK);
        return;
    }
    if (!m_renderer.IsInitialized()) {
        HRESULT hr = m_renderer.Initialize(m_hwnd);
        if (FAILED(hr)) {
            ReportError(T(S::ErrD3D), hr);
            return;
        }
    }

    m_active = true;
    m_renderer.SetScaling(m_settings.scaling);
    m_renderer.SetFpsLimit(m_settings.fpsLimit);
    RebuildSurfaces(); // retries on its own if the desktop is not ready yet (e.g. right after login)

    HRESULT hr = m_renderer.Open(m_settings.video, m_settings.volume, startSeconds);
    if (FAILED(hr)) {
        std::wstring name = FileName(m_settings.video);
        StopWallpaper();
        ReportError(std::wstring(T(S::ErrPlayback)) + L"\n" + name, hr);
        return;
    }
    SetTimer(m_hwnd, kTimerPoll, kPollIntervalMs, nullptr);
    SetTimer(m_hwnd, kTimerWatchdog, kWatchdogIntervalMs, nullptr);
    UpdatePlayback();
    UpdateTray();
}

void App::StopWallpaper() {
    KillTimer(m_hwnd, kTimerPoll);
    KillTimer(m_hwnd, kTimerCheckSoon);
    KillTimer(m_hwnd, kTimerWatchdog);
    KillTimer(m_hwnd, kTimerRebuild);
    m_renderer.SetPlaying(false);
    m_renderer.Close();
    DestroySurfaces();
    m_active = false;
    m_playing = false;
    m_reason = PauseReason::None;
    UpdateTray();
}

bool App::RebuildSurfaces() {
    KillTimer(m_hwnd, kTimerRebuild);
    DestroySurfaces();
    if (!m_active) return false;

    // Right after login Explorer may still be building the desktop: retry a
    // few times before settling for the fallback layout.
    if (FindDesktopHost(m_host, m_desktopFailures >= 3)) {
        const bool raised = m_host.layout == DesktopLayout::Raised;
        PresentMode mode = m_settings.presentMode;
        if (mode == PresentMode::Auto) mode = raised ? PresentMode::Blt : PresentMode::Flip;
        const bool layered = m_settings.layeredWindow == LayeredWindow::Yes ||
                             (m_settings.layeredWindow == LayeredWindow::Auto && raised);
        Log::Write("Desktop: %s; Progman %p (exstyle 0x%08lX), parent %p, DefView %p, WorkerW %p; mode %d, layered %d",
                   LayoutName(m_host.layout), static_cast<void*>(m_host.progman),
                   static_cast<unsigned long>(GetWindowLongPtrW(m_host.progman, GWL_EXSTYLE)),
                   static_cast<void*>(m_host.parent), static_cast<void*>(m_host.defView),
                   static_cast<void*>(m_host.workerW), static_cast<int>(mode), layered ? 1 : 0);

        std::vector<RECT> monitors;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
        for (const RECT& rc : monitors) {
            HWND hwnd = CreateWallpaperWindow(m_host, rc, m_instance, m_hwnd, layered);
            Log::Write("Monitor (%ld,%ld)-(%ld,%ld): window %p", rc.left, rc.top, rc.right, rc.bottom,
                       static_cast<void*>(hwnd));
            if (!hwnd) continue;
            if (FAILED(m_renderer.AddSurface(hwnd, mode))) {
                DestroyWindow(hwnd);
                continue;
            }
            m_windows.push_back(hwnd);
            m_monitorRects.push_back(rc);
        }
    }

    if (m_windows.empty()) {
        // Explorer may still be starting (autostart) or restarting. Retry quietly
        // for a while before telling the user.
        if (++m_desktopFailures == 5) Notify(T(S::ErrDesktop), true);
        ScheduleRebuild(3000);
        UpdatePlayback();
        return false;
    }

    m_desktopFailures = 0;
    EnsureZOrder(m_host, m_windows);
    m_renderer.RequestRedraw();
    UpdatePlayback();
    return true;
}

void App::DestroySurfaces() {
    m_renderer.ClearSurfaces();
    for (HWND hwnd : m_windows) {
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
    }
    if (!m_windows.empty()) RefreshDesktop(m_host);
    m_windows.clear();
    m_monitorRects.clear();
}

void App::ScheduleRebuild(UINT delayMs) {
    SetTimer(m_hwnd, kTimerRebuild, delayMs, nullptr);
}

// Decides whether the video should play right now. Runs every 500 ms and
// right after relevant system events; each check costs well under a millisecond.
void App::UpdatePlayback() {
    if (!m_active) return;

    PauseReason reason = PauseReason::None;
    std::vector<bool> covered(m_windows.size(), false);
    if (m_windows.empty()) {
        reason = PauseReason::NoDesktop;
    } else if (m_userPaused) {
        reason = PauseReason::User;
    } else if (m_locked) {
        reason = PauseReason::Locked;
    } else if (m_displayOff) {
        reason = PauseReason::DisplayOff;
    } else if (m_settings.pauseOnBattery && Activity::IsPowerSaving()) {
        reason = PauseReason::Battery;
    } else if (m_settings.pauseFullscreen && Activity::IsFullscreenAppActive()) {
        reason = PauseReason::Fullscreen;
    } else if (m_settings.pauseCovered) {
        std::vector<RECT> workAreas;
        workAreas.reserve(m_monitorRects.size());
        for (const RECT& rc : m_monitorRects) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            workAreas.push_back(GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcWork : rc);
        }
        Activity::ComputeCoverage(workAreas, covered);
        if (std::all_of(covered.begin(), covered.end(), [](bool c) { return c; })) reason = PauseReason::Covered;
    }

    // Monitors that are covered are skipped even while others keep playing.
    if (reason == PauseReason::None) {
        for (size_t i = 0; i < covered.size(); ++i) m_renderer.SetSurfaceVisible(i, !covered[i]);
    }

    const bool play = reason == PauseReason::None;
    if (play != m_playing) {
        m_playing = play;
        m_renderer.SetPlaying(play);
    }
    if (reason != m_reason) {
        Log::Write("Playback: %s (reason %d)", play ? "playing" : "paused", static_cast<int>(reason));
        m_reason = reason;
        UpdateTray();
    }
}

void App::ReopenAtCurrentTime() {
    const double position = m_renderer.CurrentTime();
    HRESULT hr = m_renderer.Open(m_settings.video, m_settings.volume, position);
    if (FAILED(hr)) {
        std::wstring name = FileName(m_settings.video);
        StopWallpaper();
        ReportError(std::wstring(T(S::ErrPlayback)) + L"\n" + name, hr);
    }
}

void App::RecoverDevice() {
    const DWORD now = GetTickCount();
    if (now - m_lastDeviceReset > 60000) m_deviceResets = 0;
    m_lastDeviceReset = now;

    const bool wasActive = m_active;
    const double position = m_renderer.CurrentTime();
    StopWallpaper();
    m_renderer.Shutdown();
    if (++m_deviceResets > 3) {
        ReportError(T(S::ErrD3D), DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    HRESULT hr = m_renderer.Initialize(m_hwnd);
    if (FAILED(hr)) {
        ReportError(T(S::ErrD3D), hr);
        return;
    }
    if (wasActive) StartWallpaper(position);
}

void App::SelectVideo() {
    if (m_dialogOpen) return;
    m_dialogOpen = true;

    const wchar_t kPatterns[] = L"*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.wmv;*.avi";
    std::wstring filter;
    filter += T(S::FilterVideo);
    filter += L" (mp4, mkv, webm, mov, wmv, avi)";
    filter.push_back(L'\0');
    filter += kPatterns;
    filter.push_back(L'\0');
    filter += T(S::FilterAll);
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');

    std::wstring initialDir = FolderOf(m_settings.video);
    std::wstring file(32768, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    ofn.lpstrTitle = T(S::DialogTitle);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    SetForegroundWindow(m_hwnd);
    const BOOL chosen = GetOpenFileNameW(&ofn);
    m_dialogOpen = false;
    if (chosen) SetVideo(file.c_str());
}

void App::SetVideo(const std::wstring& path) {
    m_settings.video = path;
    m_settings.Save();
    m_userPaused = false;
    StartWallpaper(0.0);
}

void App::ReportError(const std::wstring& text, HRESULT hr) {
    std::wstring message = text;
    if (FAILED(hr)) message += L"\n" + HResultText(hr);
    Notify(message, true);
}

// ---------------------------------------------------------------------------
// Tray icon and menu

std::wstring App::StatusText() const {
    std::wstring text = T(S::AppName);
    if (m_settings.video.empty()) return text + L" — " + T(S::NoVideo);
    text += L" — " + FileName(m_settings.video) + L"\n";
    if (!m_active) return text + T(S::StatusStopped);
    if (m_reason == PauseReason::None) return text + T(S::StatusPlaying);

    S reason = S::ReasonUser;
    switch (m_reason) {
    case PauseReason::Locked: reason = S::ReasonLocked; break;
    case PauseReason::DisplayOff: reason = S::ReasonDisplayOff; break;
    case PauseReason::Battery: reason = S::ReasonBattery; break;
    case PauseReason::Fullscreen: reason = S::ReasonFullscreen; break;
    case PauseReason::Covered: reason = S::ReasonCovered; break;
    case PauseReason::NoDesktop: reason = S::ReasonNoDesktop; break;
    default: break;
    }
    return text + T(S::StatusPaused) + L": " + T(reason);
}

void App::AddTrayIcon() {
    if (m_trayAdded || !m_hwnd) return;
    if (!m_icon) {
        m_icon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR));
    }
    m_nid = {};
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = kTrayId;
    m_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    m_nid.uCallbackMessage = WM_APP_TRAY;
    m_nid.hIcon = m_icon ? m_icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
    CopyTruncated(m_nid.szTip, StatusText());
    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        SetTimer(m_hwnd, kTimerTrayRetry, 2000, nullptr); // the taskbar is not there yet
        return;
    }
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    m_trayAdded = true;
}

void App::UpdateTray() {
    if (!m_trayAdded) return;
    m_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    CopyTruncated(m_nid.szTip, StatusText());
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void App::Notify(const std::wstring& text, bool error) {
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    CopyTruncated(nid.szInfoTitle, T(S::AppName));
    CopyTruncated(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowMenu(POINT anchor) {
    if (m_menuOpen) return;
    m_menuOpen = true;

    auto add = [](HMENU menu, UINT id, const wchar_t* text, bool checked = false, bool enabled = true) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0u) | (enabled ? 0u : MF_GRAYED), id, text);
    };

    HMENU menu = CreatePopupMenu();
    std::wstring header = m_settings.video.empty() ? std::wstring(T(S::NoVideo)) : FileName(m_settings.video);
    if (header.size() > 60) header = header.substr(0, 57) + L"...";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, header.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(menu, kCmdSelect, T(S::MenuSelectVideo));
    add(menu, kCmdPause, T(S::MenuPause), m_userPaused, m_active);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU scaling = CreatePopupMenu();
    add(scaling, kCmdScaleBase + static_cast<UINT>(Scaling::Fill), T(S::ScaleFill));
    add(scaling, kCmdScaleBase + static_cast<UINT>(Scaling::Fit), T(S::ScaleFit));
    add(scaling, kCmdScaleBase + static_cast<UINT>(Scaling::Stretch), T(S::ScaleStretch));
    CheckMenuRadioItem(scaling, kCmdScaleBase, kCmdScaleBase + static_cast<UINT>(Scaling::Stretch),
                       kCmdScaleBase + static_cast<UINT>(m_settings.scaling), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(scaling), T(S::MenuScaling));

    HMENU sound = CreatePopupMenu();
    UINT volumeIndex = 0;
    for (UINT i = 0; i < kVolumeCount; ++i) {
        std::wstring label = kVolumes[i] == 0 ? std::wstring(T(S::SoundOff)) : std::to_wstring(kVolumes[i]) + L"%";
        add(sound, kCmdVolumeBase + i, label.c_str());
        if (std::abs(kVolumes[i] - m_settings.volume) < std::abs(kVolumes[volumeIndex] - m_settings.volume)) {
            volumeIndex = i;
        }
    }
    CheckMenuRadioItem(sound, kCmdVolumeBase, kCmdVolumeBase + kVolumeCount - 1, kCmdVolumeBase + volumeIndex,
                       MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sound), T(S::MenuSound));

    HMENU fps = CreatePopupMenu();
    const S fpsLabels[] = {S::FpsOriginal, S::Fps30, S::Fps15};
    UINT fpsIndex = 0;
    for (UINT i = 0; i < kFpsCount; ++i) {
        add(fps, kCmdFpsBase + i, T(fpsLabels[i]));
        if (kFpsLimits[i] == m_settings.fpsLimit) fpsIndex = i;
    }
    CheckMenuRadioItem(fps, kCmdFpsBase, kCmdFpsBase + kFpsCount - 1, kCmdFpsBase + fpsIndex, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fps), T(S::MenuFps));

    HMENU saving = CreatePopupMenu();
    add(saving, kCmdPauseFullscreen, T(S::PauseFullscreen), m_settings.pauseFullscreen);
    add(saving, kCmdPauseCovered, T(S::PauseCovered), m_settings.pauseCovered);
    add(saving, kCmdPauseBattery, T(S::PauseBattery), m_settings.pauseOnBattery);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(saving), T(S::MenuSaving));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(menu, kCmdAutostart, T(S::MenuAutostart), Autostart::IsEnabled());
    add(menu, kCmdRemove, T(S::MenuRemove), false, !m_settings.video.empty());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(menu, kCmdExit, T(S::MenuExit));

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(m_hwnd);
    const UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN |
                       (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, m_hwnd, nullptr));
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu); // also destroys the submenus
    m_menuOpen = false;

    if (command) OnCommand(command);
}
