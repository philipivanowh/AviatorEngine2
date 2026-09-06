#ifndef BVH_H
#define BVH_H

#include <SDL3/SDL.h>
#include <algorithm>
#include <numeric>
#include <vector>

#include <entt/entt.hpp>

#include "scene/aabb.h"

#define BVH_LEAF_SIZE 4

// The BVH knows nothing about shapes any more. It is handed a flat list of
// entities and their world-space bounds, and it hands back a reordered entity
// list plus the node tree over it. That removes the pointer chase the old
// version did through unique_ptr<Object> to call a virtual Bounds(), and means
// adding a shape type never touches this file.
//
// Bounds are passed in rather than computed here because the caller already
// walks the registry to gather them (see GatherRenderables in shapes.h), and
// doing it in one pass keeps the component data in cache.

// Mirrors BVHNode in path_trace.comp.hlsl byte-for-byte. Two float3s (Min,
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

class BVH_node
{

public:
    AABB bbox;
    int left;
    int right;
    int count;

    BVH_node(AABB bbox, int left, int right, int count) : bbox(bbox), left(left), right(right), count(count)
    {
    }

    BVHNode_GPU CreateBVHNodeGPU()
    {
        BVHNode_GPU bvh_node = {};
        bvh_node.min_x = this->bbox.min_x;
        bvh_node.min_y = this->bbox.min_y;
        bvh_node.min_z = this->bbox.min_z;

        bvh_node.max_x = this->bbox.max_x;
        bvh_node.max_y = this->bbox.max_y;
        bvh_node.max_z = this->bbox.max_z;

        bvh_node.left = this->left;
        bvh_node.right = this->right;
        bvh_node.count = this->count;
        return bvh_node;
    }
};

namespace detail
{
    inline int BuildRecursive(
        std::vector<int> &indices,
        int start,
        int end,
        const std::vector<entt::entity> &entities,
        const std::vector<AABB> &bounds,
        std::vector<BVH_node *> &outNodes,
        std::vector<entt::entity> &outEntities)
    {
        AABB box = AABB::Empty();
        for (int i = start; i < end; i++)
        {
            box = Surround(box, bounds[indices[i]]);
        }

        // Reserve this node before descending so its index remains stable
        // while recursive calls add its children.
        const int nodeIndex = static_cast<int>(outNodes.size());
        outNodes.push_back(nullptr);

        const int count = end - start;
        if (count <= BVH_LEAF_SIZE)
        {
            const int first = static_cast<int>(outEntities.size());
            for (int i = start; i < end; i++)
            {
                outEntities.push_back(entities[indices[i]]);
            }
            outNodes[nodeIndex] = new BVH_node(box, first, -1, count);

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

        const int left = BuildRecursive(indices, start, mid, entities, bounds, outNodes, outEntities);
        const int right = BuildRecursive(indices, mid, end, entities, bounds, outNodes, outEntities);

        outNodes[nodeIndex] = new BVH_node(box, left, right, 0);

        return nodeIndex;
    }
}

// `entities` and `bounds` are parallel arrays - bounds[i] is the world-space
// AABB of entities[i]. GatherRenderables() produces both.
//
// outEntities comes back in leaf order, which is the order they must be
// uploaded to the GPU in: BVH leaves address the object buffer by index, so
// the shader's `objects[i]` has to be the same object this build put at i.
inline void BuildBVH(
    const std::vector<entt::entity> &entities,
    const std::vector<AABB> &bounds,
    std::vector<BVH_node *> &outNodes,
    std::vector<entt::entity> &outEntities)
{
    SDL_assert(entities.size() == bounds.size());

    for (BVH_node *node : outNodes)
    {
        delete node;
    }
    outNodes.clear();
    outEntities.clear();

    if (entities.empty())
    {
        return;
    }

    outNodes.reserve(entities.size() * 2);
    outEntities.reserve(entities.size());

    std::vector<int> indices(entities.size());
    std::iota(indices.begin(), indices.end(), 0);

    detail::BuildRecursive(indices, 0, static_cast<int>(indices.size()), entities, bounds, outNodes, outEntities);
}

#endif // BVH_H
