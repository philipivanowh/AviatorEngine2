#ifndef VEC3_H
#define VEC3_H

#include <cmath>
#include <iostream>
#include <string>
#include <sstream>

template <typename T>
class Vec2 {
  public:
    union {
        T e[2]; // For array and index-based access: e[0], e[1], e[2]
        struct { 
            T x, y; // For direct named access: x, y, z
        };
    };

    Vec2() : x{0}, y{0} {}
    Vec2(T e0) : x{e0}, y{e0} {} 
    Vec2(T e0, T e1) : x{e0}, y{e1} {}

    T GetX() const { return x; }
    T GetY() const { return y; }

    Vec2 operator-() const { return Vec2(-x, -y); }
    T operator[](int i) const { return e[i]; }
    T& operator[](int i) { return e[i]; }

    Vec2& operator+=(const Vec2& v) {
        x += v.x;
        y += v.y;
        return *this;
    }

    Vec2& operator*=(T t) {
        x *= t;
        y *= t;
        return *this;
    }

    Vec2& operator/=(T t) {
        return *this *= 1/t;
    }

    T length() const {
        return std::sqrt(length_squared());
    }

    T length_squared() const {
        return x*x + y*y;
    }

    std::string toString() const {
        std::stringstream ss;
        // Formats as: [x, y, z]
        ss << "()" << x << ", " << y << ")";
        return ss.str();
    }

       // Optional but recommended: Overload stream operator for direct std::cout printing
    friend std::ostream& operator<<(std::ostream& os, const Vec2& vec) {
        return os << vec.toString();
    }

    bool near_zero() const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = 1e-8;
        return (std::fabs(x) < s) && (std::fabs(y) < s);
    }

    bool near_zero(float tolerance) const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = tolerance;
        return (std::fabs(x) < s) && (std::fabs(y) < s);
    }

    // static Vec2 random() {
    //     return Vec2(random_double(), random_double(), random_double());
    // }

    // static Vec2 random(T min, T max) {
    //     return Vec2(random_double(min,max), random_double(min,max), random_double(min,max));
    // }
};

// Point3 is just an alias for Vec2, but useful for geometric clarity in the code.
using Point2 = Vec2<float>;
using Dir2 = Vec2<float>;


// Vector Utility Functions

template <typename T>
inline Vec2<T> operator+(const Vec2<T>& u, const Vec2<T>& v) {
    return Vec2(u.x + v.x, u.y + v.y);
}

template <typename T>
inline Vec2<T> operator-(const Vec2<T>& u, const Vec2<T>& v) {
    return Vec2(u.x - v.x, u.y - v.y);
}

template <typename T>
inline Vec2<T> operator*(const Vec2<T>& u, const Vec2<T>& v) {
    return Vec2(u.x * v.x, u.y * v.y);
}

template <typename T>
inline Vec2<T> operator*(T t, const Vec2<T>& v) {
    return Vec2<T>(t*v.x, t*v.y);
}

template <typename T>
inline Vec2<T> operator*(const Vec2<T>& v, T t) {
    return t * v;
}

template <typename T>
inline Vec2<T> operator/(const Vec2<T>& v, T t) {
    return (1/t) * v;
}

template <typename T>
inline T dot(const Vec2<T>& u, const Vec2<T>& v) {
    return u.x * v.x
         + u.y * v.y;
}


template <typename T>
inline Vec2<T> normalize(const Vec2<T>& v) {
    return v / v.length();
}

template <typename T>
inline Vec2<T> reflect(const Vec2<T>& v, const Vec2<T>& n) {
    return v - 2*dot(v,n)*n;
}

template <typename T>
inline Vec2<T> refract(const Vec2<T>& uv, const Vec2<T>& n, T etai_over_etat) {
    auto cos_theta = std::fmin(dot(-uv, n), 1.0);
    Vec2<T> r_out_perp =  etai_over_etat * (uv + cos_theta*n);
    Vec2<T> r_out_parallel = -std::sqrt(std::fabs(1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_parallel;
}


#endif
