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

struct PanelBounds
{
    int wall = -1;
    float wallDistance = 1e9f;
    bool moved = false;
};

// The room is convex. Constrain all four corners, rather than just the centre
// or rays starting inside: this also repairs a panel already outside a wall.
inline PanelBounds ContainPanel(XrPosef& pose, XrExtent2Df& size, const Room& room, XrPosef roomPose)
{
    PanelBounds result;
    if (!room.valid || room.env == kRoomEnvOpenAir) return result;
    const auto inverse = Inverse(roomPose.orientation);
    auto p = Rotate(inverse,Sub(pose.position,roomPose.position));
    const auto before = p;
    const auto q = Multiply(inverse,pose.orientation);
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        const auto x = Rotate(q,{size.width*.5f,0,0}), y = Rotate(q,{0,size.height*.5f,0});
        const float ex = std::fabs(x.x)+std::fabs(y.x), ey = std::fabs(x.y)+std::fabs(y.y), ez = std::fabs(x.z)+std::fabs(y.z);
        const float scale = std::min({1.0f,(2*room.X-.04f)/std::max(.0001f,2*ex),
            (room.yC-room.yF-.3f)/std::max(.0001f,2*ey)});
        if (scale < .9999f)
        { size.width *= std::max(.05f,scale*.999f); size.height *= std::max(.05f,scale*.999f); continue; }
        const float left = -room.X+ex+.02f, right = room.X-ex-.02f;
        p.x = std::clamp(p.x,left,right);
        p.y = std::clamp(p.y,room.yF+ey+.15f,room.yC-ey-.15f);
        float front = -1e9f;
        for (float sx : {-1.0f,1.0f}) for (float sy : {-1.0f,1.0f})
        {
            const auto offset = Add(Scale(x,sx),Scale(y,sy));
            front = std::max(front,FrontDepth(room,p.x+offset.x)-offset.z+.02f);
        }
        const float back = room.zB-ez-.02f;
        if (front > back)
        { size.width *= .9f; size.height *= .9f; continue; }
        p.z = std::clamp(p.z,front,back);
        const float distances[]{p.x-left,right-p.x,p.z-front,back-p.z};
        const int faces[]{kFaceLeft,kFaceRight,kFaceFront,kFaceBack};
        for (int k = 0; k < 4; ++k) if (distances[k] < result.wallDistance)
        { result.wallDistance = distances[k]; result.wall = faces[k]; }
        break;
    }
    result.moved = Dot(Sub(p,before),Sub(p,before)) > 1e-10f;
    pose.position = Add(roomPose.position,Rotate(roomPose.orientation,p));
    return result;
}

inline void FaceViewer(XrPosef& pose, XrVector3f eye, float dt)
{
    const auto normal = Normalize(Sub(eye,pose.position));
    if (Dot(normal,normal) < .5f) return;
    auto right = Normalize(Cross({0,1,0},normal));
    if (Dot(right,right) < .5f)
    {
        right = Rotate(pose.orientation,{1,0,0});
        right = Normalize(Sub(right,Scale(normal,Dot(right,normal))));
        if (Dot(right,right) < .5f) right = Normalize(Cross({0,0,1},normal));
    }
    auto target = Axes(right,Cross(normal,right),normal);
    auto& q = pose.orientation;
    if (q.x*target.x+q.y*target.y+q.z*target.z+q.w*target.w < 0) target = {-target.x,-target.y,-target.z,-target.w};
    const float a = 1-std::exp(-12*std::clamp(dt,0.0f,.05f));
    q = {q.x+a*(target.x-q.x),q.y+a*(target.y-q.y),q.z+a*(target.z-q.z),q.w+a*(target.w-q.w)};
    const float n = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    q = {q.x/n,q.y/n,q.z/n,q.w/n};
}

inline void ArrangePanels(XrPosef eye, XrFovf fov, XrPosef* poses, XrExtent2Df* sizes, int count,
    const Room& room, XrPosef roomPose, uint32_t seed)
{
    if (count <= 0 || count > 7) return;
    auto random = [&]() { seed = seed*1664525u+1013904223u; return float(seed>>8)/16777215.0f; };
    const int columns = int(std::ceil(std::sqrt(float(count)))), rows = (count+columns-1)/columns;
    int cells[9]{}; for (int i = 0; i < columns*rows; ++i) cells[i] = i;
    for (int i = columns*rows-1; i > 0; --i) std::swap(cells[i],cells[int(random()*(i+1))%(i+1)]);
    const float horizontal = std::clamp(std::min(-fov.angleLeft,fov.angleRight)*.8f,.3f,.62f);
    const float vertical = std::clamp(std::min(-fov.angleDown,fov.angleUp)*.8f,.25f,.48f);
    for (int i = 0; i < count; ++i)
    {
        const int column = cells[i]%columns, row = cells[i]/columns;
        const float nx = ((column+.5f)/columns-.5f)*2*std::tan(horizontal) + (random()-.5f)*std::tan(horizontal)*.07f/columns;
        const float ny = (.5f-(row+.5f)/rows)*2*std::tan(vertical) + (random()-.5f)*std::tan(vertical)*.07f/rows;
        float depth = 1.35f+(random()-.5f)*.1f;
        const auto ray = Normalize(Rotate(eye.orientation,{nx,ny,-1}));
        if (room.valid && room.env != kRoomEnvOpenAir)
        {
            const auto inverse = Inverse(roomPose.orientation);
            const auto origin = Rotate(inverse,Sub(eye.position,roomPose.position)), direction = Rotate(inverse,ray);
            float o[]{origin.x,origin.y,origin.z}, d[]{direction.x,direction.y,direction.z}; RoomHit hit;
            if (RoomExit(room,o,d,hit)) depth = std::min(depth,hit.t*.65f/std::sqrt(nx*nx+ny*ny+1));
        }
        poses[i].position = Add(eye.position,Rotate(eye.orientation,{nx*depth,ny*depth,-depth}));
        const auto normal = Normalize(Sub(eye.position,poses[i].position));
        const auto up = Rotate(eye.orientation,{0,1,0});
        const auto right = Normalize(Cross(up,normal));
        poses[i].orientation = Axes(right,Cross(normal,right),normal);
        const float aspect = sizes[i].height > 0 ? sizes[i].width/sizes[i].height : 1.25f;
        const float width = std::min({.85f,2*depth*std::tan(horizontal)/columns*.8f,2*depth*std::tan(vertical)/rows*aspect*.8f});
        sizes[i] = {width,width/aspect};
        ContainPanel(poses[i],sizes[i],room,roomPose);
    }
}

struct PanelFlight
{
    bool flying = false, stuck = false;
    int dockFace = -1;
    float age = 0;
    XrVector3f velocity{}, angularVelocity{};
    XrPosef releasePose{};
    void Reset() { flying = stuck = false; dockFace = -1; velocity = angularVelocity = {0,0,0}; age = 0; }
    void Release(XrPosef pose, const PanelDrag& drag)
    {
        Reset(); releasePose = pose;
        const auto radius = Rotate(pose.orientation,Scale(drag.grab,-1));
        velocity = Add(drag.pivotVelocity,Cross(drag.angularVelocity,radius));
        flying = Dot(velocity,velocity) >= .8f*.8f;
        angularVelocity = drag.angularVelocity;
    }
    bool ToggleHidesPanel() const { return !stuck; }
    void Dock(XrPosef& pose, XrExtent2Df& size, const Room& room, XrPosef roomPose, int face)
    {
        if (!room.valid || room.env == kRoomEnvOpenAir || face < 0) return;
        auto p = Rotate(Inverse(roomPose.orientation),Sub(pose.position,roomPose.position));
        XrVector3f normal{};
        switch (face)
        {
        case kFaceLeft: p.x = -room.X; normal = {1,0,0}; break;
        case kFaceRight: p.x = room.X; normal = {-1,0,0}; break;
        case kFaceBack: p.z = room.zB; normal = {0,0,-1}; break;
        case kFaceFront:
            p.z = FrontDepth(room,p.x);
            normal = Normalize({-(FrontDepth(room,p.x+.001f)-FrontDepth(room,p.x-.001f))/.002f,0,1}); break;
        default: return;
        }
        const auto right = Normalize(Cross({0,1,0},normal));
        pose = {Multiply(roomPose.orientation,Axes(right,Cross(normal,right),normal)),Add(roomPose.position,Rotate(roomPose.orientation,p))};
        ContainPanel(pose,size,room,roomPose);
        flying = false; stuck = true; dockFace = face;
        velocity = angularVelocity = {0,0,0};
    }
    void FitToWall(XrPosef& pose, XrExtent2Df& size, const Room& room, XrPosef roomPose)
    {
        if (stuck) Dock(pose,size,room,roomPose,dockFace);
    }
    void Step(XrPosef& pose, XrExtent2Df& size, const Room& room, XrPosef roomPose, float dt)
    {
        if (!flying || dt <= 0) return;
        dt = std::min(dt,.05f); age += dt;
        const int steps = int(std::ceil(dt*240));
        const float step = dt/steps;
        for (int i = 0; i < steps && flying; ++i)
        {
            const float decay = std::exp(-.9f*step);
            pose.position = Add(pose.position,Scale(velocity,(1-decay)/.9f));
            velocity = Scale(velocity,decay);
            pose.orientation = Spin(pose.orientation,angularVelocity,step);
            angularVelocity = Scale(angularVelocity,std::exp(-1.2f*step));
            const auto before = pose.position;
            const auto bounds = ContainPanel(pose,size,room,roomPose);
            if (bounds.wallDistance <= .001f) Dock(pose,size,room,roomPose,bounds.wall);
            else if (std::fabs(pose.position.y-before.y) > 1e-5f) velocity.y = 0;
        }
        if (flying && Dot(velocity,velocity) < .03f*.03f && Dot(angularVelocity,angularVelocity) < .03f*.03f)
        { flying = false; velocity = angularVelocity = {0,0,0}; }
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
