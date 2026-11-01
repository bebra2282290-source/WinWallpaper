#pragma once

#include <windows.h>

// Window messages exchanged between components. All of them are delivered to
// the hidden main window, which lives on the UI thread.
constexpr UINT WM_APP_ENGINE_EVENT = WM_APP + 1;  // lParam: EngineEvent* (the receiver deletes it)
constexpr UINT WM_APP_DEVICE_LOST = WM_APP + 2;   // the GPU device was removed or reset
constexpr UINT WM_APP_SURFACE_PAINT = WM_APP + 3; // a wallpaper window was invalidated
constexpr UINT WM_APP_TRAY = WM_APP + 4;          // notification area icon callback
