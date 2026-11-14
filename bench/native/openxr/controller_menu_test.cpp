#include "controller_menu_math.h"
#include "controller_menu_motion.h"
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
        // carries the same title-bar point. Zero elapsed time must not jump it.
        pointer.position = Add(eye,Rotate(heading,{.1f,.28f,0}));
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
    assert(TitleHit(.5f,float(TitleTop+2)/Height));
    assert(!TitleHit(.5f,float(TitleTop+TitleHeight+10)/Height) && !TitleHit(.01f,float(TitleTop+2)/Height));
    const auto straightBeam = Beam({{0,0,0,1},{0,0,0}},2,{0,0,0});
    assert(std::isfinite(straightBeam.orientation.w)); // viewer exactly along ray
    // A tilted sheet pinched off-centre hangs with its centre below the grab.
    // The exact grabbed point is preserved during every physics step, including
    // wrist turns and movement, at common headset frame rates.
    for (float hz : {72.0f,90.0f,120.0f})
    {
        const float dt = 1/hz;
        XrPosef sheet{{std::sin(.3f),0,0,std::cos(.3f)},{0,0,-1.2f}};
        const float gu = .7f, gv = .06f;
        const XrVector3f localGrab{(gu-.5f)*menuSize.width,(.5f-gv)*menuSize.height,0};
        const auto pin = Add(sheet.position,Rotate(sheet.orientation,localGrab));
        XrPosef hand{sheet.orientation,Add(pin,Rotate(sheet.orientation,{0,0,1}))};
        PanelDrag drag; drag.Begin(sheet,menuSize,hand,gu,gv);
        for (int i = 0; i < int(20*hz); ++i)
        {
            drag.Move(sheet,hand,dt);
            const auto held = Add(sheet.position,Rotate(sheet.orientation,localGrab));
            const auto pivot = drag.Point(hand);
            assert(Near(held.x,pivot.x) && Near(held.y,pivot.y) && Near(held.z,pivot.z));
            const auto q = sheet.orientation;
            assert(Near(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w,1));
        }
        auto radius = Sub(sheet.position,drag.Point(hand));
        assert(std::fabs(radius.x) < .003f && std::fabs(radius.z) < .003f && radius.y < -.3f);
        // Moving the pinch point excites a swing, without detaching the sheet.
        for (int i = 0; i < int(hz*.5f); ++i)
        {
            hand.position.x += dt*.6f;
            drag.Move(sheet,hand,dt);
        }
        radius = Sub(sheet.position,drag.Point(hand));
        assert(std::fabs(radius.x) > .005f);
        // Wrist yaw can rotate the hanging sheet independently of gravity.
        const auto oldNormal = Rotate(sheet.orientation,{0,0,1});
        hand.orientation = Multiply({0,std::sin(.4f),0,std::cos(.4f)},hand.orientation);
        for (int i = 0; i < int(3*hz); ++i) drag.Move(sheet,hand,dt);
        const auto normal = Rotate(sheet.orientation,{0,0,1});
        assert(Dot(oldNormal,normal) < .9f);
    }
    // The back stays readable and pickable after turning the sheet around.
    XrPosef front{{0,0,0,1},{0,0,-2}}, backRay{{0,1,0,0},{-.2f,.2f,-3}};
    assert(Hit(front,menuSize,backRay,u,v,true,true));
    assert(Near(u,.5f+.2f/menuSize.width));
    PanelDrag backDrag; backDrag.Begin(front,menuSize,backRay,u,v);
    const auto oldCentre = front.position;
    backDrag.Move(front,backRay);
    assert(Near(front.position.x,oldCentre.x) && Near(front.position.y,oldCentre.y) && Near(front.position.z,oldCentre.z));
    // Right-stick depth control has a dead zone, a predictable speed and bounds.
    PanelDrag depth; depth.distance = 1;
    depth.PushPull(.15f,.05f); assert(Near(depth.distance,1));
    for (int i = 0; i < 90; ++i) depth.PushPull(1,1.0f/90);
    assert(Near(depth.distance,3));
    for (int i = 0; i < 90; ++i) depth.PushPull(-1,1.0f/90);
    assert(Near(depth.distance,1));
    depth.distance = 6; depth.PushPull(1,.05f); assert(Near(depth.distance,6));
    depth.distance = .15f; depth.PushPull(-1,.05f); assert(Near(depth.distance,.15f));
    // Both grabbed title pixels remain attached through translation and scaling.
    XrPosef heldPanel{{0,0,0,1},{0,0,-1.2f}};
    XrExtent2Df heldSize = menuSize;
    XrPosef leftRay{{0,0,0,1},{-.2f,.3f,0}}, rightRay{{0,0,0,1},{.2f,.3f,0}};
    PanelDrag leftGrab, rightGrab;
    const float titleV = .5f-.3f/menuSize.height;
    leftGrab.Begin(heldPanel,heldSize,leftRay,.5f-.2f/menuSize.width,titleV);
    rightGrab.Begin(heldPanel,heldSize,rightRay,.5f+.2f/menuSize.width,titleV);
    TwoHandDrag both;
    both.Begin(heldPanel,heldSize,leftGrab,rightGrab,leftRay,rightRay);
    assert(both.Move(heldPanel,heldSize,leftGrab,rightGrab,leftRay,rightRay,1.0f/90));
    assert(Near(heldPanel.position.x,0) && Near(heldPanel.position.z,-1.2f));
    leftRay.position = Add(leftRay.position,{.1f,.05f,-.1f});
    rightRay.position = Add(rightRay.position,{.1f,.05f,-.1f});
    both.Move(heldPanel,heldSize,leftGrab,rightGrab,leftRay,rightRay,1.0f/90);
    assert(Near(heldPanel.position.x,.1f) && Near(heldPanel.position.y,.05f) && Near(heldPanel.position.z,-1.3f));
    leftRay.position.x -= .1f; rightRay.position.x += .1f;
    both.Move(heldPanel,heldSize,leftGrab,rightGrab,leftRay,rightRay,1.0f/90);
    assert(Near(heldSize.width,menuSize.width*1.5f));
    auto assertPins = [&]()
    {
        const float scale = heldSize.width/menuSize.width;
        const auto a = Add(heldPanel.position,Rotate(heldPanel.orientation,Scale(leftGrab.grab,scale)));
        const auto b = Add(heldPanel.position,Rotate(heldPanel.orientation,Scale(rightGrab.grab,scale)));
        const auto pa = leftGrab.Point(leftRay), pb = rightGrab.Point(rightRay);
        assert(Near(a.x,pa.x) && Near(a.y,pa.y) && Near(a.z,pa.z));
        assert(Near(b.x,pb.x) && Near(b.y,pb.y) && Near(b.z,pb.z));
    };
    assertPins();
    // Wrist rotation pitches the card about the line between the two fingers.
    auto pa = leftGrab.Point(leftRay), pb = rightGrab.Point(rightRay);
    leftRay.orientation = rightRay.orientation = {std::sin(.2f),0,0,std::cos(.2f)};
    leftRay.position = Sub(pa,Rotate(leftRay.orientation,{0,0,-leftGrab.distance}));
    rightRay.position = Sub(pb,Rotate(rightRay.orientation,{0,0,-rightGrab.distance}));
    both.Move(heldPanel,heldSize,leftGrab,rightGrab,leftRay,rightRay,1.0f/90);
    assertPins();
    assert(Near(Rotate(heldPanel.orientation,{0,0,1}).y,-std::sin(.4f)));
    // Releasing either hand can rebase the remaining grab without a pose jump.
    const auto beforeHandoff = heldPanel;
    PanelDrag remaining;
    remaining.Begin(heldPanel,heldSize,leftRay,.5f-.2f/menuSize.width,titleV);
    remaining.Move(heldPanel,leftRay);
    assert(Near(heldPanel.position.x,beforeHandoff.position.x) && Near(heldPanel.position.y,beforeHandoff.position.y) && Near(heldPanel.position.z,beforeHandoff.position.z));
    const auto turned = Rotate(RotationBetween({1,0,0},{-1,0,0}),{1,0,0});
    assert(Near(turned.x,-1) && Near(turned.y,0));
    // IPC layout and ordered events, including wraparound of the sequence.
    RoomInputs roomInput;
    roomInput.W = 4; roomInput.H = 2; roomInput.eye[2] = 3; roomInput.floorY = -2;
    Room room;
    assert(BuildRoom(roomInput,room));
    const XrPosef roomPose{{0,std::sin(.35f),0,std::cos(.35f)},{2,1,-4}};
    for (const auto speed : {XrVector3f{30,0,0},XrVector3f{-30,0,0},XrVector3f{0,0,30},XrVector3f{0,0,-30},XrVector3f{1000,0,0}})
        for (float hz : {72.0f,90.0f,120.0f})
        {
            PanelFlight flight;
            XrPosef sheet{roomPose.orientation,Add(roomPose.position,Rotate(roomPose.orientation,{0,0,3}))};
            flight.flying = true; flight.velocity = Rotate(roomPose.orientation,speed);
            for (int i = 0; i < int(2*hz) && flight.flying; ++i) flight.Step(sheet,menuSize,room,roomPose,1/hz);
            assert(flight.stuck && !flight.flying && !flight.ToggleHidesPanel());
            const auto centre = Rotate(Inverse(roomPose.orientation),Sub(sheet.position,roomPose.position));
            float p[]{centre.x,centre.y,centre.z};
            assert(RoomInside(room,p));
            // Docked orientation faces into the room and remains upright.
            const auto up = Rotate(sheet.orientation,{0,1,0});
            assert(Near(up.y,1));
            const auto frozen = sheet;
            flight.Step(sheet,menuSize,room,roomPose,.05f);
            assert(Near(sheet.position.x,frozen.position.x) && Near(sheet.position.y,frozen.position.y));
            flight.Reset(); assert(flight.ToggleHidesPanel()); // grabbing removes the visibility exception
        }
    // Downward throws float clear of the floor; free throws coast and stop in place.
    PanelFlight fall; fall.flying = true; fall.velocity = {0,-10,0};
    XrPosef sheet{{0,0,0,1},{0,0,3}};
    for (int i = 0; i < 90 && fall.flying; ++i) fall.Step(sheet,menuSize,room,{{0,0,0,1},{0,0,0}},1.0f/90);
    assert(!fall.flying && !fall.stuck && fall.ToggleHidesPanel());
    assert(sheet.position.y - menuSize.height*.5f >= room.yF+.149f);
    PanelDrag place; place.grab = {0,.3f,0};
    PanelFlight release; release.Release(sheet,place); assert(!release.flying);
    place.pivotVelocity = {2,0,0}; release.Release(sheet,place); assert(release.flying);
    const auto releasedAt = sheet;
    for (int i = 0; i < 900; ++i) release.Step(sheet,menuSize,Room{},roomPose,1.0f/90);
    assert(!release.flying && !release.stuck && sheet.position.x > releasedAt.position.x+2.1f && sheet.position.x < releasedAt.position.x+2.23f);
    // No vertical acceleration, and the exponential coast is frame-rate independent.
    for (float hz : {72.0f,90.0f,120.0f})
    {
        PanelFlight lunar; lunar.flying = true; lunar.velocity = {1,0,0};
        XrPosef floating{{0,0,0,1},{0,0,0}};
        for (int i = 0; i < int(hz*.5f); ++i) lunar.Step(floating,menuSize,Room{},roomPose,1/hz);
        assert(Near(floating.position.y,0));
        assert(std::fabs(floating.position.x - (1-std::exp(-.45f))/.9f) < .0001f);
    }
    assert(EdgeHit(.0125f,.5f) && EdgeHit(.99f,.5f) && EdgeHit(.5f,.015f));
    assert(EdgeHit(.04f,.5f) && EdgeHit(.5f,.05f)); // wider grab area
    assert(!EdgeHit(.5f,.5f) && !EdgeHit(-.01f,.5f));
    XrPosef resizePose{{0,0,0,1},{0,0,-1.2f}};
    XrExtent2Df resizeSize = menuSize;
    XrPosef resizeRay{{0,0,0,1},{menuSize.width*.49f,0,0}};
    PanelResize edge; edge.Begin(resizePose,resizeSize,resizeRay,.99f,.5f);
    const float fixedLeft = resizePose.position.x - resizeSize.width*.5f;
    resizeRay.position.x += .2f; assert(edge.Move(resizePose,resizeSize,resizeRay));
    assert(resizeSize.width > menuSize.width && Near(resizePose.position.x-resizeSize.width*.5f,fixedLeft));
    resizeRay.position.x += 10; assert(edge.Move(resizePose,resizeSize,resizeRay));
    assert(Near(resizeSize.width,3) && Near(resizeSize.height,3*menuSize.height/menuSize.width));
    assert(Near(resizePose.position.x-resizeSize.width*.5f,fixedLeft));
    XrPosef largePose{{0,0,0,1},{0,0,-1.2f}}, largeLeft{{0,0,0,1},{-.2f,0,0}}, largeRight{{0,0,0,1},{.2f,0,0}};
    XrExtent2Df largeSize = menuSize;
    PanelDrag largeFirst, largeSecond;
    largeFirst.Begin(largePose,largeSize,largeLeft,.5f-.2f/menuSize.width,.5f);
    largeSecond.Begin(largePose,largeSize,largeRight,.5f+.2f/menuSize.width,.5f);
    TwoHandDrag largeBoth; largeBoth.Begin(largePose,largeSize,largeFirst,largeSecond,largeLeft,largeRight);
    largeLeft.position.x = -10; largeRight.position.x = 10;
    assert(largeBoth.Move(largePose,largeSize,largeFirst,largeSecond,largeLeft,largeRight,1.0f/90));
    assert(Near(largeSize.width,3) && Near(largeSize.height,3*menuSize.height/menuSize.width));
    assert(BuildCylinder(4,2,3,.8f,roomInput.cyl) && BuildRoom(roomInput,room));
    PanelFlight curveFlight; curveFlight.flying = true; curveFlight.velocity = {0,0,-30};
    sheet = {{0,0,0,1},{.5f,0,3}};
    for (int i = 0; i < 90 && curveFlight.flying; ++i) curveFlight.Step(sheet,menuSize,room,{{0,0,0,1},{0,0,0}},1.0f/90);
    assert(curveFlight.stuck);
    XrExtent2Df fittedSize{1.6f,1.28f};
    curveFlight.FitToWall(sheet,fittedSize,room,{{0,0,0,1},{0,0,0}});
    for (float x : {-.5f,.5f})
    {
        const auto corner = Add(sheet.position,Rotate(sheet.orientation,{x*fittedSize.width,0,0}));
        assert(corner.z > FrontDepth(room,corner.x));
    }
    // Dragging, resizing, hovering and even old poses already outside the room
    // all use the same full-rectangle constraint, in the room's actual frame.
    auto checkCorners = [&](XrPosef panelPose, XrExtent2Df panelSize, const Room& boundsRoom)
    {
        for (float x : {-.5f,.5f}) for (float y : {-.5f,.5f})
        {
            const auto world = Add(panelPose.position,Rotate(panelPose.orientation,{x*panelSize.width,y*panelSize.height,0}));
            const auto local = Rotate(Inverse(roomPose.orientation),Sub(world,roomPose.position));
            float corner[]{local.x,local.y,local.z}; assert(RoomInside(boundsRoom,corner));
        }
    };
    for (bool curved : {false,true})
    {
        RoomInputs boundedInput = roomInput;
        if (!curved) boundedInput.cyl = Cylinder{};
        Room boundedRoom; assert(BuildRoom(boundedInput,boundedRoom));
        for (auto centre : {XrVector3f{-20,-20,-20},XrVector3f{20,20,20},XrVector3f{0,0,3},XrVector3f{3,1,-.1f}})
            for (float yaw : {-3.0f,-1.5f,0.0f,.9f}) for (float pitch : {-.7f,0.0f,.8f}) for (float roll : {-.6f,.4f})
            {
                auto rotation = Multiply({0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)},
                    Multiply({std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)},{0,0,std::sin(roll*.5f),std::cos(roll*.5f)}));
                XrPosef panelPose{Multiply(roomPose.orientation,rotation),Add(roomPose.position,Rotate(roomPose.orientation,centre))};
                XrExtent2Df panelSize{3,2.4f};
                ContainPanel(panelPose,panelSize,boundedRoom,roomPose);
                checkCorners(panelPose,panelSize,boundedRoom);
            }
        PanelFlight wall;
        XrPosef onWall{roomPose.orientation,Add(roomPose.position,Rotate(roomPose.orientation,{.5f,0,-1}))};
        XrExtent2Df wallSize = menuSize;
        wall.Dock(onWall,wallSize,boundedRoom,roomPose,kFaceFront);
        onWall.position = Add(onWall.position,Rotate(roomPose.orientation,{.5f,.2f,.06f}));
        const auto nearby = ContainPanel(onWall,wallSize,boundedRoom,roomPose);
        assert(nearby.wallDistance <= .14f);
        wall.Dock(onWall,wallSize,boundedRoom,roomPose,nearby.wall);
        assert(wall.stuck && !wall.flying); checkCorners(onWall,wallSize,boundedRoom);
        const auto eye = Add(roomPose.position,Rotate(roomPose.orientation,{-.5f,.5f,3}));
        for (int i = 0; i < 90; ++i)
        {
            FaceViewer(onWall,eye,1.0f/90);
            ContainPanel(onWall,wallSize,boundedRoom,roomPose);
            checkCorners(onWall,wallSize,boundedRoom);
        }
        assert(Dot(Rotate(onWall.orientation,{0,0,1}),Normalize(Sub(eye,onWall.position))) > .99f);
    }
    XrPosef belowViewer{{0,0,0,1},{0,0,0}};
    for (int i = 0; i < 90; ++i) FaceViewer(belowViewer,{0,2,0},1.0f/90);
    assert(Rotate(belowViewer.orientation,{0,0,1}).y > .99f);
    // Arrange all seven surfaces within the gaze frustum, close to the user,
    // facing them, with no projected overlap in the normal room layout.
    const XrFovf arrangeFov{-.8f,.8f,.7f,-.7f};
    XrPosef arrangeEye{roomPose.orientation,Add(roomPose.position,Rotate(roomPose.orientation,{0,0,3}))};
    RoomInputs arrangeInputs; arrangeInputs.W = 4; arrangeInputs.H = 2; arrangeInputs.eye[2] = 3; arrangeInputs.floorY = -2;
    Room arrangeRoom; assert(BuildRoom(arrangeInputs,arrangeRoom));
    for (int count = 1; count <= 7; ++count) for (uint32_t seed : {1u,99u,123456u})
    {
        XrPosef poses[7]{}; XrExtent2Df sizes[7]{}; float boxes[7][4]{};
        for (int i = 0; i < count; ++i) sizes[i] = menuSize;
        ArrangePanels(arrangeEye,arrangeFov,poses,sizes,count,arrangeRoom,roomPose,seed);
        for (int i = 0; i < count; ++i)
        {
            const auto delta = Sub(poses[i].position,arrangeEye.position);
            assert(std::sqrt(Dot(delta,delta)) < 1.9f);
            assert(Dot(Rotate(poses[i].orientation,{0,0,1}),Normalize(Scale(delta,-1))) > .999f);
            assert(Near(sizes[i].width/sizes[i].height,menuSize.width/menuSize.height));
            boxes[i][0] = boxes[i][2] = 1e9f; boxes[i][1] = boxes[i][3] = -1e9f;
            for (float x : {-.5f,.5f}) for (float y : {-.5f,.5f})
            {
                const auto world = Add(poses[i].position,Rotate(poses[i].orientation,{x*sizes[i].width,y*sizes[i].height,0}));
                const auto local = Rotate(Inverse(arrangeEye.orientation),Sub(world,arrangeEye.position));
                assert(local.z < 0 && std::fabs(std::atan2(local.x,-local.z)) < .8f && std::fabs(std::atan2(local.y,-local.z)) < .7f);
                float px = local.x/-local.z, py = local.y/-local.z;
                boxes[i][0] = std::min(boxes[i][0],px); boxes[i][1] = std::max(boxes[i][1],px);
                boxes[i][2] = std::min(boxes[i][2],py); boxes[i][3] = std::max(boxes[i][3],py);
            }
            checkCorners(poses[i],sizes[i],arrangeRoom);
        }
        for (int i = 0; i < count; ++i) for (int j = i+1; j < count; ++j)
            assert(boxes[i][1] <= boxes[j][0] || boxes[j][1] <= boxes[i][0] || boxes[i][3] <= boxes[j][2] || boxes[j][3] <= boxes[i][2]);
    }
    // Mouse mode needs a fresh press and always balances a captured down.
    MouseLatch mouse;
    assert(mouse.Step(true,true) == 0); // enabling while trigger held
    assert(mouse.Step(true,false) == 0);
    assert(mouse.Step(true,true) == 1 && mouse.held);
    assert(mouse.Step(true,true) == 0); // held drag
    assert(mouse.Step(false,true) == -1 && !mouse.held); // loss of hit/focus/mode
    assert(mouse.Step(true,true) == 0); // returning while held does not click
    assert(mouse.Step(true,false) == 0);
    assert(mouse.Step(true,true) == 1);
    assert(mouse.Step(true,false) == -1);
    // Flat and curved screen rays map to the displayed picture, including edges.
    XrPosef pointer;
    assert(ScreenHit(panel,{2,1},Cylinder{},{{0,0,0,1},{0,0,0}},u,v,pointer));
    assert(Near(u,.5f) && Near(v,.5f));
    assert(!ScreenHit(panel,{2,1},Cylinder{},{{0,0,0,1},{2,0,0}},u,v,pointer));
    Cylinder curve; assert(BuildCylinder(2,1,2,.5f,curve));
    for (float targetU : {.05f,.5f,.95f})
    {
        const float phi = (targetU-.5f)*2*curve.halfWrap;
        XrPosef screenRay{{0,0,0,1},{curve.radius*std::sin(phi),0,0}};
        assert(ScreenHit(panel,{2,1},curve,screenRay,u,v,pointer));
        assert(Near(u,targetU) && Near(v,.5f));
    }
    auto shared = std::make_unique<Shared>();
    shared->header.write = shared->header.read = UINT32_MAX-1;
    Push(*shared,{Down,.25f,.5f,0}); Push(*shared,{Up,.3f,.5f,0});
    assert(uint32_t(shared->header.write-shared->header.read) == 2);
    assert(shared->events[(UINT32_MAX-1)%EventCount].kind == Down);
    assert(shared->events[UINT32_MAX%EventCount].kind == Up);
    for (int i = 0; i < EventCount+1; ++i) Push(*shared,{Move,0,0,0});
    assert(uint32_t(shared->header.write-shared->header.read) > EventCount); // consumer must cancel drag
    puts("PASS: full-panel room constraints, hover facing, wall resticking, thick resize borders, inertial coast, two-hand manipulation, mouse input, screen picking and snapshot IPC");
}
