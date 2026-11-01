cmake_minimum_required(VERSION 3.20)

project(WinWallpaper VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "WinWallpaper is a Windows application. Build it with MSVC (or a MinGW toolchain that targets Windows).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(WinWallpaper WIN32
    src/main.cpp
    src/App.cpp
    src/Activity.cpp
    src/Desktop.cpp
    src/Settings.cpp
    src/Strings.cpp
    src/VideoRenderer.cpp
    res/app.rc
)

target_compile_definitions(WinWallpaper PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_link_libraries(WinWallpaper PRIVATE
    d3d11 dxgi
    mfplat mfuuid
    dwmapi wtsapi32 comdlg32 shell32 ole32 oleaut32 uuid
    user32 gdi32 advapi32
)

if(MSVC)
    # Static CRT: a single .exe without the Visual C++ redistributable.
    set_property(TARGET WinWallpaper PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(WinWallpaper PRIVATE /W4 /utf-8 /permissive- /EHsc)
    # The manifest is embedded through app.rc.
    target_link_options(WinWallpaper PRIVATE /MANIFEST:NO $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
else()
    target_compile_options(WinWallpaper PRIVATE -Wall -Wextra)
    target_link_options(WinWallpaper PRIVATE -municode -static -s)
endif()
