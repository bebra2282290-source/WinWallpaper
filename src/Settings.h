#pragma once

#include <string>

enum class Scaling : int {
    Fill = 0,    // cover the whole screen, cropping the video if needed
    Fit = 1,     // show the whole video, with black bars if needed
    Stretch = 2, // stretch to the screen, ignoring the aspect ratio
};

// How frames reach the screen. "Auto" picks the flip model on the classic
// desktop and the blt model on the Windows 11 24H2+ desktop, where the
// wallpaper window has to be a layered window.
enum class PresentMode : int {
    Auto = 0,
    Flip = 1,
    Blt = 2,
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

    void Load();
    void Save() const;
};

namespace Autostart {
bool IsEnabled();
bool SetEnabled(bool enabled);
// Keeps the registered path valid if the program was moved to another folder.
void RefreshPath();
} // namespace Autostart
