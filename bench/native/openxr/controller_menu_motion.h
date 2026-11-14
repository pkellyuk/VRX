#pragma once
#include "controller_menu_math.h"
#include "room.h"

namespace vrxmenu
{
inline XrQuaternionf Spin(XrQuaternionf q, XrVector3f velocity, float dt)
{
    const float speed = std::sqrt(Dot(velocity,velocity));
    if (speed < 1e-6f) return q;
    const auto axis = Scale(velocity,std::sin(speed*dt*.5f)/speed);
    q = Multiply({axis.x,axis.y,axis.z,std::cos(speed*dt*.5f)},q);
    const float n = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    return {q.x/n,q.y/n,q.z/n,q.w/n};
}

struct PanelFlight
{
    bool flying = false, stuck = false;
    float age = 0;
    XrVector3f velocity{}, angularVelocity{};
    XrPosef releasePose{};
    void Reset() { flying = stuck = false; velocity = angularVelocity = {0,0,0}; age = 0; }
    void Release(XrPosef pose, const PanelDrag& drag)
    {
        Reset(); releasePose = pose;
        const auto radius = Rotate(pose.orientation,Scale(drag.grab,-1));
        velocity = Add(drag.pivotVelocity,Cross(drag.angularVelocity,radius));
        // Ordinary placement still leaves the panel where it was released.
        flying = Dot(velocity,velocity) >= .8f*.8f;
        angularVelocity = drag.angularVelocity;
    }
    bool ToggleHidesPanel() const { return !stuck; }
    void Step(XrPosef& pose, XrExtent2Df size, const Room& room, XrPosef roomPose, float dt)
    {
        if (!flying || dt <= 0) return;
        dt = std::min(dt,.05f); age += dt;
        const int steps = int(std::ceil(dt*240));
        const float step = dt/steps;
        const auto inverse = Inverse(roomPose.orientation);
        for (int i = 0; i < steps && flying; ++i)
        {
            const auto old = pose;
            pose.position = Add(pose.position,Add(Scale(velocity,step),{0,-.5f*MenuGravity*step*step,0}));
            velocity.y -= MenuGravity*step;
            pose.orientation = Spin(pose.orientation,angularVelocity,step);
            angularVelocity = Scale(angularVelocity,std::exp(-.5f*step));
            float first = 2; RoomHit collision;
            // Sweep the centre and corners; fast throws cannot tunnel through walls.
            if (room.valid && room.env != kRoomEnvOpenAir)
                for (int k = 0; k < 5; ++k)
                {
                    const XrVector3f offset = k == 0 ? XrVector3f{0,0,0} :
                        XrVector3f{((k-1)&1 ? .5f : -.5f)*size.width,((k-1)&2 ? .5f : -.5f)*size.height,0};
                    const auto a = Rotate(inverse,Sub(Add(old.position,Rotate(old.orientation,offset)),roomPose.position));
                    const auto b = Rotate(inverse,Sub(Add(pose.position,Rotate(pose.orientation,offset)),roomPose.position));
                    const auto delta = Sub(b,a);
                    float origin[]{a.x,a.y,a.z}, direction[]{delta.x,delta.y,delta.z};
                    RoomHit hit;
                    if (RoomExit(room,origin,direction,hit) && hit.t <= 1 && hit.t < first)
                    { first = hit.t; collision = hit; }
                }
            if (first <= 1)
            {
                float point[3], normal[3];
                if (!RoomFacePoint(room,collision.face,collision.u,collision.v,point,normal)) continue;
                const bool wall = collision.face != kFaceFloor && collision.face != kFaceCeiling;
                XrVector3f p{point[0],point[1],point[2]}, n{normal[0],normal[1],normal[2]};
                if (wall)
                {
                    p.y = std::clamp(p.y,room.yF+size.height*.5f+.02f,room.yC-size.height*.5f-.02f);
                    if (collision.face == kFaceLeft || collision.face == kFaceRight)
                        p.z = std::clamp(p.z,room.zSide+size.width*.5f+.02f,room.zB-size.width*.5f-.02f);
                    else
                    {
                        p.x = std::clamp(p.x,-room.X+size.width*.5f+.02f,room.X-size.width*.5f-.02f);
                        if (collision.face == kFaceFront)
                        {
                            p.z = FrontDepth(room,p.x);
                            const float slope = (FrontDepth(room,p.x+.001f)-FrontDepth(room,p.x-.001f))/.002f;
                            n = Normalize({-slope,0,1});
                        }
                    }
                }
                const auto up = wall ? XrVector3f{0,1,0} : XrVector3f{0,0,1};
                const auto right = Normalize(Cross(up,n));
                const auto orientation = Axes(right,Cross(n,right),n);
                p = Add(p,Scale(n,.015f));
                // A flat card on a curved wall needs a little extra clearance at its corners.
                if (wall && collision.face == kFaceFront)
                    for (int pass = 0; pass < 4; ++pass)
                    {
                        float clearance = 0;
                        for (float x : {-.5f,.5f})
                        {
                            const auto corner = Add(p,Rotate(orientation,{x*size.width,0,0}));
                            clearance = std::max(clearance,FrontDepth(room,corner.x)+.01f-corner.z);
                        }
                        if (clearance <= 0) break;
                        p = Add(p,Scale(n,clearance/std::max(.1f,n.z)));
                    }
                pose = {Multiply(roomPose.orientation,orientation),Add(roomPose.position,Rotate(roomPose.orientation,p))};
                flying = false; stuck = wall;
                velocity = angularVelocity = {0,0,0};
            }
        }
        // A miss in open air returns the panel rather than losing it below the floor.
        if (flying && age > 8) { pose = releasePose; Reset(); }
    }
};

inline bool ScreenHit(XrPosef screen, XrExtent2Df size, const Cylinder& cylinder, XrPosef ray,
    float& u, float& v, XrPosef& pointer)
{
    if (!cylinder.curved)
    {
        if (!Hit(screen,size,ray,u,v)) return false;
        pointer = {screen.orientation,Add(screen.position,Rotate(screen.orientation,{(u-.5f)*size.width,(.5f-v)*size.height,.005f}))};
        return true;
    }
    const auto inverse = Inverse(screen.orientation);
    const auto origin = Rotate(inverse,Sub(ray.position,screen.position));
    const auto direction = Rotate(inverse,Rotate(ray.orientation,{0,0,-1}));
    float o[]{origin.x,origin.y,origin.z}, d[]{direction.x,direction.y,direction.z};
    if (!CylinderHit(cylinder,o,d,&u,&v)) return false;
    const float phi = (u-.5f)*2*cylinder.halfWrap;
    pointer = {Multiply(screen.orientation,{0,-std::sin(phi*.5f),0,std::cos(phi*.5f)}),
        Add(screen.position,Rotate(screen.orientation,{(cylinder.radius-.005f)*std::sin(phi),(v-.5f)*-size.height,
            cylinder.radius-(cylinder.radius-.005f)*std::cos(phi)}))};
    return true;
}

struct MouseLatch
{
    bool armed = false, held = false;
    // -1 release, +1 press, 0 unchanged. Re-entry while held never creates a click.
    int Step(bool allowed, bool down)
    {
        if (!allowed) { const bool release = held; held = armed = false; return release ? -1 : 0; }
        if (!down) { armed = true; const bool release = held; held = false; return release ? -1 : 0; }
        if (armed && !held) { held = true; armed = false; return 1; }
        return 0;
    }
};
}
