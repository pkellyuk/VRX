#pragma once
#include <openxr/openxr.h>
#include <cmath>

// Heading only: an orientation with pitch and roll removed, so a floor under it is
// level. Looking straight up or down, the heading comes from the head's up vector.
inline XrQuaternionf YawOnly(const XrQuaternionf& q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    // forward = q * (0, 0, -1); up = q * (0, 1, 0)
    float fx = -(2 * (x * z + y * w)), fz = -(1 - 2 * (x * x + y * y));
    const float fy = -(2 * (y * z - x * w));
    if (fx * fx + fz * fz < 1e-8f)
    {
        const float ux = 2 * (x * y - z * w), uz = 2 * (y * z + x * w);
        const float sign = fy < 0 ? 1.0f : -1.0f;       // looking down: the top of the head points forward
        fx = sign * ux; fz = sign * uz;
    }
    const float yaw = std::atan2(-fx, -fz);
    return { 0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw) };
}

// One finite screen in LOCAL space. Only initial placement or an explicit
// recenter changes it; normal headset rotation/translation never moves it.
// `level` (the room is on) keeps only the recentre heading, so the room's floor is
// level; the raw orientation is kept, so turning it off restores any tilt.
struct ScreenAnchor
{
    static constexpr float distance = 3.0f;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrExtent2Df size{};
    bool pending = true;
    bool keyWasDown = false;
    bool level = false;
    XrPosef origin{{0,0,0,1}, {0,0,0}};

    void Adjust(float width, float depth, float height, float horizontal, float aspect)
    {
        const auto q = level ? YawOnly(origin.orientation) : origin.orientation;
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
        if (level) rotation = YawOnly(rotation);
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
