#include "controller_menu_math.h"
#include "controller_menu_protocol.h"
#include "screen_anchor.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <memory>

static bool Near(float a, float b) { return std::fabs(a-b) < 1e-4f; }

int main()
{
    using namespace vrxmenu;
    PressLatch toggle;
    assert(!toggle.Press(true)); // held while enabling or regaining focus
    assert(!toggle.Press(false));
    assert(toggle.Press(true));
    assert(!toggle.Press(true)); // held Menu never flickers the menu
    toggle.Reset();
    assert(!toggle.Press(true));
    assert(!toggle.Press(false));
    assert(toggle.Press(true)); // next deliberate press after focus returns
    constexpr float pi = 3.14159265358979323846f;
    XrPosef panel{{0,0,0,1},{0,0,-2}}, ray{{0,0,0,1},{0,0,0}};
    XrExtent2Df size{2,1};
    float u = 0, v = 0;
    assert(Hit(panel,size,ray,u,v) && Near(u,.5f) && Near(v,.5f));
    ray.position = {1,.5f,0};
    assert(Hit(panel,size,ray,u,v) && Near(u,1) && Near(v,0));
    ray.position.x = 2;
    assert(!Hit(panel,size,ray,u,v));
    assert(Hit(panel,size,ray,u,v,false) && u>1); // slider drag outside panel
    ray.position = {0,0,-3}; assert(!Hit(panel,size,ray,u,v)); // ray behind panel
    ray.position = {0,0,0}; ray.orientation = {0,1,0,0}; assert(!Hit(panel,size,ray,u,v));
    ray.orientation = {0,std::sin(pi/4),0,std::cos(pi/4)}; assert(!Hit(panel,size,ray,u,v)); // parallel
    ray.position.x = std::numeric_limits<float>::quiet_NaN(); assert(!Hit(panel,size,ray,u,v));

    // Opening follows the full gaze pose, including pitch, at a fixed 1.2 m.
    XrExtent2Df menuSize{.85f,.68f};
    for (float yaw : {0.0f,pi/3,-pi/2,pi}) for (float pitch : {-.6f,0.0f,.5f})
    {
        XrVector3f eye{2,1.7f,-4};
        XrQuaternionf heading{0,std::sin(yaw/2),0,std::cos(yaw/2)};
        heading = Multiply(heading,{std::sin(pitch/2),0,0,std::cos(pitch/2)});
        auto pose = Place({heading,eye});
        auto centre = Rotate(Inverse(heading),Sub(pose.position,eye));
        assert(Near(centre.x,0) && Near(centre.y,0) && Near(centre.z,-1.2f));
        XrPosef pointer{pose.orientation,eye};
        assert(Hit(pose,menuSize,pointer,u,v) && Near(u,.5f) && Near(v,.5f));
        // Inverse projection agrees with a point on the tilted panel.
        pointer.position = Add(eye,Rotate(pose.orientation,{.2f,.1f,0}));
        assert(Hit(pose,menuSize,pointer,u,v));
        assert(Near(u,.5f+.2f/menuSize.width) && Near(v,.5f-.1f/menuSize.height));

        // The initial grab must not jump; translating or turning the controller
        // carries the same title-bar point, without rotating the panel itself.
        pointer.position = Add(eye,Rotate(heading,{.1f,.30f,0}));
        assert(Hit(pose,menuSize,pointer,u,v) && TitleHit(u,v));
        const float grabU = u, grabV = v;
        PanelDrag drag; drag.Begin(pose,menuSize,pointer,u,v);
        const auto before = pose.position;
        drag.Move(pose,pointer);
        assert(Near(pose.position.x,before.x) && Near(pose.position.y,before.y) && Near(pose.position.z,before.z));
        pointer.position = Add(pointer.position,{.2f,.1f,-.05f}); drag.Move(pose,pointer);
        assert(Near(pose.position.x,before.x+.2f) && Near(pose.position.y,before.y+.1f) && Near(pose.position.z,before.z-.05f));
        pointer.orientation = Multiply({0,std::sin(.1f),0,std::cos(.1f)},pointer.orientation);
        drag.Move(pose,pointer);
        assert(Hit(pose,menuSize,pointer,u,v) && Near(u,grabU) && Near(v,grabV));
        assert(Near(pose.orientation.w,heading.w));

        // Beam endpoints coincide with the controller origin and its aim ray.
        const auto beam = Beam(pointer,1.5f,eye);
        const auto start = Add(beam.position,Rotate(beam.orientation,{0,-.75f,0}));
        const auto end = Add(beam.position,Rotate(beam.orientation,{0,.75f,0}));
        const auto expected = Add(pointer.position,Rotate(pointer.orientation,{0,0,-1.5f}));
        assert(Near(start.x,pointer.position.x) && Near(start.y,pointer.position.y) && Near(start.z,pointer.position.z));
        assert(Near(end.x,expected.x) && Near(end.y,expected.y) && Near(end.z,expected.z));
    }
    assert(TitleHit(.5f,50.0f/Height));
    assert(!TitleHit(.5f,100.0f/Height) && !TitleHit(.01f,50.0f/Height));
    const auto straightBeam = Beam({{0,0,0,1},{0,0,0}},2,{0,0,0});
    assert(std::isfinite(straightBeam.orientation.w)); // viewer exactly along ray
    // IPC layout and ordered events, including wraparound of the sequence.
    auto shared = std::make_unique<Shared>();
    shared->header.write = shared->header.read = UINT32_MAX-1;
    Push(*shared,{Down,.25f,.5f,0}); Push(*shared,{Up,.3f,.5f,0});
    assert(uint32_t(shared->header.write-shared->header.read) == 2);
    assert(shared->events[(UINT32_MAX-1)%EventCount].kind == Down);
    assert(shared->events[UINT32_MAX%EventCount].kind == Up);
    for (int i = 0; i < EventCount+1; ++i) Push(*shared,{Move,0,0,0});
    assert(uint32_t(shared->header.write-shared->header.read) > EventCount); // consumer must cancel drag
    puts("PASS: menu ray hits, gaze placement, title-bar drag, white beam geometry, invalid rays, IPC layout and event wrap/overflow");
}
