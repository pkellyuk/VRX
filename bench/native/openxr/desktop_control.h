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
    int curve = 0;      // v8: curved screen, 0 flat .. 100 fully wrapped, live
    int ambilight = 0;  // v8: glow around the screen from the picture's edges, live
    int ambiStrength = 85;  // v9: the glow's brightness next to the screen, percent, live
    int worldColor = 0;     // v9: 0xRRGGBB around the screen (0 = black), live
    int room = 0;           // v10: the room lit by the screen, 0 off .. 100 (wall brightness), live
    int roomGlass = 0;      // v11: glass walls, 0 solid .. 100 clear, live
    int roomReflect = 0;    // v11: reflections, 0 off .. 100, live
    int roomLight = 0;      // v11: the ceiling panel light, 0 off .. 100, live
    int roomLightColor = 0xFFB46B; // v11: the room light colour 0xRRGGBB, live
    int edge = 0;           // v12: the picture's left/right edges flattened to the screen plane over this percent of its width
                            // (each side, 0 off .. 15), live; v1-v11 have it off
    int edgeSteps = 0;      // v13: depth edges across the picture drawn as steps (1) or as before, slanted (0), live; v1-v12 slanted
    int edgeStepMin = 12;   // v13: the least depth change made a step, percent of the depth range, 5 .. 60, live
    int widen = 100;        // v13: near objects widened up and down, percent of the model's own amount, 0 .. 300, live
    int thin = 0;           // v14: thin upright parts given the depth of what they stand on (thin_parts.h), live
    int refine = 0;         // v15: depth edges sharpened by the picture (depth_refine.h), live
    int showDepthMap = 0;   // v16: the depth map shown in place of the picture (a live diagnostic)
    int layer = 0;          // v17: what near objects uncover shows the background layer (back_layer.h), live
    int env = 0;            // v18: room/cinema/open air/lounge; v19 also theatre/dungeon/hell/cyberpunk/starship (room.h kRoomEnv*), live;
                            // v19 adds 4, the theatre (cinema with seats)
    int seat = 1;           // v19: the theatre's seat, 0 front, 1 middle, 2 rear (room.h kRoomSeat*), live; v1-v18 the middle
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
    if (version >= 8 && !(input >> s.curve >> s.ambilight)) return false;
    if (version >= 9 && !(input >> s.ambiStrength >> s.worldColor)) return false;
    if (version >= 10 && !(input >> s.room)) return false;
    if (version >= 11 && !(input >> s.roomGlass >> s.roomReflect >> s.roomLight >> s.roomLightColor)) return false;
    if (version >= 12 && !(input >> s.edge)) return false;
    if (version >= 13 && !(input >> s.edgeSteps >> s.edgeStepMin >> s.widen)) return false;
    if (version >= 14 && !(input >> s.thin)) return false;
    if (version >= 15 && !(input >> s.refine)) return false;
    if (version >= 16 && !(input >> s.showDepthMap)) return false;
    if (version >= 17 && !(input >> s.layer)) return false;
    if (version >= 18 && !(input >> s.env)) return false;
    if (version >= 19 && !(input >> s.seat)) return false;
    s.version = version;
    if (input >> tail) return false;
    auto between = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (magic != "VRX" || version < 1 || version > 19 || s.env < 0 || s.env > (version >= 19 ? 8 : 3) || s.seat < 0 || s.seat > 2 || s.layer < 0 || s.layer > 1 || s.showDepthMap < 0 || s.showDepthMap > 1 || s.refine < 0 || s.refine > 1 || s.thin < 0 || s.thin > 1 || s.edge < 0 || s.edge > 15 || s.edgeSteps < 0 || s.edgeSteps > 1 || s.edgeStepMin < 5 || s.edgeStepMin > 60 || s.widen < 0 || s.widen > 300 || s.steady < 0 || s.steady > 1 || s.fuse < 0 || s.fuse > 1 || s.delayed < 0 || s.delayed > 1 ||
        s.subpixel < 0 || s.subpixel > 1 || s.curve < 0 || s.curve > 100 || s.ambilight < 0 || s.ambilight > 1 ||
        s.ambiStrength < 0 || s.ambiStrength > 100 || s.worldColor < 0 || s.worldColor > 0xFFFFFF || s.room < 0 || s.room > 100 ||
        s.roomGlass < 0 || s.roomGlass > 100 || s.roomReflect < 0 || s.roomReflect > 100 || s.roomLight < 0 || s.roomLight > 100 ||
        s.roomLightColor < 0 || s.roomLightColor > 0xFFFFFF ||
        !between(s.width, 1, 10) || !between(s.distance, 1, 8) ||
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
