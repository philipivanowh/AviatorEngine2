#ifndef VEC3_H
#define VEC3_H

#include <iostream>
#include <string>
#include <sstream>

template <typename T>
class Vec3 {
  public:
    union {
        T e[3]; // For array and index-based access: e[0], e[1], e[2]
        struct { 
            T x, y, z; // For direct named access: x, y, z
        };
    };

    Vec3() : x{0}, y{0}, z{0} {}
    Vec3(T e0, T e1, T e2) : x{e0}, y{e1}, z{e2} {}

    T GetX() const { return x; }
    T GetY() const { return y; }
    T GetZ() const { return z; }

    Vec3 operator-() const { return Vec3(-x, -y, -z); }
    T operator[](int i) const { return e[i]; }
    T& operator[](int i) { return e[i]; }

    Vec3& operator+=(const Vec3& v) {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }

    Vec3& operator*=(T t) {
        x *= t;
        y *= t;
        z *= t;
        return *this;
    }

    Vec3& operator/=(T t) {
        return *this *= 1/t;
    }

    T length() const {
        return std::sqrt(length_squared());
    }

    T length_squared() const {
        return x*x + y*y + z*z;
    }

    std::string toString() const {
        std::stringstream ss;
        // Formats as: [x, y, z]
        ss << "()" << x << ", " << y << ", " << z << ")";
        return ss.str();
    }

       // Optional but recommended: Overload stream operator for direct std::cout printing
    friend std::ostream& operator<<(std::ostream& os, const Vec3& vec) {
        return os << vec.toString();
    }

    bool near_zero() const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = 1e-8;
        return (std::fabs(x) < s) && (std::fabs(y) < s) && (std::fabs(z) < s);
    }

    bool near_zero(float tolerance) const {
        // Return true if the vector is close to zero in all dimensions.
        auto s = tolerance;
        return (std::fabs(x) < s) && (std::fabs(y) < s) && (std::fabs(z) < s);
    }

    // static Vec3 random() {
    //     return Vec3(random_double(), random_double(), random_double());
    // }

    // static Vec3 random(T min, T max) {
    //     return Vec3(random_double(min,max), random_double(min,max), random_double(min,max));
    // }
};

// point3 is just an alias for Vec3, but useful for geometric clarity in the code.
using point3 = Vec3<float>;


// Vector Utility Functions

template <typename T>
inline Vec3<T> operator+(const Vec3<T>& u, const Vec3<T>& v) {
    return Vec3(u.x + v.x, u.y + v.y, u.z + v.z);
}

template <typename T>
inline Vec3<T> operator-(const Vec3<T>& u, const Vec3<T>& v) {
    return Vec3(u.x - v.x, u.y - v.y, u.z - v.z);
}

template <typename T>
inline Vec3<T> operator*(const Vec3<T>& u, const Vec3<T>& v) {
    return Vec3(u.x * v.x, u.y * v.y, u.z * v.z);
}

template <typename T>
inline Vec3<T> operator*(T t, const Vec3<T>& v) {
    return Vec3<T>(t*v.x, t*v.y, t*v.z);
}

template <typename T>
inline Vec3<T> operator*(const Vec3<T>& v, T t) {
    return t * v;
}

template <typename T>
inline Vec3<T> operator/(const Vec3<T>& v, T t) {
    return (1/t) * v;
}

template <typename T>
inline T dot(const Vec3<T>& u, const Vec3<T>& v) {
    return u.x * v.x
         + u.y * v.y
         + u.z * v.z;
}

template <typename T>
inline Vec3<T> cross(const Vec3<T>& u, const Vec3<T>& v) {
    return Vec3<T>(u.y * v.z - u.z * v.y,
                u.z * v.x - u.x * v.z,
                u.x * v.y - u.y * v.x);
}

template <typename T>
inline Vec3<T> normalize(const Vec3<T>& v) {
    return v / v.length();
}

// template <typename T>
// inline Vec3<T> random_in_unit_disk() {
//     while (true) {
//         auto p = Vec3<T>(random_double(-1,1), random_double(-1,1), 0);
//         if (p.length_squared() < 1)
//             return p;
//     }
// }

// template <typename T>
// inline Vec3<T> random_unit_vector() {
//     while (true) {
//         auto p = Vec3<T>::random(-1,1);
//         auto lensq = p.length_squared();
//         if (1e-160 < lensq && lensq <= 1.0)
//             return p / sqrt(lensq);
//     }
// }

// template <typename T>
// inline Vec3<T> random_on_hemisphere(const Vec3<T>& normal) {
//     Vec3<T> on_unit_sphere = random_unit_vector();
//     if (dot(on_unit_sphere, normal) > 0.0) // In the same hemisphere as the normal
//         return on_unit_sphere;
//     else
//         return -on_unit_sphere;
// }

template <typename T>
inline Vec3<T> reflect(const Vec3<T>& v, const Vec3<T>& n) {
    return v - 2*dot(v,n)*n;
}

template <typename T>
inline Vec3<T> refract(const Vec3<T>& uv, const Vec3<T>& n, T etai_over_etat) {
    auto cos_theta = std::fmin(dot(-uv, n), 1.0);
    Vec3<T> r_out_perp =  etai_over_etat * (uv + cos_theta*n);
    Vec3<T> r_out_parallel = -std::sqrt(std::fabs(1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_parallel;
}


#endif