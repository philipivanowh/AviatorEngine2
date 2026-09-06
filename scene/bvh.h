#ifndef BVH_H
#define BVH_H

#include <SDL3/SDL.h>
#include <algorithm>
#include <numeric>
#include <vector>

#include "aabb.h"

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
        const std::vector<std::unique_ptr<Object>> &objects,
        const std::vector<AABB> &bounds,
        std::vector<BVH_node *> &outNodes,
        std::vector<Object *> &outObjects)
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
            const int first = static_cast<int>(outObjects.size());
            for (int i = start; i < end; i++)
            {
                outObjects.push_back(objects[indices[i]].get());
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

        const int left = BuildRecursive(indices, start, mid, objects, bounds, outNodes, outObjects);
        const int right = BuildRecursive(indices, mid, end, objects, bounds, outNodes, outObjects);

        outNodes[nodeIndex] = new BVH_node(box, left, right, 0);

        return nodeIndex;
    }
}

inline void BuildBVH(
    const std::vector<std::unique_ptr<Object>> &objects,
    std::vector<BVH_node *> &outNodes,
    std::vector<Object *> &outObjects)
{
    for (BVH_node *node : outNodes)
    {
        delete node;
    }
    outNodes.clear();
    outObjects.clear();

    if (objects.empty())
    {
        return;
    }

    outNodes.reserve(objects.size() * 2);
    outObjects.reserve(objects.size());

    std::vector<AABB> bounds(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
    {
        bounds[i] = objects[i]->Bounds();
    }

    std::vector<int> indices(objects.size());
    std::iota(indices.begin(), indices.end(), 0);

    detail::BuildRecursive(indices, 0, static_cast<int>(indices.size()), objects, bounds, outNodes, outObjects);
}

#endif // BVH_H
