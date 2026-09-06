#pragma once

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cassert>

template <typename T>
struct Quat {
    union { T x; T r; T s; };
    union { T y; T g; T t; };
    union { T z; T b; T p; };
    union { T w; T a; T q; };

    // -- Component accesses --
    typedef size_t size_type;
    
    constexpr size_type length() const { return 4; }

    constexpr T& operator[](size_type i) {
        assert(i >= 0 && i < 4);
        return (&x)[i];
    }

    constexpr const T& operator[](size_type i) const {
        assert(i >= 0 && i < 4);
        return (&x)[i];
    }

    // -- Implicit constructors --
    constexpr Quat() : x(0), y(0), z(0), w(1) {}
    constexpr Quat(const Quat<T>& q) : x(q.x), y(q.y), z(q.z), w(q.w) {}

    // -- Explicit constructors --
    constexpr Quat(T _s, T _x, T _y, T _z) : x(_x), y(_y), z(_z), w(_s) {} // GLM layout convention: w (s) is first in parameters, but layout is x, y, z, w
    constexpr Quat(T _x, T _y, T _z, T _w, int) : x(_x), y(_y), z(_z), w(_w) {} // Internal raw layout

    // -- Unary arithmetic operators --
    Quat<T>& operator=(const Quat<T>& q) {
        this->x = q.x; this->y = q.y; this->z = q.z; this->w = q.w;
        return *this;
    }

    template <typename U>
    Quat<T>& operator=(const Quat<U>& q) {
        this->x = static_cast<T>(q.x);
        this->y = static_cast<T>(q.y);
        this->z = static_cast<T>(q.z);
        this->w = static_cast<T>(q.w);
        return *this;
    }

    Quat<T>& operator+=(const Quat<T>& q) {
        this->x += q.x; this->y += q.y; this->z += q.z; this->w += q.w;
        return *this;
    }

    Quat<T>& operator-=(const Quat<T>& q) {
        this->x -= q.x; this->y -= q.y; this->z -= q.z; this->w -= q.w;
        return *this;
    }

    Quat<T>& operator*=(const Quat<T>& q) {
        Quat<T> const p(*this);
        this->w = p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z;
        this->x = p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y;
        this->y = p.w * q.y + p.y * q.w + p.z * q.x - p.x * q.z;
        this->z = p.w * q.z + p.z * q.w + p.x * q.y - p.y * q.x;
        return *this;
    }

    Quat<T>& operator*=(T s) {
        this->x *= s; this->y *= s; this->z *= s; this->w *= s;
        return *this;
    }

    Quat<T>& operator/=(T s) {
        this->x /= s; this->y /= s; this->z /= s; this->w /= s;
        return *this;
    }
};

// -- Binary operators --
template <typename T>
Quat<T> operator+(Quat<T> const& q, Quat<T> const& p) {
    return Quat<T>(q) += p;
}

template <typename T>
Quat<T> operator-(Quat<T> const& q, Quat<T> const& p) {
    return Quat<T>(q) -= p;
}

template <typename T>
Quat<T> operator*(Quat<T> const& q, Quat<T> const& p) {
    return Quat<T>(q) *= p;
}

template <typename T>
Quat<T> operator*(Quat<T> const& q, T const& s) {
    return Quat<T>(q) *= s;
}

template <typename T>
Quat<T> operator*(T const& s, Quat<T> const& q) {
    return Quat<T>(q) *= s;
}

template <typename T>
Quat<T> operator/(Quat<T> const& q, T const& s) {
    return Quat<T>(q) /= s;
}

// -- Unary operators --
template <typename T>
Quat<T> operator-(Quat<T> const& q) {
    return Quat<T>(-q.w, -q.x, -q.y, -q.z);
}

// -- Boolean operators --
template <typename T>
bool operator==(Quat<T> const& q1, Quat<T> const& q2) {
    return q1.x == q2.x && q1.y == q2.y && q1.z == q2.z && q1.w == q2.w;
}

template <typename T>
bool operator!=(Quat<T> const& q1, Quat<T> const& q2) {
    return !(q1 == q2);
}

// -- Geometric Functions --
template <typename T>
T dot(Quat<T> const& q1, Quat<T> const& q2) {
    return q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w;
}

template <typename T>
T length(Quat<T> const& q) {
    return std::sqrt(dot(q, q));
}

template <typename T>
Quat<T> normalize(Quat<T> const& q) {
    T len = length(q);
    if (len <= T(0)) {
        return Quat<T>(T(0), T(0), T(0), T(1));
    }
    return q / len;
}

template <typename T>
Quat<T> conjugate(Quat<T> const& q) {
    return Quat<T>(q.w, -q.x, -q.y, -q.z);
}

template <typename T>
Quat<T> inverse(Quat<T> const& q) {
    return conjugate(q) / dot(q, q);
}

template <typename T>
Quat<T> angleAxis(T const& angle, T const& x, T const& y, T const& z) {
    T const halfAngle = angle * static_cast<T>(0.5);
    T const s = std::sin(halfAngle);
    return Quat<T>(std::cos(halfAngle), x * s, y * s, z * s);
}

template <typename T>
Quat<T> slerp(Quat<T> const& x, Quat<T> const& y, T const& a) {
    Quat<T> z = y;
    T cosTheta = dot(x, y);

    // If cosTheta < 0, the interpolation will take the long way around the sphere.
    // To fix this, one Quaternion must be negated.
    if (cosTheta < static_cast<T>(0)) {
        z = -y;
        cosTheta = -cosTheta;
    }

    // Perform a linear interpolation for close orientations
    if (cosTheta > static_cast<T>(1) - std::numeric_limits<T>::epsilon()) {
        return Quat<T>(
            x.x + a * (z.x - x.x),
            x.y + a * (z.y - x.y),
            x.z + a * (z.z - x.z),
            x.w + a * (z.w - x.w),
            0
        );
    } else {
        T const angle = std::acos(cosTheta);
        return (std::sin((static_cast<T>(1) - a) * angle) * x + std::sin(a * angle) * z) / std::sin(angle);
    }
}


// -- Interop with Vec3 ------------------------------------------------------
//
// Kept at the bottom so the quaternion algebra above stays independent of the
// vector type; only these helpers need it.

#include "vec3.h"

// v rotated by the unit quaternion q, i.e. q * v * conj(q). Written in the
// cross-product form rather than by building a 3x3 matrix: 2 crosses and 2
// adds beats 9 multiplies plus the matrix construction, and it is what every
// call site here wants (one vector, one quaternion, no reuse).
//
// Assumes q is normalized. Rotations composed over many frames drift, so
// physics should normalize() its orientation after integrating.
template <typename T>
inline Vec3<T> rotate(Quat<T> const &q, Vec3<T> const &v)
{
    const Vec3<T> u(q.x, q.y, q.z);
    const Vec3<T> t = static_cast<T>(2) * cross(u, v);
    return v + q.w * t + cross(u, t);
}

// The rotation about +Y contained in q, in radians.
//
// This is the twist half of a swing-twist decomposition about Y, which is
// exact when q is a pure Y rotation and a reasonable projection otherwise.
// The shader can only represent a single yaw angle for spheres and boxes
// (Object_GPU::rotation), so this is how a general orientation gets squeezed
// into what the GPU understands - see YawOnly() in shapes.h for the check
// that catches the cases where that squeeze would lose something.
template <typename T>
inline T YawOf(Quat<T> const &q)
{
    return static_cast<T>(2) * std::atan2(q.y, q.w);
}

// A quaternion for `radians` about the given world axis. Thin wrappers over
// angleAxis so scene code reads the same way the old Object::RotateY did.
template <typename T>
inline Quat<T> QuatX(T radians) { return angleAxis(radians, T(1), T(0), T(0)); }
template <typename T>
inline Quat<T> QuatY(T radians) { return angleAxis(radians, T(0), T(1), T(0)); }
template <typename T>
inline Quat<T> QuatZ(T radians) { return angleAxis(radians, T(0), T(0), T(1)); }
