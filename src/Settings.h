#pragma once

#include <string>

enum class Scaling : int {
    Fill = 0,    // cover the whole screen, cropping the video if needed
    Fit = 1,     // show the whole video, with black bars if needed
    Stretch = 2, // stretch to the screen, ignoring the aspect ratio
};

// How frames reach the screen (advanced, settings file only).
enum class PresentMode : int {
    Auto = 0,  // currently the flip model
    Flip = 1,  // DXGI flip-model swap chain on the window
    Blt = 2,   // DXGI blt-model swap chain on the window
    DComp = 3, // composition swap chain shown through DirectComposition
};

// Whether the wallpaper window is a layered window (advanced, settings file only).
enum class LayeredWindow : int {
    Auto = 0, // layered on the Windows 11 24H2+ desktop only
    Yes = 1,
    No = 2,
};

struct Settings {
    std::wstring video;
    Scaling scaling = Scaling::Fill;
    int volume = 0;   // 0 = sound off (the audio track is not decoded at all)
    int fpsLimit = 0; // 0 = as in the video
    bool pauseFullscreen = true;
    bool pauseCovered = true;
    bool pauseOnBattery = true;
    bool efficiencyMode = true; // low process priority + Windows "Efficiency mode" (EcoQoS)
    PresentMode presentMode = PresentMode::Auto;
    LayeredWindow layeredWindow = LayeredWindow::Auto;
    bool log = false;

    void Load();
    void Save() const;

    // Folder that holds the settings file (and the optional log).
    static std::wstring Directory();
};

namespace Autostart {
bool IsEnabled();
bool SetEnabled(bool enabled);
// Keeps the registered path valid if the program was moved to another folder.
void RefreshPath();
} // namespace Autostart
