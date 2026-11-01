#pragma once

#include <cmath>

// Minimal math for tracking-space poses. Conventions match OpenXR: right handed, +Y up, -Z
// forward, meters. The game world is also right handed with +Y up, so only scale, yaw and an
// origin separate the two.
namespace dusk::vr {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(dot(*this)); }
    float lengthXZ() const { return std::sqrt(x * x + z * z); }
    Vec3 normalized() const {
        const float l = length();
        return l > 1e-6f ? *this * (1.f / l) : Vec3{};
    }
};

struct Quat {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;

    Quat operator*(const Quat& b) const {
        return {
            w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z,
        };
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
    Vec3 rotate(const Vec3& v) const {
        const Vec3 q{x, y, z};
        const Vec3 t = q.cross(v) * 2.f;
        return v + t * w + q.cross(t);
    }
    static Quat axisAngle(const Vec3& axis, float rad) {
        const float s = std::sin(rad * 0.5f);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(rad * 0.5f)};
    }
    static Quat yaw(float rad) { return axisAngle({0.f, 1.f, 0.f}, rad); }
};

struct Pose {
    Quat orientation;
    Vec3 position;

    Vec3 forward() const { return orientation.rotate({0.f, 0.f, -1.f}); }
    Vec3 up() const { return orientation.rotate({0.f, 1.f, 0.f}); }
    Vec3 right() const { return orientation.rotate({1.f, 0.f, 0.f}); }
};

inline Quat normalized(const Quat& q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-6f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}

// Shortest rotation taking direction a onto direction b.
inline Quat from_to(const Vec3& a, const Vec3& b) {
    const Vec3 u = a.normalized();
    const Vec3 v = b.normalized();
    const float d = u.dot(v);
    if (d > 0.99999f) {
        return {};
    }
    if (d < -0.99999f) {
        Vec3 axis = Vec3{1.f, 0.f, 0.f}.cross(u);
        if (axis.length() < 1e-3f) {
            axis = Vec3{0.f, 1.f, 0.f}.cross(u);
        }
        return Quat::axisAngle(axis.normalized(), 3.14159265358979f);
    }
    const Vec3 c = u.cross(v);
    return normalized({c.x, c.y, c.z, 1.f + d});
}

// Yaw (radians) of a direction around +Y, measured so that yaw(0) looks down -Z.
inline float yaw_of(const Vec3& dir) { return std::atan2(-dir.x, -dir.z); }

// Removes pitch and roll, keeping only the heading of a rotation.
inline Quat heading_only(const Quat& q) { return Quat::yaw(yaw_of(q.rotate({0.f, 0.f, -1.f}))); }

constexpr float kPi = 3.14159265358979f;
inline float deg2rad(float d) { return d * (kPi / 180.f); }

inline float wrap_pi(float a) {
    while (a > kPi) a -= 2.f * kPi;
    while (a < -kPi) a += 2.f * kPi;
    return a;
}

}  // namespace dusk::vr
