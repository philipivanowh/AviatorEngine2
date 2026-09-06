#pragma once

#include <algorithm>
#include <cfloat>

#include "math/vec3.h"

// A minimal axis-aligned bounding box. Deliberately has no knowledge of
// Sphere/scene types so it stays reusable if other primitive types are
// added later - see bvh.h for the Sphere -> AABB conversion.

inline const float bounds_minimum = 0.0001f;

struct AABB
{
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;

    static AABB Empty()
    {
        return AABB{FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
    }

    float Extent(int axis) const
    {
        switch (axis)
        {
        case 0:
            return max_x - min_x;
        case 1:
            return max_y - min_y;
        default:
            return max_z - min_z;
        }
    }

    float Centroid(int axis) const
    {
        switch (axis)
        {
        case 0:
            return (min_x + max_x) * 0.5f;
        case 1:
            return (min_y + max_y) * 0.5f;
        default:
            return (min_z + max_z) * 0.5f;
        }
    }

    // Which axis this box is longest along - used to pick the BVH split
    // axis, same heuristic as Ray Tracing: The Next Week.
    int LongestAxis() const
    {
        const float ex = Extent(0);
        const float ey = Extent(1);
        const float ez = Extent(2);
        if (ex > ey && ex > ez)
            return 0;
        if (ey > ez)
            return 1;
        return 2;
    }
};




// Smallest AABB containing both a and b. Used to build interior node
// bounds bottom-up from their children during the BVH build.
inline AABB Surround(const AABB &a, const AABB &b)
{
    // Choose the smaller side of the bound first and making sure that it is not zero value that will crush the 3d bounds
    return AABB{
        std::min(a.min_x, b.min_x),
        std::min(a.min_y, b.min_y),
        std::min(a.min_z, b.min_z),
        std::max(a.max_x, b.max_x),
        std::max(a.max_y, b.max_y),
        std::max(a.max_z, b.max_z)};
}

// Grows box a so it also contains the point (x, y, z). Used to extend a
// sphere's resting bounds to also cover its position at the other end of
// this frame's motion (motion blur shutter, or last frame's position for
// a swept/physics bound).
inline AABB SurroundPoint(const AABB &a, float x, float y, float z)
{
    return AABB{
        std::min(a.min_x, x),
        std::min(a.min_y, y),
        std::min(a.min_z, z),
        std::max(a.max_x, x),
        std::max(a.max_y, y),
        std::max(a.max_z, z)};
}

inline AABB Surround2Points(const Point3& a, const Point3& b)
{
    return AABB{
        std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z),
        std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)
    };
}
