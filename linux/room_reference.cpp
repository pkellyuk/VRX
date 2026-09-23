// Portable Linux reference fixture for the first-release room Vulkan passes.
#include "live_settings.h"
#include "room.h"
#include "synthetic_scene.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
bool Check(bool ok, const char* what) {
    if (!ok) std::fprintf(stderr, "Room reference failed: %s\n", what);
    return ok;
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::puts("vrx-room-reference: validate portable flat-room geometry, emitters, lightmap samples and mirror image");
        return 0;
    }
    if (argc != 1) return 2;

    const vrx::LiveSettings settings;
    const float height = settings.width * vrx::kSyntheticHeight / vrx::kSyntheticWidth;
    RoomInputs inputs;
    inputs.W = settings.width;
    inputs.H = height;
    inputs.eye[0] = -settings.horizontal;
    inputs.eye[1] = -settings.height;
    inputs.eye[2] = settings.distance;
    inputs.floorY = std::numeric_limits<float>::quiet_NaN();
    Room room;
    if (!Check(BuildRoom(inputs, room), "flat geometry") ||
        !Check(room.valid && RoomInside(room, inputs.eye), "viewer inside room") ||
        !Check(room.yF < -height * 0.5f && room.yC > height * 0.5f, "floor and ceiling clear screen"))
        return 1;
    const float originalFloor = room.yF;
    inputs.floorY = inputs.eye[1] - 1.7f;
    Room tracked;
    if (!Check(BuildRoom(inputs, tracked) && tracked.floorTracked, "STAGE floor accepted") ||
        !Check(tracked.yF < originalFloor, "tracked floor changes geometry"))
        return 1;

    // The same synthetic picture used by the Linux stereo reference, converted to
    // RGBA8 in the format expected by the Windows room CPU oracle.
    std::vector<unsigned char> rgb, rgba(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight * 4);
    vrx::MakeScene(rgb, 0.0);
    for (size_t i = 0; i < rgb.size() / 3; ++i) {
        rgba[i * 4] = rgb[i * 3];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    float decode[256];
    RoomDecodeTable(decode);
    const float glowHalfW = settings.width * 0.7f, glowHalfH = height * 0.7f;
    const auto layout = RoomLayout(settings.width, height, 64, 64);
    RoomView view;
    view.flatLayer = true;
    view.W = settings.width;
    view.H = height;
    view.glowHalfW = glowHalfW;
    view.glowHalfH = glowHalfH;
    std::vector<RoomEmitter> emitters;
    if (!Check(layout.count() > 0 && layout.count() <= kRoomMaxEmitters, "emitter budget") ||
        !Check(BuildRoomEmitters(room, view.cyl, settings.width, height,
                                 glowHalfW, glowHalfH, layout, emitters), "emitter geometry") ||
        !Check(RoomEmitRadiance(layout, rgba.data(), vrx::kSyntheticWidth,
                                vrx::kSyntheticHeight, vrx::kSyntheticWidth * 4,
                                nullptr, 0, false, 1.0f, decode, emitters), "picture emitter reduction"))
        return 1;
    RoomMirror mirror;
    float mirrorCenter[3];
    if (!Check(RoomMirrorPicture(rgba.data(), vrx::kSyntheticWidth, vrx::kSyntheticHeight,
                                 vrx::kSyntheticWidth * 4, decode, mirror), "mirror reduction") ||
        !Check(mirror.w == kRoomMirrorW && mirror.h > 0 &&
               mirror.Sample(0.5f, 0.5f, mirrorCenter), "mirror sample"))
        return 1;

    RoomLook diffuseLook;
    RoomLook finishLook;
    finishLook.glass = 60;
    finishLook.reflect = 25;
    finishLook.light = 30;
    const auto diffuse = MakeRoomShading(room, 30, 0, settings.width * height, diffuseLook);
    const auto finish = MakeRoomShading(room, 30, 0, settings.width * height, finishLook);
    if (!Check(!diffuse.finish && finish.finish && finish.lightOn, "finish and ceiling light") ||
        !Check(RoomFresnel(finish.reflect, 0.1f) > RoomFresnel(finish.reflect, 1.0f),
               "grazing reflection strength"))
        return 1;
    float floorDiffuse[3], floorFinished[3], wall[3];
    if (!Check(RoomTexel(room, diffuse, emitters, kFaceFloor, 32, 32, floorDiffuse), "diffuse floor lightmap"))
        return 1;
    for (int ch = 0; ch < 3; ++ch) emitters[size_t(layout.lampIndex())].L[ch] = finish.lightL[ch];
    if (!Check(RoomTexel(room, finish, emitters, kFaceFloor, 32, 32, floorFinished), "finished floor lightmap") ||
        !Check(RoomTexel(room, finish, emitters, kFaceBack, 32, 32, wall), "finished back wall lightmap") ||
        !Check(floorFinished[0] > floorDiffuse[0], "ceiling light reaches floor"))
        return 1;
    for (float value : {floorDiffuse[0], floorFinished[0], wall[0], mirrorCenter[0]})
        if (!Check(std::isfinite(value) && value >= 0, "finite nonnegative radiance")) return 1;
    std::printf("Room reference: %.2f x %.2f m, floor %.2f m, %d emitters, mirror %dx%d\n",
                room.X * 2, room.zB + room.g, room.yF, layout.count(), mirror.w, mirror.h);
    std::printf("Room samples: floor diffuse %.6f, finish %.6f, back wall %.6f, mirror %.6f\n",
                floorDiffuse[0], floorFinished[0], wall[0], mirrorCenter[0]);
    return 0;
}
