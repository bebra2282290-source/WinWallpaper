#include "Settings.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>

namespace {

constexpr wchar_t kSection[] = L"WinWallpaper";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WinWallpaper";

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring ExeDir() {
    std::wstring exe = ExePath();
    size_t slash = exe.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash + 1);
}

// Settings live in %APPDATA%\WinWallpaper\settings.ini. If WinWallpaper.ini
// exists next to the executable, it is used instead (portable mode).
const std::wstring& IniPath() {
    static const std::wstring path = [] {
        std::wstring portable = ExeDir() + L"WinWallpaper.ini";
        if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;

        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
            std::wstring dir = std::wstring(appData) + L"\\WinWallpaper";
            CoTaskMemFree(appData);
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir + L"\\settings.ini";
        }
        return portable;
    }();
    return path;
}

// The profile API writes UTF-16 only into files that already start with a
// UTF-16 BOM; otherwise non-ASCII paths (e.g. Cyrillic folder names) get lost.
void EnsureUnicodeFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return; // already exists (or cannot be created)
    const BYTE bom[] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(file, bom, sizeof(bom), &written, nullptr);
    CloseHandle(file);
}

int ReadInt(const wchar_t* key, int fallback, int lo, int hi) {
    int value = static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, IniPath().c_str()));
    return std::clamp(value, lo, hi);
}

bool ReadBool(const wchar_t* key, bool fallback) {
    return ReadInt(key, fallback ? 1 : 0, 0, 1) != 0;
}

void WriteString(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), IniPath().c_str());
}

void WriteInt(const wchar_t* key, int value) {
    WriteString(key, std::to_wstring(value));
}

std::wstring AutostartCommand() {
    return L"\"" + ExePath() + L"\" --autostart";
}

bool ReadRunValue(std::wstring& value) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    value.assign(size / sizeof(wchar_t) + 1, L'\0');
    size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return false;
    }
    value.resize(wcslen(value.c_str()));
    return true;
}

} // namespace

void Settings::Load() {
    const std::wstring& ini = IniPath();
    std::wstring buffer(32768, L'\0');
    DWORD n = GetPrivateProfileStringW(kSection, L"Video", L"", buffer.data(), static_cast<DWORD>(buffer.size()), ini.c_str());
    video.assign(buffer.c_str(), n);

    scaling = static_cast<Scaling>(ReadInt(L"Scaling", static_cast<int>(Scaling::Fill), 0, 2));
    volume = ReadInt(L"Volume", 0, 0, 100);
    fpsLimit = ReadInt(L"FpsLimit", 0, 0, 240);
    pauseFullscreen = ReadBool(L"PauseFullscreen", true);
    pauseCovered = ReadBool(L"PauseCovered", true);
    pauseOnBattery = ReadBool(L"PauseOnBattery", true);
    efficiencyMode = ReadBool(L"EfficiencyMode", true);
    presentMode = static_cast<PresentMode>(ReadInt(L"PresentMode", 0, 0, 2));
}

void Settings::Save() const {
    EnsureUnicodeFile(IniPath());
    // Quotes keep leading/trailing spaces; the profile API strips them on read.
    WriteString(L"Video", L"\"" + video + L"\"");
    WriteInt(L"Scaling", static_cast<int>(scaling));
    WriteInt(L"Volume", volume);
    WriteInt(L"FpsLimit", fpsLimit);
    WriteInt(L"PauseFullscreen", pauseFullscreen ? 1 : 0);
    WriteInt(L"PauseCovered", pauseCovered ? 1 : 0);
    WriteInt(L"PauseOnBattery", pauseOnBattery ? 1 : 0);
    WriteInt(L"EfficiencyMode", efficiencyMode ? 1 : 0);
    WriteInt(L"PresentMode", static_cast<int>(presentMode));
}

namespace Autostart {

bool IsEnabled() {
    std::wstring value;
    return ReadRunValue(value) && !value.empty();
}

bool SetEnabled(bool enabled) {
    if (!enabled) {
        LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    std::wstring command = AutostartCommand();
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

void RefreshPath() {
    std::wstring value;
    if (ReadRunValue(value) && !value.empty() && value != AutostartCommand()) SetEnabled(true);
}

} // namespace Autostart
