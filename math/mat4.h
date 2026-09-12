#ifndef MAT4_H
#define MAT4_H

#include <cmath>

#include "math/vec3.h"

// Row-major 4x4: m[row][col]. Uploaded verbatim to HLSL, where the matching
// field is declared `row_major float4x4`. HLSL's cbuffer DEFAULT is
// column-major, so leaving that keyword off silently transposes every matrix
// you upload - which looks like a scene that is inside-out and rotated, not
// like an obvious error. The keyword is load-bearing.
struct Mat4
{
    float m[4][4];

    static Mat4 Identity()
    {
        Mat4 r = {};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

// Transforms a point (w = 1) and divides through. Used only by the CPU-side
// agreement test in Phase 1.5 - the GPU does this itself.
inline Vec3<float> TransformPoint(const Mat4 &a, const Vec3<float> &p, float &outW)
{
    const float x = a.m[0][0]*p.x + a.m[0][1]*p.y + a.m[0][2]*p.z + a.m[0][3];
    const float y = a.m[1][0]*p.x + a.m[1][1]*p.y + a.m[1][2]*p.z + a.m[1][3];
    const float z = a.m[2][0]*p.x + a.m[2][1]*p.y + a.m[2][2]*p.z + a.m[2][3];
    outW          = a.m[3][0]*p.x + a.m[3][1]*p.y + a.m[3][2]*p.z + a.m[3][3];
    return Vec3<float>(x, y, z);
}

// Right-handed look-at, built from EXACTLY the basis the compute shader's
// main() builds. If you ever change the basis in one place, change it here in
// the same commit - the two disagreeing is the hybrid renderer's signature bug.
//
//   w = normalize(eye - target)     backward
//   u = normalize(cross(up, w))     right
//   v = cross(w, u)                 up
//
// The result maps world space to view space with x right, y up, z BACKWARD,
// so anything the camera can see has a negative view-space z.
inline Mat4 LookAtRH(const Vec3<float> &eye,
                     const Vec3<float> &target,
                     const Vec3<float> &up)
{
    const Vec3<float> w = normalize(eye - target);
    const Vec3<float> u = normalize(cross(up, w));
    const Vec3<float> v = cross(w, u);

    Mat4 r = Mat4::Identity();
    r.m[0][0] = u.x; r.m[0][1] = u.y; r.m[0][2] = u.z; r.m[0][3] = -dot(u, eye);
    r.m[1][0] = v.x; r.m[1][1] = v.y; r.m[1][2] = v.z; r.m[1][3] = -dot(v, eye);
    r.m[2][0] = w.x; r.m[2][1] = w.y; r.m[2][2] = w.z; r.m[2][3] = -dot(w, eye);
    return r;
}

// Right-handed perspective for SDL_gpu clip space: depth in [0,1] (not [-1,1]
// like OpenGL) and NDC +Y pointing UP the screen.
//
// That is SDL_gpu's convention on EVERY backend, Vulkan included - see the
// "Normalized Device Coordinates" note in SDL_gpu.h. Raw Vulkan is +Y down, but
// SDL's Vulkan backend flips the viewport (negative height) to match D3D12 and
// Metal. So m[1][1] is a plain positive f: a -f here, which is the usual
// hand-written-Vulkan advice, renders the raster image upside down under SDL.
// The compute shader's viewportV carries a -vectorV because its pixel y grows
// downward; SDL's viewport transform does the same flip on the raster side.
//
// fovYDegrees matches CameraComponent::fov and the shader's `Fov` uniform -
// both are the FULL vertical angle in degrees.
inline Mat4 PerspectiveRH_ZO(float fovYDegrees, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovYDegrees * 0.017453292519943295f * 0.5f);

    Mat4 r = {};
    r.m[0][0] =  f / aspect;
    r.m[1][1] =  f;                                  // SDL_gpu NDC is +Y up
    r.m[2][2] =  zFar / (zNear - zFar);
    r.m[2][3] =  (zNear * zFar) / (zNear - zFar);
    r.m[3][2] = -1.0f;
    return r;
}

// Recovers view-space distance from a [0,1] depth-buffer value written by
// PerspectiveRH_ZO. The algebra is just that matrix inverted for z:
//   ndcZ = zFar*(zNear - z) / ((zNear - zFar) * z)   =>   z = zn*zf / (zf + d*(zn - zf))
// Phase 6 uses this to drop gPosition and reconstruct world space from depth.
inline float LinearizeDepth(float d, float zNear, float zFar)
{
    return (zNear * zFar) / (zFar + d * (zNear - zFar));
}

#endif // MAT4_H