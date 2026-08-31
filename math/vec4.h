#ifndef Vec4_H
#define Vec4_H

#include <cmath>
#include <iostream>
#include <string>
#include <sstream>

template <typename T>
class Vec4 {
  public:
    union {
        T e[4]; // For array and index-based access: e[0], e[1], e[2]
        struct { 
            T x, y, z, w; // For direct named access: x, y, z, w
        };
    };

    Vec4() : x{0}, y{0}, z{0}, w{0} {}
    Vec4(T e0) : x{e0}, y{e0}, z{e0}, w{e0} {} 
    Vec4(T e0, T e1, T e2, T e3) : x{e0}, y{e1}, z{e2}, w{e3} {}

    T GetX() const { return x; }
    T GetY() const { return y; }
    T GetZ() const { return z; }
    T GetW() const { return w; }

    Vec4 operator-() const { return Vec4(-x, -y, -z, -w); }
    T operator[](int i) const { return e[i]; }
    T& operator[](int i) { return e[i]; }

    Vec4& operator+=(const Vec4& v) {
        x += v.x;
        y += v.y;
        z += v.z;
        w += v.w;
        return *this;
    }

    Vec4& operator*=(T t) {
        x *= t;
        y *= t;
        z *= t;
        w *= t;
        return *this;
    }

    Vec4& operator/=(T t) {
        return *this *= 1/t;
    }

    T length() const {
        return std::sqrt(length_squared());
    }

    T length_squared() const {
        return x*x + y*y + z*z + w*w;
    }

    std::string toString() const {
        std::stringstream ss;
        // Formats as: [x, y, z]
        ss << "()" << x << ", " << y << ", " << z << ", " << w << ")";
        return ss.str();
    }

       // Optional but recommended: Overload stream operator for direct std::cout printing
    friend std::ostream& operator<<(std::ostream& os, const Vec4& vec) {
        return os << vec.toString();
    }

    bool near_zero() const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = 1e-8;
        return (std::fabs(x) < s) && (std::fabs(y) < s) && (std::fabs(z) < s) && (std::fabs(w) < s);
    }

    bool near_zero(float tolerance) const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = tolerance;
        return (std::fabs(x) < s) && (std::fabs(y) < s) && (std::fabs(z) < s) && (std::fabs(w) < s);
    }

    // static Vec4 random() {
    //     return Vec4(random_double(), random_double(), random_double());
    // }

    // static Vec4 random(T min, T max) {
    //     return Vec4(random_double(min,max), random_double(min,max), random_double(min,max));
    // }
};

// Point3 is just an alias for Vec4, but useful for geometric clarity in the code.
using Point3 = Vec4<float>;
using Dir4 = Vec4<float>;


// Vector Utility Functions

template <typename T>
inline Vec4<T> operator+(const Vec4<T>& u, const Vec4<T>& v) {
    return Vec4(u.x + v.x, u.y + v.y, u.z + v.z, u.w + v.w);
}

template <typename T>
inline Vec4<T> operator-(const Vec4<T>& u, const Vec4<T>& v) {
    return Vec4(u.x - v.x, u.y - v.y, u.z - v.z, u.w - v.w);
}

template <typename T>
inline Vec4<T> operator*(const Vec4<T>& u, const Vec4<T>& v) {
    return Vec4(u.x * v.x, u.y * v.y, u.z * v.z, u.w * v.w);
}

template <typename T>
inline Vec4<T> operator*(T t, const Vec4<T>& v) {
    return Vec4<T>(t*v.x, t*v.y, t*v.z, t*v.w);
}

template <typename T>
inline Vec4<T> operator*(const Vec4<T>& v, T t) {
    return t * v;
}

template <typename T>
inline Vec4<T> operator/(const Vec4<T>& v, T t) {
    return (1/t) * v;
}

template <typename T>
inline T dot(const Vec4<T>& u, const Vec4<T>& v) {
    return u.x * v.x
         + u.y * v.y
         + u.z * v.z
         + u.w * v.w;
}

template <typename T>
inline Vec4<T> normalize(const Vec4<T>& v) {
    return v / v.length();
}

#endif
