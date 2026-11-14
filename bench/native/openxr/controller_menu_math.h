#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>

namespace vrxmenu
{
// A held button must not retrigger or become a new press after focus/enable changes.
struct PressLatch
{
    bool armed = false;
    void Reset() { armed = false; }
    bool Press(bool down)
    {
        if (!down) { armed = true; return false; }
        const bool pressed = armed; armed = false; return pressed;
    }
};
inline XrVector3f Add(XrVector3f a, XrVector3f b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline XrVector3f Sub(XrVector3f a, XrVector3f b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline XrVector3f Scale(XrVector3f a, float s) { return {a.x*s,a.y*s,a.z*s}; }
inline XrQuaternionf Multiply(XrQuaternionf a, XrQuaternionf b)
{
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y, a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w, a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline XrVector3f Rotate(XrQuaternionf q, XrVector3f v)
{
    XrVector3f t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,v.y+q.w*t.y+q.z*t.x-q.x*t.z,v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
inline XrQuaternionf Inverse(XrQuaternionf q) { return {-q.x,-q.y,-q.z,q.w}; }

inline float Dot(XrVector3f a, XrVector3f b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline XrVector3f Cross(XrVector3f a, XrVector3f b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline XrVector3f Normalize(XrVector3f v) { const float n = std::sqrt(Dot(v,v)); return n > 1e-6f ? Scale(v,1/n) : XrVector3f{0,0,0}; }

// Open in the current gaze direction, then remain fixed until grabbed or reopened.
inline XrPosef Place(XrPosef eye)
{
    return {eye.orientation, Add(eye.position, Rotate(eye.orientation,{0,0,-1.2f}))};
}

struct PanelDrag
{
    float distance = 0;
    float grabU = 0, grabV = 0, inertia = 1;
    XrVector3f grab{}, previousPivot{}, pivotVelocity{}, angularVelocity{};
    XrQuaternionf handOffset{0,0,0,1};
    XrVector3f Point(XrPosef ray) const
    { return Add(ray.position,Rotate(ray.orientation,{0,0,-distance})); }
    void Begin(XrPosef panel, XrExtent2Df size, XrPosef ray, float u, float v)
    {
        grabU = u; grabV = v;
        if (Rotate(Inverse(panel.orientation),Sub(ray.position,panel.position)).z < 0) u = 1-u;
        grab = {(u-.5f)*size.width,(.5f-v)*size.height,0};
        const auto point = Add(panel.position,Rotate(panel.orientation,grab));
        const auto delta = Sub(point,ray.position);
        distance = std::sqrt(Dot(delta,delta));
        previousPivot = point; pivotVelocity = angularVelocity = {0,0,0};
        handOffset = Multiply(Inverse(ray.orientation),panel.orientation);
        // Unit-mass plate about the pinched point, with isotropic rotational
        // inertia for a stable, lightweight hanging-sheet interaction.
        inertia = Dot(grab,grab) + (size.width*size.width+size.height*size.height)/12;
    }
    void Move(XrPosef& panel, XrPosef ray, float dt = 0)
    {
        const auto pivot = Point(ray);
        dt = std::clamp(dt,0.0f,.05f);
        if (dt > 0)
        {
            const auto velocity = Scale(Sub(pivot,previousPivot),1/dt);
            auto acceleration = Scale(Sub(velocity,pivotVelocity),1/dt);
            const float a = std::sqrt(Dot(acceleration,acceleration));
            if (a > 40) acceleration = Scale(acceleration,40/a);
            pivotVelocity = velocity;
            const auto gravity = Sub({0,-9.81f,0},acceleration);
            const auto target = Multiply(ray.orientation,handOffset);
            // Small fixed substeps keep the swing stable at different headset rates.
            const int steps = int(std::ceil(dt*240));
            const float step = dt/steps;
            for (int i = 0; i < steps; ++i)
            {
                const auto radius = Rotate(panel.orientation,Scale(grab,-1));
                auto torque = Scale(Cross(radius,gravity),1/inertia);
                auto twist = Multiply(target,Inverse(panel.orientation));
                if (twist.w < 0) twist = {-twist.x,-twist.y,-twist.z,-twist.w};
                // Fingers can turn the sheet around the vertical axis, while
                // gravity controls pitch and roll instead of locking the old tilt.
                torque.y += 20*twist.y;
                angularVelocity = Scale(Add(angularVelocity,Scale(torque,step)),std::exp(-2.5f*step));
                float speed = std::sqrt(Dot(angularVelocity,angularVelocity));
                if (speed > 8) { angularVelocity = Scale(angularVelocity,8/speed); speed = 8; }
                if (speed > 1e-6f)
                {
                    const auto axis = Scale(angularVelocity,std::sin(speed*step*.5f)/speed);
                    auto q = Multiply({axis.x,axis.y,axis.z,std::cos(speed*step*.5f)},panel.orientation);
                    const float n = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
                    panel.orientation = {q.x/n,q.y/n,q.z/n,q.w/n};
                }
            }
        }
        previousPivot = pivot;
        // Exact pivot constraint: the pinched pixel never slides away from the ray.
        panel.position = Sub(pivot,Rotate(panel.orientation,grab));
    }
};

// Rotation whose local X/Y axes are the billboard's right/up vectors.
inline XrQuaternionf Axes(XrVector3f x, XrVector3f y, XrVector3f z)
{
    XrQuaternionf q{};
    const float trace = x.x+y.y+z.z;
    if (trace > 0) { const float s = 2*std::sqrt(trace+1); q = {(y.z-z.y)/s,(z.x-x.z)/s,(x.y-y.x)/s,s/4}; }
    else if (x.x > y.y && x.x > z.z) { const float s = 2*std::sqrt(1+x.x-y.y-z.z); q = {s/4,(y.x+x.y)/s,(z.x+x.z)/s,(y.z-z.y)/s}; }
    else if (y.y > z.z) { const float s = 2*std::sqrt(1+y.y-x.x-z.z); q = {(y.x+x.y)/s,s/4,(z.y+y.z)/s,(z.x-x.z)/s}; }
    else { const float s = 2*std::sqrt(1+z.z-x.x-y.y); q = {(z.x+x.z)/s,(z.y+y.z)/s,s/4,(x.y-y.x)/s}; }
    return q;
}

// A thin white compositor quad along the ray, billboarded towards the viewer.
inline XrPosef Beam(XrPosef ray, float distance, XrVector3f eye)
{
    const auto up = Rotate(ray.orientation,{0,0,-1});
    const auto middle = Add(ray.position,Scale(up,distance*.5f));
    auto right = Normalize(Cross(up,Sub(eye,middle)));
    if (Dot(right,right) < .5f) right = Rotate(ray.orientation,{1,0,0});
    return {Axes(right,up,Normalize(Cross(right,up))),middle};
}

// UV origin is the bitmap's upper-left. Reject behind/parallel rays; a drag can
// request unbounded UVs so the slider still clamps correctly beyond its ends.
inline bool Hit(XrPosef panel, XrExtent2Df size, XrPosef aim, float& u, float& v, bool bounded = true, bool twoSided = false)
{
    auto inverse = Inverse(panel.orientation);
    auto origin = Rotate(inverse, Sub(aim.position,panel.position));
    auto direction = Rotate(inverse, Rotate(aim.orientation,{0,0,-1}));
    if (!std::isfinite(direction.z) || std::fabs(direction.z) < 1e-5f || (!twoSided && direction.z >= 0) || size.width <= 0 || size.height <= 0) return false;
    const float t = -origin.z / direction.z;
    if (!std::isfinite(t) || t <= 0) return false;
    u = .5f + (origin.x+t*direction.x)/size.width;
    if (twoSided && origin.z < 0) u = 1-u;
    v = .5f - (origin.y+t*direction.y)/size.height;
    return std::isfinite(u) && std::isfinite(v) && (!bounded || (u>=0 && u<=1 && v>=0 && v<=1));
}
}
