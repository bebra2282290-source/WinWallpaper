#pragma once

#include <windows.h>

#include <vector>

// Cheap checks that decide when the wallpaper does not need to play.
namespace Activity {

// A game or another app runs fullscreen (exclusive or borderless), or
// Windows presentation mode is on.
bool IsFullscreenAppActive();

// For each area (monitor work area, screen coordinates), whether opaque
// windows cover it completely, i.e. nothing of the desktop is visible.
void ComputeCoverage(const std::vector<RECT>& areas, std::vector<bool>& covered);

// Running on battery, or Windows battery/energy saver is on.
bool IsPowerSaving();

} // namespace Activity
