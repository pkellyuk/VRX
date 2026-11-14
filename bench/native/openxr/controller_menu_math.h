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

// A stationary console, facing the seated player. Its top edge clears the bottom
// of a level screen by five degrees. It is re-anchored only on opening/recentering,
// never when the player tilts their head down to read it.
inline XrPosef Place(XrVector3f eye, XrQuaternionf yaw, float screenBottomAngle, XrExtent2Df size)
{
    constexpr float distance = 1.2f, pi = 3.14159265358979323846f;
    const float half = std::atan2(size.height * .5f, distance);
    const float down = std::clamp(std::max(40*pi/180, screenBottomAngle + half + 5*pi/180), 40*pi/180, 75*pi/180);
    const XrQuaternionf pitch{-std::sin(down*.5f),0,0,std::cos(down*.5f)};
    const auto orientation = Multiply(yaw,pitch);
    return {orientation, Add(eye, Rotate(orientation,{0,0,-distance}))};
}

// UV origin is the bitmap's upper-left. Reject behind/parallel rays; a drag can
// request unbounded UVs so the slider still clamps correctly beyond its ends.
inline bool Hit(XrPosef panel, XrExtent2Df size, XrPosef aim, float& u, float& v, bool bounded = true)
{
    auto inverse = Inverse(panel.orientation);
    auto origin = Rotate(inverse, Sub(aim.position,panel.position));
    auto direction = Rotate(inverse, Rotate(aim.orientation,{0,0,-1}));
    if (!std::isfinite(direction.z) || direction.z >= -1e-5f || size.width <= 0 || size.height <= 0) return false;
    const float t = -origin.z / direction.z;
    if (!std::isfinite(t) || t <= 0) return false;
    u = .5f + (origin.x+t*direction.x)/size.width;
    v = .5f - (origin.y+t*direction.y)/size.height;
    return std::isfinite(u) && std::isfinite(v) && (!bounded || (u>=0 && u<=1 && v>=0 && v<=1));
}
}
