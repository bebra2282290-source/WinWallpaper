#pragma once

#include <windows.h>

#include <vector>

// How Explorer's desktop window tree is organised.
enum class DesktopLayout {
    None,
    // Windows 10 / Windows 11 up to 23H2: a top-level WorkerW that sits right
    // behind the window hosting the icons (SHELLDLL_DefView).
    Legacy,
    // Windows 11 24H2+: SHELLDLL_DefView and the wallpaper WorkerW are both
    // children of Progman. Our window must be a layered child of Progman,
    // z-ordered between the icons and the WorkerW.
    Raised,
    // Neither layout was found: child of Progman, behind the icon view.
    ProgmanFallback,
};

struct DesktopHost {
    DesktopLayout layout = DesktopLayout::None;
    HWND progman = nullptr;
    HWND parent = nullptr;  // the window our wallpaper windows are children of
    HWND defView = nullptr; // icon view; our windows go right below it (Raised / fallback)
    HWND workerW = nullptr; // Explorer's wallpaper layer (must stay below us in Raised)
};

// Finds (and if needed asks Explorer to create) the layer behind the icons.
// Blocks for up to ~2 s while Explorer builds the layer. With allowFallback,
// settles for a child of Progman when no WorkerW layer shows up.
bool FindDesktopHost(DesktopHost& host, bool allowFallback);

// Non-blocking check for a proper (Legacy or Raised) layout, without asking
// Explorer to create anything.
bool ProbeDesktopHost(DesktopHost& host);

// Creates a wallpaper window covering screenRect (in screen coordinates) and
// attaches it behind the desktop icons. Paint notifications go to `owner`.
// `layered` makes it an opaque layered window, which gets its own
// redirection surface (needed under Progman on the 24H2+ desktop).
HWND CreateWallpaperWindow(const DesktopHost& host, const RECT& screenRect, HINSTANCE instance, HWND owner,
                           bool layered);

const char* LayoutName(DesktopLayout layout);

// True while Explorer's windows we are attached to still exist and still own
// our windows. False after Explorer restarts or rebuilds the desktop.
bool IsHostIntact(const DesktopHost& host, const std::vector<HWND>& windows);

// Raised layout only: puts our windows back between the icons and the
// WorkerW if Explorer reordered them. Returns false if a fix was needed.
bool EnsureZOrder(const DesktopHost& host, const std::vector<HWND>& windows);

// Makes Explorer repaint the static wallpaper after our windows are gone.
void RefreshDesktop(const DesktopHost& host);
