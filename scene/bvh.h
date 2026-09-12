#ifndef BVH_H
#define BVH_H

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
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

    // Buckets per axis for the binned surface area heuristic. 12 is the usual
    // choice: more buys almost no tree quality, and build time is linear in it.
    constexpr int kSahBins = 12;

    // Deepest the builder will go. The shaders walk the tree with fixed-size
    // stacks - BVH_STACK_SIZE (32) at the top level, BLAS_STACK_SIZE (24) inside
    // a mesh - and a walk needs at most depth + 1 slots. A median split could
    // never get near this; SAH can, on clustered geometry, and running off the
    // end of a stack in HLSL is an out-of-bounds write, not an error message.
    // So at this depth a node becomes a leaf however many primitives it holds.
    constexpr int kMaxDepth = 20;

    // Half a box's surface area. The factor of two cancels out of every SAH
    // comparison, so it is never computed.
    inline float HalfArea(const AABB &b)
    {
        const float dx = b.max_x - b.min_x;
        const float dy = b.max_y - b.min_y;
        const float dz = b.max_z - b.min_z;
        return dx * dy + dy * dz + dz * dx;
    }

    // Which SAH bucket a centroid falls in. Binning and partitioning both call
    // this, so the two can never disagree about which side a primitive is on.
    inline int SahBin(float centroid, float centroidMin, float scale)
    {
        return std::min(kSahBins - 1, static_cast<int>((centroid - centroidMin) * scale));
    }

    inline int BuildRecursive(
        std::vector<uint32_t> &indices,
        int start,
        int end,
        const std::vector<AABB> &bounds,
        std::vector<BVHNode_GPU> &outNodes,
        std::vector<uint32_t> &outOrder,
        int depth)
    {
        AABB box = AABB::Empty();
        float centroidMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float centroidMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (int i = start; i < end; i++)
        {
            const AABB &b = bounds[indices[i]];
            box = Surround(box, b);
            for (int axis = 0; axis < 3; axis++)
            {
                const float c = b.Centroid(axis);
                centroidMin[axis] = std::min(centroidMin[axis], c);
                centroidMax[axis] = std::max(centroidMax[axis], c);
            }
        }

        // Reserve this node before descending so its index remains stable
        // while recursive calls add its children.
        const int nodeIndex = static_cast<int>(outNodes.size());
        outNodes.push_back(BVHNode_GPU{});

        const int count = end - start;
        if (count <= BVH_LEAF_SIZE || depth >= kMaxDepth)
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

        // Binned SAH. The cost of a split is roughly how many primitive tests a
        // ray entering this node will pay: each child's primitive count, weighted
        // by the chance of a ray hitting that child, which is proportional to its
        // surface area. A median split ignores area entirely, so on uneven
        // geometry - a dense cluster next to a wide flat ground - it produces
        // big, mostly empty child boxes that every ray has to open.
        //
        // Nodes above BVH_LEAF_SIZE always split (no "leaf is cheaper" early
        // out), which keeps leaves small and the leaf loops in the shaders short.
        int bestAxis = -1;
        int bestSplit = 0; // first bucket on the right side
        float bestCost = FLT_MAX;

        for (int axis = 0; axis < 3; axis++)
        {
            const float extent = centroidMax[axis] - centroidMin[axis];
            if (extent <= 0.0f)
            {
                continue; // every centroid in one plane: no plane on this axis separates them
            }
            const float scale = static_cast<float>(kSahBins) / extent;

            AABB binBox[kSahBins];
            int binCount[kSahBins] = {};
            for (int b = 0; b < kSahBins; b++)
            {
                binBox[b] = AABB::Empty();
            }
            for (int i = start; i < end; i++)
            {
                const AABB &primitive = bounds[indices[i]];
                const int b = SahBin(primitive.Centroid(axis), centroidMin[axis], scale);
                binBox[b] = Surround(binBox[b], primitive);
                binCount[b]++;
            }

            // Sweep left to right recording everything left of each plane, then
            // right to left pricing each plane with what is right of it.
            float leftCost[kSahBins - 1];
            int leftCount[kSahBins - 1];
            AABB accumulated = AABB::Empty();
            int accumulatedCount = 0;
            for (int s = 0; s < kSahBins - 1; s++)
            {
                accumulated = Surround(accumulated, binBox[s]);
                accumulatedCount += binCount[s];
                leftCount[s] = accumulatedCount;
                leftCost[s] = accumulatedCount > 0 ? HalfArea(accumulated) * accumulatedCount : 0.0f;
            }

            accumulated = AABB::Empty();
            accumulatedCount = 0;
            for (int s = kSahBins - 1; s > 0; s--)
            {
                accumulated = Surround(accumulated, binBox[s]);
                accumulatedCount += binCount[s];

                // Plane between bucket s-1 and s. Both sides must be non-empty,
                // or the "split" just recreates this node one level down.
                if (leftCount[s - 1] == 0 || accumulatedCount == 0)
                {
                    continue;
                }
                const float cost = leftCost[s - 1] + HalfArea(accumulated) * accumulatedCount;
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = s;
                }
            }
        }

        int mid = start + count / 2;
        if (bestAxis >= 0)
        {
            const float scale = static_cast<float>(kSahBins) / (centroidMax[bestAxis] - centroidMin[bestAxis]);
            const auto middle = std::partition(
                indices.begin() + start,
                indices.begin() + end,
                [&](uint32_t index)
                {
                    return SahBin(bounds[index].Centroid(bestAxis), centroidMin[bestAxis], scale) < bestSplit;
                });
            mid = static_cast<int>(middle - indices.begin());
        }

        // No plane separated anything - every centroid coincides, e.g. identical
        // boxes stacked on one spot - or, defensively, the partition came back
        // one-sided. Halving the list in any order still terminates the build.
        if (mid <= start || mid >= end)
        {
            mid = start + count / 2;
        }

        const int left = BuildRecursive(indices, start, mid, bounds, outNodes, outOrder, depth + 1);
        const int right = BuildRecursive(indices, mid, end, bounds, outNodes, outOrder, depth + 1);

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

    detail::BuildRecursive(indices, 0, static_cast<int>(indices.size()), bounds, outNodes, outOrder, 0);
}

#endif // BVH_H
