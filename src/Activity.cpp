#include "Activity.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <cwchar>

namespace Activity {
namespace {

bool IsShellWindow(HWND hwnd) {
    wchar_t cls[64];
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsOwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// Windows on other virtual desktops and suspended UWP apps are "cloaked":
// visible by style but not on screen.
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// Overlays (GPU driver overlays, recording tools, etc.) are transparent or
// semi-transparent layered windows; they do not hide the desktop.
bool IsSeeThrough(HWND hwnd) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return true;
    if (ex & WS_EX_LAYERED) {
        COLORREF key = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) return true; // per-pixel alpha
        if (flags & LWA_COLORKEY) return true;
        if ((flags & LWA_ALPHA) && alpha < 255) return true;
    }
    return false;
}

bool GetVisibleBounds(HWND hwnd, RECT& rc) {
    // Excludes the invisible resize borders of Windows 10/11 windows.
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) return true;
    return GetWindowRect(hwnd, &rc) != FALSE;
}

struct CoverageContext {
    std::vector<RECT> areas;
    std::vector<HRGN> remaining;
    size_t uncovered;
};

BOOL CALLBACK CoverageProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<CoverageContext*>(param);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (IsShellWindow(hwnd) || IsOwnWindow(hwnd) || IsSeeThrough(hwnd) || IsCloaked(hwnd)) return TRUE;

    RECT bounds;
    if (!GetVisibleBounds(hwnd, bounds) || IsRectEmpty(&bounds)) return TRUE;

    HRGN windowRgn = nullptr;
    for (size_t i = 0; i < ctx->areas.size(); ++i) {
        if (!ctx->remaining[i]) continue; // already fully covered
        RECT overlap;
        if (!IntersectRect(&overlap, &bounds, &ctx->areas[i])) continue;
        if (!windowRgn) windowRgn = CreateRectRgnIndirect(&bounds);
        if (!windowRgn) break;
        if (CombineRgn(ctx->remaining[i], ctx->remaining[i], windowRgn, RGN_DIFF) == NULLREGION) {
            DeleteObject(ctx->remaining[i]);
            ctx->remaining[i] = nullptr;
            --ctx->uncovered;
        }
    }
    if (windowRgn) DeleteObject(windowRgn);
    return ctx->uncovered > 0 ? TRUE : FALSE; // stop early once everything is covered
}

} // namespace

bool IsFullscreenAppActive() {
    HWND fg = GetForegroundWindow();
    // The desktop itself spans the whole screen; never treat it as fullscreen.
    if (fg && IsShellWindow(fg)) return false;

    QUERY_USER_NOTIFICATION_STATE state;
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY || state == QUNS_PRESENTATION_MODE)) {
        return true;
    }

    // Borderless fullscreen windows are not always reported above.
    if (!fg || IsOwnWindow(fg) || !IsWindowVisible(fg) || IsIconic(fg)) return false;
    LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if (IsZoomed(fg) && (style & WS_CAPTION) == WS_CAPTION) return false; // an ordinary maximized window
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

void ComputeCoverage(const std::vector<RECT>& areas, std::vector<bool>& covered) {
    covered.assign(areas.size(), false);
    if (areas.empty()) return;

    CoverageContext ctx;
    ctx.areas = areas;
    ctx.remaining.resize(areas.size(), nullptr);
    ctx.uncovered = 0;
    std::vector<bool> tracked(areas.size(), false);
    for (size_t i = 0; i < areas.size(); ++i) {
        // A couple of pixels of slack absorbs rounded corners and snap gaps.
        InflateRect(&ctx.areas[i], -2, -2);
        if (IsRectEmpty(&ctx.areas[i])) continue;
        ctx.remaining[i] = CreateRectRgnIndirect(&ctx.areas[i]);
        if (ctx.remaining[i]) {
            tracked[i] = true;
            ++ctx.uncovered;
        }
    }
    if (ctx.uncovered > 0) EnumWindows(CoverageProc, reinterpret_cast<LPARAM>(&ctx));

    for (size_t i = 0; i < areas.size(); ++i) {
        covered[i] = tracked[i] && ctx.remaining[i] == nullptr;
        if (ctx.remaining[i]) DeleteObject(ctx.remaining[i]);
    }
}

bool IsPowerSaving() {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) return false;
    return status.ACLineStatus == 0 || status.SystemStatusFlag == 1;
}

} // namespace Activity
