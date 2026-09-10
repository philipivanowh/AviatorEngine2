#ifndef BVH_H
#define BVH_H

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "scene/aabb.h"

#define BVH_LEAF_SIZE 4

// The BVH knows nothing about what it is built over. It is handed a flat list
// of world-space (or local-space) AABBs and hands back the node tree plus a
// permutation saying which primitive ended up in which leaf slot.
//
// That last part is what makes this file reusable at both levels of the
// two-level structure. The top level maps the permutation back to entities; a
// mesh maps it back to triangles. Neither meaning lives here, which is why
// this header no longer includes entt.
//
// Bounds are passed in rather than computed because the caller already walks
// its own data to produce them (see GatherRenderables in shapes.h), and doing
// it in one pass keeps that data in cache.

// Mirrors BVHNode in the compute shaders byte-for-byte. Two float3s (Min,
// Max) each need a trailing pad float for the same 16-byte-boundary reason as
// Object_GPU::pad0.
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

static_assert(sizeof(BVHNode_GPU) == 48, "BVHNode_GPU must match the HLSL BVHNode stride");

namespace detail
{
    // Turns a box into a node, inflating any axis that is flat.
    //
    // A perfectly axis-aligned primitive - a quad in the XY plane, or any face
    // of Mesh::CreateBox - produces a zero-thickness AABB. The shader's slab
    // test then computes (bmin - origin) * (1/0), and when the ray origin lies
    // exactly in that plane it evaluates 0 * inf = NaN. Every NaN comparison is
    // false, so the node is silently skipped and the primitive disappears from
    // some angles but not others.
    //
    // bounds_minimum in aabb.h has existed for exactly this since before the
    // BVH did; this is the first code to actually use it.
    inline BVHNode_GPU MakeNode(const AABB &box, int left, int right, int count)
    {
        BVHNode_GPU node = {};

        node.min_x = box.min_x;
        node.min_y = box.min_y;
        node.min_z = box.min_z;
        node.max_x = box.max_x;
        node.max_y = box.max_y;
        node.max_z = box.max_z;

        if (node.max_x - node.min_x < bounds_minimum)
        {
            node.min_x -= bounds_minimum * 0.5f;
            node.max_x += bounds_minimum * 0.5f;
        }
        if (node.max_y - node.min_y < bounds_minimum)
        {
            node.min_y -= bounds_minimum * 0.5f;
            node.max_y += bounds_minimum * 0.5f;
        }
        if (node.max_z - node.min_z < bounds_minimum)
        {
            node.min_z -= bounds_minimum * 0.5f;
            node.max_z += bounds_minimum * 0.5f;
        }

        node.left = left;
        node.right = right;
        node.count = count;
        return node;
    }

    inline int BuildRecursive(
        std::vector<uint32_t> &indices,
        int start,
        int end,
        const std::vector<AABB> &bounds,
        std::vector<BVHNode_GPU> &outNodes,
        std::vector<uint32_t> &outOrder)
    {
        AABB box = AABB::Empty();
        for (int i = start; i < end; i++)
        {
            box = Surround(box, bounds[indices[i]]);
        }

        // Reserve this node before descending so its index remains stable
        // while recursive calls add its children.
        const int nodeIndex = static_cast<int>(outNodes.size());
        outNodes.push_back(BVHNode_GPU{});

        const int count = end - start;
        if (count <= BVH_LEAF_SIZE)
        {
            // `left` on a leaf is the first slot in outOrder, not a node index.
            // `count > 0` is what tells the shader which meaning to read.
            const int first = static_cast<int>(outOrder.size());
            for (int i = start; i < end; i++)
            {
                outOrder.push_back(indices[i]);
            }
            outNodes[nodeIndex] = MakeNode(box, first, -1, count);

            return nodeIndex;
        }

        const int axis = box.LongestAxis();
        const int mid = start + count / 2;
        std::nth_element(
            indices.begin() + start,
            indices.begin() + mid,
            indices.begin() + end,
            [&](uint32_t a, uint32_t b)
            {
                return bounds[a].Centroid(axis) < bounds[b].Centroid(axis);
            });

        const int left = BuildRecursive(indices, start, mid, bounds, outNodes, outOrder);
        const int right = BuildRecursive(indices, mid, end, bounds, outNodes, outOrder);

        outNodes[nodeIndex] = MakeNode(box, left, right, 0);

        return nodeIndex;
    }
}

// Builds a BVH over `bounds`. Node 0 is the root.
//
// outOrder is a permutation of [0, bounds.size()): outOrder[i] is the index of
// the primitive that the build placed at slot i, and leaves address primitives
// by slot. The caller MUST reorder its own primitive data to match, because a
// leaf saying "slots 4..7" means nothing otherwise. At the top level that is
// the entity list; inside a mesh it is the index buffer.
inline void BuildBVH(
    const std::vector<AABB> &bounds,
    std::vector<BVHNode_GPU> &outNodes,
    std::vector<uint32_t> &outOrder)
{
    outNodes.clear();
    outOrder.clear();

    if (bounds.empty())
    {
        return;
    }

    outNodes.reserve(bounds.size() * 2);
    outOrder.reserve(bounds.size());

    std::vector<uint32_t> indices(bounds.size());
    std::iota(indices.begin(), indices.end(), 0u);

    detail::BuildRecursive(indices, 0, static_cast<int>(indices.size()), bounds, outNodes, outOrder);
}

#endif // BVH_H
