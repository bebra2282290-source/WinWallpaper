// WinWallpaper: a lightweight video wallpaper for the Windows desktop.
//
// Usage:
//   WinWallpaper.exe [video file] [--autostart] [--exit] [--log]
//
// Only one instance runs at a time. Starting the program again passes the
// command line to the running instance: a file name switches the video,
// --exit closes it, and no arguments opens the "choose video" dialog.

#include "App.h"
#include "Strings.h"

#include <windows.h>
#include <mfapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

namespace {

std::wstring FullPath(const std::wstring& path) {
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) return path;
    std::wstring full(size, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    if (written == 0 || written >= size) return path;
    full.resize(written);
    return full;
}

// Forwards the command line to the instance that is already running.
void ForwardToRunningInstance(const std::wstring& video, bool exitRequested) {
    HWND other = nullptr;
    for (int attempt = 0; attempt < 20 && !other; ++attempt) {
        other = FindWindowW(kMainWindowClass, nullptr);
        if (!other) Sleep(100);
    }
    if (!other) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid); // so its file dialog can come to the front

    COPYDATASTRUCT data{};
    if (exitRequested) {
        data.dwData = kCopyExit;
    } else if (!video.empty()) {
        data.dwData = kCopySetVideo;
        data.cbData = static_cast<DWORD>((video.size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(video.c_str());
    } else {
        data.dwData = kCopyActivate;
    }
    SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000, nullptr);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    bool autostart = false;
    bool exitRequested = false;
    bool log = false;
    std::wstring video;

    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--autostart") {
                autostart = true;
            } else if (arg == L"--exit") {
                exitRequested = true;
            } else if (arg == L"--log") {
                log = true;
            } else if (video.empty() && !arg.empty() && arg[0] != L'-') {
                video = FullPath(arg);
            }
        }
        LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\WinWallpaper.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        ForwardToRunningInstance(video, exitRequested);
        CloseHandle(mutex);
        return 0;
    }
    if (exitRequested) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, T(S::ErrMediaFoundation), T(S::AppName), MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    {
        App app;
        exitCode = app.Run(instance, video, autostart, log);
    }

    MFShutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
