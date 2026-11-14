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
    assert(!toggle.Press(true)); // held X/A never flickers the menu
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

    // The normal screen's bottom is 28 degrees down. The entire console must
    // remain below it, even after a yaw recenter or changing world position.
    XrExtent2Df menuSize{.85f,.68f};
    const float bottom = 28*pi/180;
    for (float yaw : {0.0f,pi/3,-pi/2,pi})
    {
        XrVector3f eye{2,1.7f,-4};
        XrQuaternionf heading{0,std::sin(yaw/2),0,std::cos(yaw/2)};
        const auto pose = Place(eye,heading,bottom,menuSize);
        auto centre = Rotate(Inverse(heading),Sub(pose.position,eye));
        const float down = std::atan2(-centre.y,-centre.z);
        const float half = std::atan2(menuSize.height/2,1.2f);
        assert(down-half >= bottom+4.99f*pi/180);
        assert(down >= 40*pi/180 && down < 55*pi/180);
        XrPosef pointer{pose.orientation,eye};
        assert(Hit(pose,menuSize,pointer,u,v) && Near(u,.5f) && Near(v,.5f));
        // Inverse projection agrees with a point on the tilted panel.
        pointer.position = Add(eye,Rotate(pose.orientation,{.2f,.1f,0}));
        assert(Hit(pose,menuSize,pointer,u,v));
        assert(Near(u,.5f+.2f/menuSize.width) && Near(v,.5f-.1f/menuSize.height));
    }
    // IPC layout and ordered events, including wraparound of the sequence.
    auto shared = std::make_unique<Shared>();
    shared->header.write = shared->header.read = UINT32_MAX-1;
    Push(*shared,{Down,.25f,.5f,0}); Push(*shared,{Up,.3f,.5f,0});
    assert(uint32_t(shared->header.write-shared->header.read) == 2);
    assert(shared->events[(UINT32_MAX-1)%EventCount].kind == Down);
    assert(shared->events[UINT32_MAX%EventCount].kind == Up);
    for (int i = 0; i < EventCount+1; ++i) Push(*shared,{Move,0,0,0});
    assert(uint32_t(shared->header.write-shared->header.read) > EventCount); // consumer must cancel drag
    puts("PASS: menu ray hits, drag bounds, invalid rays, placement below screen, recenter, IPC layout and event wrap/overflow");
}
