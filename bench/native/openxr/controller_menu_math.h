#pragma once
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>

namespace vrxmenu
{
constexpr float MenuGravity = 1.62f; // Moon-like gravity, metres per second squared.
constexpr float MenuAngularDamping = 1.0f;
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

constexpr float ResizeBorderU = 40.0f/960, ResizeBorderV = 40.0f/768;
constexpr float MaxPanelWidth = 3.0f; // Metres; shared by edge and two-handed resizing.
inline bool EdgeHit(float u, float v)
{
    return u >= 0 && u <= 1 && v >= 0 && v <= 1 &&
        (u < ResizeBorderU || u > 1-ResizeBorderU || v < ResizeBorderV || v > 1-ResizeBorderV);
}

struct PanelResize
{
    XrPosef plane{};
    XrExtent2Df original{};
    XrVector3f anchor{}, anchorLocal{}, initialDelta{};
    void Begin(XrPosef pose, XrExtent2Df size, XrPosef ray, float u, float v)
    {
        plane = pose; original = size;
        if (Rotate(Inverse(pose.orientation),Sub(ray.position,pose.position)).z < 0) u = 1-u;
        anchorLocal = {(u < ResizeBorderU ? .5f : u > 1-ResizeBorderU ? -.5f : 0)*size.width,
                       (v < ResizeBorderV ? -.5f : v > 1-ResizeBorderV ? .5f : 0)*size.height,0};
        anchor = Add(pose.position,Rotate(pose.orientation,anchorLocal));
        initialDelta = Sub({(u-.5f)*size.width,(.5f-v)*size.height,0},anchorLocal);
    }
    bool Move(XrPosef& pose, XrExtent2Df& size, XrPosef ray) const
    {
        const auto inverse = Inverse(plane.orientation);
        const auto origin = Rotate(inverse,Sub(ray.position,plane.position));
        const auto direction = Rotate(inverse,Rotate(ray.orientation,{0,0,-1}));
        if (std::fabs(direction.z) < 1e-5f) return false;
        const float t = -origin.z/direction.z;
        if (!std::isfinite(t) || t <= 0) return false;
        const auto delta = Sub(Add(origin,Scale(direction,t)),anchorLocal);
        const float scale = std::clamp(Dot(delta,initialDelta)/std::max(.0001f,Dot(initialDelta,initialDelta)),.35f/original.width,MaxPanelWidth/original.width);
        size = {original.width*scale,original.height*scale};
        pose.position = Sub(anchor,Rotate(plane.orientation,Scale(anchorLocal,scale)));
        return true;
    }
};

struct PanelDrag
{
    float distance = 0;
    float grabU = 0, grabV = 0, inertia = 1;
    XrVector3f grab{}, previousPivot{}, pivotVelocity{}, angularVelocity{};
    XrQuaternionf handOffset{0,0,0,1};
    XrVector3f Point(XrPosef ray) const
    { return Add(ray.position,Rotate(ray.orientation,{0,0,-distance})); }
    void PushPull(float stick, float dt)
    {
        if (std::fabs(stick) <= .2f) return;
        const float speed = (std::fabs(stick)-.2f)/.8f * (stick > 0 ? 2.0f : -2.0f);
        distance = std::clamp(distance + speed*std::clamp(dt,0.0f,.05f),.15f,6.0f);
    }
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
            const auto gravity = Sub({0,-MenuGravity,0},acceleration);
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
                angularVelocity = Scale(Add(angularVelocity,Scale(torque,step)),std::exp(-MenuAngularDamping*step));
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

inline XrQuaternionf RotationBetween(XrVector3f from, XrVector3f to)
{
    from = Normalize(from); to = Normalize(to);
    const float dot = std::clamp(Dot(from,to),-1.0f,1.0f);
    if (dot < -.9999f)
    {
        auto axis = Normalize(Cross(from,std::fabs(from.y) < .9f ? XrVector3f{0,1,0} : XrVector3f{1,0,0}));
        return {axis.x,axis.y,axis.z,0};
    }
    const auto axis = Cross(from,to);
    const float n = std::sqrt(2*(1+dot));
    return {axis.x/n,axis.y/n,axis.z/n,(1+dot)/n};
}

struct TwoHandDrag
{
    XrVector3f initialSpan{}, localMidpoint{}, previousCentre{};
    XrQuaternionf initialOrientation{0,0,0,1}, previousOrientation{0,0,0,1};
    XrQuaternionf initialHands[2]{};
    XrExtent2Df initialSize{};
    void Begin(XrPosef panel, XrExtent2Df size, const PanelDrag& first, const PanelDrag& second, XrPosef firstRay, XrPosef secondRay)
    {
        initialOrientation = previousOrientation = panel.orientation;
        initialSize = size; previousCentre = panel.position;
        initialSpan = Rotate(panel.orientation,Sub(second.grab,first.grab));
        localMidpoint = Scale(Add(first.grab,second.grab),.5f);
        initialHands[0] = firstRay.orientation; initialHands[1] = secondRay.orientation;
    }
    bool Move(XrPosef& panel, XrExtent2Df& size, PanelDrag& first, PanelDrag& second,
        XrPosef firstRay, XrPosef secondRay, float dt)
    {
        const auto a = first.Point(firstRay), b = second.Point(secondRay);
        const auto span = Sub(b,a);
        const float length = std::sqrt(Dot(span,span)), original = std::sqrt(Dot(initialSpan,initialSpan));
        if (length < .03f || original < .03f) return false;
        const float scale = std::clamp(length/original,.35f/initialSize.width,MaxPanelWidth/initialSize.width);
        const auto swing = RotationBetween(initialSpan,span);
        const auto firstTurn = Multiply(firstRay.orientation,Inverse(initialHands[0]));
        auto secondTurn = Multiply(secondRay.orientation,Inverse(initialHands[1]));
        if (firstTurn.x*secondTurn.x+firstTurn.y*secondTurn.y+firstTurn.z*secondTurn.z+firstTurn.w*secondTurn.w < 0)
            secondTurn = {-secondTurn.x,-secondTurn.y,-secondTurn.z,-secondTurn.w};
        auto handTurn = XrQuaternionf{firstTurn.x+secondTurn.x,firstTurn.y+secondTurn.y,firstTurn.z+secondTurn.z,firstTurn.w+secondTurn.w};
        const float hn = std::sqrt(handTurn.x*handTurn.x+handTurn.y*handTurn.y+handTurn.z*handTurn.z+handTurn.w*handTurn.w);
        handTurn = {handTurn.x/hn,handTurn.y/hn,handTurn.z/hn,handTurn.w/hn};
        const auto relative = Multiply(handTurn,Inverse(swing));
        const auto axis = Normalize(span);
        const float projection = Dot({relative.x,relative.y,relative.z},axis);
        const float tn = std::sqrt(projection*projection+relative.w*relative.w);
        const auto twist = tn > 1e-5f ? XrQuaternionf{axis.x*projection/tn,axis.y*projection/tn,axis.z*projection/tn,relative.w/tn} : XrQuaternionf{0,0,0,1};
        panel.orientation = Multiply(Multiply(twist,swing),initialOrientation);
        size = {initialSize.width*scale,initialSize.height*scale};
        panel.position = Sub(Scale(Add(a,b),.5f),Rotate(panel.orientation,Scale(localMidpoint,scale)));
        if (dt > 0)
        {
            auto q = Multiply(panel.orientation,Inverse(previousOrientation));
            if (q.w < 0) q = {-q.x,-q.y,-q.z,-q.w};
            const auto omega = Scale({q.x,q.y,q.z},2/dt);
            const auto centreVelocity = Scale(Sub(panel.position,previousCentre),1/dt);
            // Preserve the centre's release velocity when both triggers let go.
            first.angularVelocity = omega;
            first.pivotVelocity = Sub(centreVelocity,Cross(omega,Rotate(panel.orientation,Scale(first.grab,-1))));
        }
        previousCentre = panel.position; previousOrientation = panel.orientation;
        return true;
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

// Adaptive aim filtering removes small tremors, while large deliberate movements
// catch up quickly. Quaternions are interpolated on their shortest arc.
struct PointerAim
{
    XrPosef pose{};
    bool ready=false,wasPressed=false;
    void Reset() { ready=wasPressed=false; }
    XrPosef Step(XrPosef raw,float dt,bool pressed)
    {
        const bool edge=pressed!=wasPressed; wasPressed=pressed;
        if(!ready || dt<=0 || dt>.1f) { pose=raw; ready=true; return pose; }
        // The click uses the aim the user just saw, before squeezing/releasing
        // the trigger disturbed their hand. Subsequent drag samples catch up.
        if(edge) return pose;
        const auto delta=Sub(raw.position,pose.position);
        const auto turn=Sub(Rotate(raw.orientation,{0,0,-1}),Rotate(pose.orientation,{0,0,-1}));
        const float motion=std::clamp(std::max(std::sqrt(Dot(delta,delta))/.04f,std::sqrt(Dot(turn,turn))/.04f),0.0f,1.0f);
        const float tau=(pressed ? .10f:.065f)*(1-motion)+.008f*motion;
        const float alpha=1-std::exp(-dt/tau);
        pose.position=Add(pose.position,Scale(delta,alpha));
        auto q=raw.orientation;
        if(pose.orientation.x*q.x+pose.orientation.y*q.y+pose.orientation.z*q.z+pose.orientation.w*q.w<0) q={-q.x,-q.y,-q.z,-q.w};
        auto& p=pose.orientation;
        p={p.x+(q.x-p.x)*alpha,p.y+(q.y-p.y)*alpha,p.z+(q.z-p.z)*alpha,p.w+(q.w-p.w)*alpha};
        const float length=std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z+p.w*p.w);
        if(length>1e-6f) p={p.x/length,p.y/length,p.z/length,p.w/length};
        else p=raw.orientation;
        return pose;
    }
};

// Hold the clicked pixel through a small trigger squeeze/release wobble. Once a
// deliberate drag exceeds twelve physical panel pixels, motion is unrestricted.
struct StableClick
{
    bool held=false,moved=false;
    float x=0,y=0;
    void Reset() { held=moved=false; }
    void Apply(bool down,float& u,float& v)
    {
        if(down && !held) { x=u; y=v; moved=false; }
        if(held || down)
        {
            const float dx=(u-x)*1920,dy=(v-y)*1536;
            if(dx*dx+dy*dy>12*12) moved=true;
            if(!moved) { u=x; v=y; }
        }
        held=down;
    }
};

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
