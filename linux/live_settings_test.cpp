// The Linux live-settings snapshot: every version the desktop app has written
// still loads, and malformed or out-of-range snapshots are rejected.
#include "live_settings.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {
std::string path;

bool Read(const char* text, vrx::LiveSettings& out) {
    std::ofstream(path) << text;
    return vrx::ReadLiveSettings(path, out);
}
}

int main() {
    path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vrx-live-settings-test";
    vrx::LiveSettings s;
    // VRXL 1: screen and stereo; the room is off.
    assert(Read("VRXL 1 2.5 3.0 0.25 -0.5 1.2\n", s));
    assert(s.width == 2.5f && s.distance == 3.0f && s.height == 0.25f && s.horizontal == -0.5f && s.strength == 1.2f);
    assert(s.room == 0 && s.recenter == 0);
    // VRXL 2: room controls.
    assert(Read("VRXL 2 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867\n", s));
    assert(s.room == 30 && s.glass == 60 && s.reflect == 25 && s.light == 30 && s.lightRgb == 0xFFB46Bu && s.recenter == 0);
    // VRXL 3: the recenter counter.
    assert(Read("VRXL 3 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 7\n", s));
    assert(s.recenter == 7 && s.room == 30);
    // VRXL 4: the frame timing mode.
    assert(Read("VRXL 4 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 7 1\n", s));
    assert(s.timing == 1 && s.recenter == 7);
    assert(Read("VRXL 3 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 7\n", s) && s.timing == 1);   // the default
    // VRXL 5: the screen curve.
    assert(Read("VRXL 5 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 7 1 40\n", s));
    assert(s.curve == 40 && s.timing == 1);
    assert(Read("VRXL 4 2.000 2.000 0.000 0.000 1.000 30 60 25 30 16757867 7 1\n", s) && s.curve == 0);   // flat
    // Rejected, leaving the previous settings.
    const char* bad[] = {
        "VRXL 6 2 2 0 0 1 30 60 25 30 0 1 0 0\n",       // unknown version
        "VRXL 5 2 2 0 0 1 30 60 25 30 0 1 0 101\n",     // curve above range
        "VRXL 5 2 2 0 0 1 30 60 25 30 0 1 0\n",         // missing curve
        "VRXL 4 2 2 0 0 1 30 60 25 30 0 1 3\n",         // unknown timing mode
        "VRXL 4 2 2 0 0 1 30 60 25 30 0 1\n",           // missing timing mode
        "VRXL 3 2 2 0 0 1 30 60 25 30 0\n",             // missing counter
        "VRXL 2 2 2 0 0 1 30 60 25 30 0 5\n",           // extra field for version 2
        "VRXL 3 11 2 0 0 1 30 60 25 30 0 1\n",          // width above range
        "VRXL 3 2 2 0 0 1 101 60 25 30 0 1\n",          // room above range
        "VRXL 3 2 2 0 0 1 30 60 25 30 16777216 1\n",    // colour above 0xFFFFFF
        "VRX 11 2 2 0 0 1\n",                           // the Windows snapshot
    };
    for (const char* text : bad) assert(!Read(text, s) && s.recenter == 7);
    std::remove(path.c_str());
    std::puts("Live settings checks passed");
}
