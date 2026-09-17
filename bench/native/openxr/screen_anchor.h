#pragma once
#include <openxr/openxr.h>
#include <cmath>

// One finite screen in LOCAL space. Only initial placement or an explicit
// recenter changes it; normal headset rotation/translation never moves it.
struct ScreenAnchor
{
    static constexpr float distance = 3.0f;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrExtent2Df size{};
    bool pending = true;
    bool keyWasDown = false;
    XrPosef origin{{0,0,0,1}, {0,0,0}};

    void Adjust(float width, float depth, float height, float horizontal, float aspect)
    {
        const auto q = origin.orientation;
        // Rotate the saved local offset, not today's headset pose. Sliders must
        // not accidentally recenter a stationary screen when the player moves.
        const float vx = horizontal, vy = height, vz = -depth;
        const float tx = 2 * (q.y * vz - q.z * vy);
        const float ty = 2 * (q.z * vx - q.x * vz);
        const float tz = 2 * (q.x * vy - q.y * vx);
        pose = {q, {origin.position.x + vx + q.w * tx + q.y * tz - q.z * ty,
            origin.position.y + vy + q.w * ty + q.z * tx - q.x * tz,
            origin.position.z + vz + q.w * tz + q.x * ty - q.y * tx}};
        size = {width, width * aspect};
    }

    void Key(bool down)
    {
        if (down && !keyWasDown) pending = true;
        keyWasDown = down;
    }

    bool Place(const XrPosef& left, const XrPosef& right,
        XrQuaternionf rotation, float tanHalfX, float aspect)
    {
        if (!pending) return false;
        origin = {rotation, {(left.position.x + right.position.x) * .5f,
            (left.position.y + right.position.y) * .5f, (left.position.z + right.position.z) * .5f}};
        // Rotate (0,0,-distance) by the normalized headset quaternion.
        const float x = -2 * distance * (rotation.x * rotation.z + rotation.w * rotation.y);
        const float y = -2 * distance * (rotation.y * rotation.z - rotation.w * rotation.x);
        const float z = -distance * (1 - 2 * (rotation.x * rotation.x + rotation.y * rotation.y));
        pose = {rotation, {(left.position.x + right.position.x) * .5f + x,
            (left.position.y + right.position.y) * .5f + y,
            (left.position.z + right.position.z) * .5f + z}};
        size = {2 * distance * tanHalfX, 2 * distance * tanHalfX * aspect};
        pending = false;
        return true;
    }
};
