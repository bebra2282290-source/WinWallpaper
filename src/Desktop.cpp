#include "Desktop.h"

#include "Messages.h"

#include <algorithm>
#include <climits>

namespace {

constexpr wchar_t kSurfaceClass[] = L"WinWallpaper.Surface";

// Undocumented Progman message that makes Explorer create the WorkerW
// wallpaper layer. (0xD, 1) works on all supported versions; other parameter
// combinations can tear the freshly created layer down again on 24H2+.
constexpr UINT kSpawnWorkerW = 0x052C;

bool SameThread(HWND a, HWND b) {
    return GetWindowThreadProcessId(a, nullptr) == GetWindowThreadProcessId(b, nullptr);
}

LONGLONG WindowArea(HWND hwnd) {
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return 0;
    return static_cast<LONGLONG>(rc.right - rc.left) * (rc.bottom - rc.top);
}

// Explorer keeps a few tiny helper WorkerW windows around; the wallpaper layer
// is the large one.
HWND FindLargestWorkerWChild(HWND progman) {
    HWND best = nullptr;
    LONGLONG bestArea = 0;
    for (HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); w;
         w = FindWindowExW(progman, w, L"WorkerW", nullptr)) {
        LONGLONG area = WindowArea(w);
        if (area > bestArea) {
            best = w;
            bestArea = area;
        }
    }
    return best;
}

struct LegacySearch {
    HWND progman;
    HWND workerW;
};

BOOL CALLBACK FindLegacyWorkerW(HWND top, LPARAM param) {
    auto* search = reinterpret_cast<LegacySearch*>(param);
    if (!FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
    // The WorkerW right behind the window that hosts the icons is the wallpaper layer.
    HWND candidate = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
    if (candidate && SameThread(candidate, search->progman) && IsWindowVisible(candidate) &&
        !FindWindowExW(candidate, nullptr, L"SHELLDLL_DefView", nullptr) && WindowArea(candidate) > 0) {
        search->workerW = candidate;
        return FALSE;
    }
    return TRUE;
}

bool Probe(HWND progman, DesktopHost& host, bool allowLegacy) {
    HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        if (HWND workerW = FindLargestWorkerWChild(progman)) {
            host = {DesktopLayout::Raised, progman, progman, defView, workerW};
            return true;
        }
    }
    if (allowLegacy) {
        LegacySearch search{progman, nullptr};
        EnumWindows(FindLegacyWorkerW, reinterpret_cast<LPARAM>(&search));
        if (search.workerW) {
            host = {DesktopLayout::Legacy, progman, search.workerW, nullptr, search.workerW};
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // never let GDI paint over the video
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        // Ask the renderer to present the current frame again (matters while paused).
        if (HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            PostMessageW(owner, WM_APP_SURFACE_PAINT, 0, 0);
        }
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void RegisterSurfaceClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SurfaceProc;
    wc.hInstance = instance;
    wc.lpszClassName = kSurfaceClass;
    registered = RegisterClassExW(&wc) != 0;
}

RECT ToParentClient(HWND parent, const RECT& screenRect) {
    RECT rc = screenRect;
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rc), 2);
    return rc;
}

} // namespace

bool FindDesktopHost(DesktopHost& host, bool allowFallback) {
    host = {};
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return false;

    // On 24H2+ the layer may already exist (created earlier by us or by
    // Explorer itself). Do not send the spawn message again in that case.
    if (Probe(progman, host, false)) return true;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(progman, kSpawnWorkerW, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (Probe(progman, host, true)) return true;
        Sleep(100);
    }

    if (!allowFallback) {
        host = {};
        return false;
    }
    host = {DesktopLayout::ProgmanFallback, progman, progman,
            FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr), nullptr};
    return true;
}

bool ProbeDesktopHost(DesktopHost& host) {
    host = {};
    HWND progman = FindWindowW(L"Progman", nullptr);
    return progman && Probe(progman, host, true);
}

HWND CreateWallpaperWindow(const DesktopHost& host, const RECT& screenRect, HINSTANCE instance, HWND owner,
                           bool layered) {
    if (!host.parent) return nullptr;
    RegisterSurfaceClass(instance);

    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    HWND hwnd = nullptr;

    if (layered) {
        // Microsoft's guidance for the 24H2+ desktop: an opaque WS_EX_LAYERED
        // child of Progman, z-ordered under SHELLDLL_DefView. It is created as
        // a top-level window first, made opaque, then re-parented.
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kSurfaceClass, L"", WS_POPUP,
                               screenRect.left, screenRect.top, width, height, nullptr, nullptr, instance, nullptr);
        if (!hwnd) return nullptr;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD);
        if (!SetParent(hwnd, host.parent)) {
            DestroyWindow(hwnd);
            return nullptr;
        }
    } else {
        RECT rc = ToParentClient(host.parent, screenRect);
        hwnd = CreateWindowExW(WS_EX_NOACTIVATE, kSurfaceClass, L"", WS_CHILD | WS_CLIPSIBLINGS, rc.left, rc.top,
                               width, height, host.parent, nullptr, instance, nullptr);
        if (!hwnd) return nullptr;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));

    RECT rc = ToParentClient(host.parent, screenRect);
    HWND insertAfter = HWND_TOP;
    if (host.layout != DesktopLayout::Legacy) insertAfter = host.defView ? host.defView : HWND_BOTTOM;
    SetWindowPos(hwnd, insertAfter, rc.left, rc.top, width, height, SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (host.layout == DesktopLayout::Raised && host.workerW) {
        SetWindowPos(host.workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    ShowWindow(hwnd, SW_SHOWNA);
    return hwnd;
}

bool IsHostIntact(const DesktopHost& host, const std::vector<HWND>& windows) {
    if (host.layout == DesktopLayout::None || !IsWindow(host.parent)) return false;
    if (FindWindowW(L"Progman", nullptr) != host.progman) return false;
    switch (host.layout) {
    case DesktopLayout::Legacy:
        if (!IsWindow(host.workerW)) return false;
        break;
    case DesktopLayout::Raised:
        if (!IsWindow(host.workerW) || !IsWindow(host.defView)) return false;
        if (GetAncestor(host.defView, GA_PARENT) != host.progman) return false;
        break;
    default:
        break;
    }
    for (HWND w : windows) {
        if (!IsWindow(w) || GetAncestor(w, GA_PARENT) != host.parent) return false;
    }
    return true;
}

bool EnsureZOrder(const DesktopHost& host, const std::vector<HWND>& windows) {
    if (host.layout != DesktopLayout::Raised || windows.empty()) return true;

    // Expected sibling order, top to bottom: SHELLDLL_DefView, our windows, WorkerW.
    int index = 0, defViewIndex = -1, workerWIndex = -1, firstOurs = INT_MAX, lastOurs = -1;
    for (HWND child = GetWindow(host.parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT), ++index) {
        if (child == host.defView) {
            defViewIndex = index;
        } else if (child == host.workerW) {
            workerWIndex = index;
        } else if (std::find(windows.begin(), windows.end(), child) != windows.end()) {
            firstOurs = std::min(firstOurs, index);
            lastOurs = std::max(lastOurs, index);
        }
    }
    if (defViewIndex >= 0 && workerWIndex >= 0 && firstOurs > defViewIndex && workerWIndex > lastOurs) return true;

    for (HWND w : windows) {
        SetWindowPos(w, host.defView, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetWindowPos(host.workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return false;
}

const char* LayoutName(DesktopLayout layout) {
    switch (layout) {
    case DesktopLayout::Legacy: return "legacy (WorkerW behind the icons)";
    case DesktopLayout::Raised: return "raised (Windows 11 24H2+, children of Progman)";
    case DesktopLayout::ProgmanFallback: return "fallback (child of Progman)";
    default: return "none";
    }
}

void RefreshDesktop(const DesktopHost& host) {
    // Only the classic layout needs a nudge. On 24H2+ the system wallpaper
    // layer is composed by DWM, and forcing Progman to repaint can paint over
    // the icons.
    if (host.layout == DesktopLayout::Legacy && IsWindow(host.workerW)) {
        RedrawWindow(host.workerW, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}
