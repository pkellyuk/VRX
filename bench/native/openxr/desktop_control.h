#pragma once
#include <windows.h>
#include <cmath>
#include <sstream>
#include <string>

// Versioned, complete snapshots written atomically by the desktop application.
// Only the render thread reads/applies settings; the model choice is handed to the
// inference worker, which reloads the model between passes.
struct DesktopSettings
{
    float width = 5.7f, distance = 3, height = 0, horizontal = 0, strength = 1;
    int follow = 0, stereo = 1, autoDismiss = 1, recenterKey = VK_OEM_PLUS, menuKey = VK_F8;
    unsigned recenter = 0, menu = 0;
    int stop = 0;
    int foreground = 0; // v1 launchers retain their original behaviour
    int paired = 0;
    int fastModel = 0;
    int version = 0;    // snapshot version, so v1-v3 launchers do not override --model  // v4: 1 = ZipDepth, 0 = Depth Anything V2 (worker reloads live)
    int steady = 0;     // v5: steady depth (motion-vector steadying), live
    int fuse = 0;       // v5: fuse with Depth Anything V2, live
    int delayed = 0;    // v6: game frame timing "delayed to depth" (paired wins if both), live
    int subpixel = 1;   // v7: sub-pixel warp (no depth banding), live; v1-v6 keep it on
};

inline bool ParseDesktopSettings(const std::string& text, DesktopSettings& result)
{
    std::istringstream input(text);
    DesktopSettings s;
    std::string magic, tail; int version = 0;
    if (!(input >> magic >> version >> s.width >> s.distance >> s.height >> s.horizontal >> s.strength
        >> s.follow >> s.stereo >> s.autoDismiss >> s.recenterKey >> s.menuKey >> s.recenter >> s.menu >> s.stop)) return false;
    if (version >= 2 && !(input >> s.foreground)) return false;
    if (version >= 3 && !(input >> s.paired)) return false;
    if (version >= 4 && !(input >> s.fastModel)) return false;
    if (version >= 5 && !(input >> s.steady >> s.fuse)) return false;
    if (version >= 6 && !(input >> s.delayed)) return false;
    if (version >= 7 && !(input >> s.subpixel)) return false;
    s.version = version;
    if (input >> tail) return false;
    auto between = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (magic != "VRX" || version < 1 || version > 7 || s.steady < 0 || s.steady > 1 || s.fuse < 0 || s.fuse > 1 || s.delayed < 0 || s.delayed > 1 ||
        s.subpixel < 0 || s.subpixel > 1 || !between(s.width, 1, 10) || !between(s.distance, 1, 8) ||
        !between(s.height, -2, 2) || !between(s.horizontal, -3, 3) || !between(s.strength, 0, 2) ||
        s.follow < 0 || s.follow > 1 || s.stereo < 0 || s.stereo > 1 || s.autoDismiss < 0 || s.autoDismiss > 1 ||
        s.stop < 0 || s.stop > 1 || s.foreground < 0 || s.foreground > 1 || s.paired < 0 || s.paired > 1 || s.fastModel < 0 || s.fastModel > 1 || s.recenterKey < 1 || s.recenterKey > 254 ||
        s.menuKey < 1 || s.menuKey > 254 || s.recenterKey == s.menuKey) return false;
    result = s;
    return true;
}

inline bool ReadDesktopSettings(const std::wstring& path, DesktopSettings& result)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char data[2048]; DWORD length = 0;
    const bool ok = ReadFile(file, data, sizeof(data), &length, nullptr) && length < sizeof(data);
    CloseHandle(file);
    return ok && ParseDesktopSettings(std::string(data, length), result);
}
