#ifndef BVH_H
#define BVH_H

#include <SDL3/SDL.h>
#include <algorithm>
#include <numeric>
#include <vector>

#define LAMBERTIAN 0
#define METAL 1
#define DIAELECTRIC 2


#define BVH_LEAF_SIZE 4

#include "object.h"

// Mirrors BVHNode in shader_comp.hlsl byte-for-byte. Two float3s (Min,
// Max) each need a trailing pad float for the same 16-byte-boundary
// reason as Sphere::pad0.
struct BVHNode_GPU
{
    float min_x, min_y, min_z;
    float pad0;
    float max_x, max_y, max_z;
    Sint32 left;
    Sint32 right;
    Sint32 count;
    Sint32 padding;
    Sint32 pad_trailing;
};

// Bounds a sphere across its motion this frame - the swept volume the
// BVH node must contain so a ray sampled at any time in [0,1] still
// finds it. If the sphere isn't moving (x2,y2,z2 == x,y,z) this just
// collapses to the ordinary sphere bounds.
inline AABB SphereBounds(const Sphere_GPU &s)
{
    AABB box{
        s.x - s.radius, s.y - s.radius, s.z - s.radius,
        s.x + s.radius, s.y + s.radius, s.z + s.radius};
    box = SurroundPoint(box, s.x2 - s.radius, s.y2 - s.radius, s.z2 - s.radius);
    box = SurroundPoint(box, s.x2 + s.radius, s.y2 + s.radius, s.z2 + s.radius);
    return box;
}

namespace detail
{
    // Recursively builds a BVH over `spheres[indices[start..end)]`,
    // appending flattened nodes to `outNodes` and a leaf-contiguous copy
    // of the spheres to `outSpheres`. Returns the index of the node it
    // just created within `outNodes`.
    //
    // Split strategy follows Ray Tracing: The Next Week's approach: pick
    // the axis the current range's bounding box is longest along, split
    // at the median centroid on that axis. O(n log n), no SAH, but more
    // than good enough to rebuild every frame for a few hundred spheres.
    inline int BuildRecursive(
        std::vector<int> &indices,
        int start,
        int end,
        const std::vector<Sphere_GPU> &spheres,
        const std::vector<AABB> &bounds,
        std::vector<BVHNode_GPU> &outNodes,
        std::vector<Sphere_GPU> &outSpheres)
    {
        AABB box = AABB::Empty();
        for (int i = start; i < end; i++)
        {
            box = Surround(box, bounds[indices[i]]);
        }

        const int nodeIndex = static_cast<int>(outNodes.size());
        outNodes.push_back(BVHNode_GPU{}); // placeholder, patched below

        const int count = end - start;
        if (count <= BVH_LEAF_SIZE)
        {
            const int first = static_cast<int>(outSpheres.size());
            for (int i = start; i < end; i++)
            {
                outSpheres.push_back(spheres[indices[i]]);
            }
            outNodes[nodeIndex] = BVHNode_GPU{
                box.min_x, box.min_y, box.min_z, 0,
                box.max_x, box.max_y, box.max_z, first,
                -1, count, 0, 0};
            return nodeIndex;
        }

        const int axis = box.LongestAxis();
        const int mid = start + count / 2;
        std::nth_element(
            indices.begin() + start,
            indices.begin() + mid,
            indices.begin() + end,
            [&](int a, int b)
            {
                return bounds[a].Centroid(axis) < bounds[b].Centroid(axis);
            });

        const int left = BuildRecursive(indices, start, mid, spheres, bounds, outNodes, outSpheres);
        const int right = BuildRecursive(indices, mid, end, spheres, bounds, outNodes, outSpheres);

        outNodes[nodeIndex] = BVHNode_GPU{
            box.min_x, box.min_y, box.min_z, 0,
            box.max_x, box.max_y, box.max_z, left,
            right, 0, 0, 0};
        return nodeIndex;
    }
}

// Builds a BVH over `spheres`. Call this fresh every frame after
// stepping physics/motion - it's cheap compared to a GPU rebuild for
// scenes of a few hundred to a few thousand spheres, and much simpler.
inline void BuildBVH(
    const std::vector<Sphere_GPU> &spheres,
    std::vector<BVHNode_GPU> &outNodes,
    std::vector<Sphere_GPU> &outSpheres)
{
    outNodes.clear();
    outSpheres.clear();

    if (spheres.empty())
    {
        return;
    }

    outNodes.reserve(spheres.size() * 2);
    outSpheres.reserve(spheres.size());

    std::vector<AABB> bounds(spheres.size());
    for (size_t i = 0; i < spheres.size(); i++)
    {
        bounds[i] = SphereBounds(spheres[i]);
    }

    std::vector<int> indices(spheres.size());
    std::iota(indices.begin(), indices.end(), 0);

    detail::BuildRecursive(indices, 0, static_cast<int>(indices.size()), spheres, bounds, outNodes, outSpheres);
}

#endif BVH_H