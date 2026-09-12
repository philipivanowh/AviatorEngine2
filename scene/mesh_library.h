#ifndef MESH_LIBRARY_H
#define MESH_LIBRARY_H

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "scene/aabb.h"
#include "scene/bvh.h"
#include "scene/mesh.h"

// Owns every mesh in a scene and flattens them into the handful of flat arrays
// the shader reads. Modelled on TextureLibrary: handles ARE the index, they are
// handed out at registration, and there is no separate "upload the meshes" pass
// to forget.
//
// The central idea of the two-level structure lives here. Geometry is stored
// ONCE, in local space, and an instance is just a transform plus four offsets
// into these arrays. A sphere that costs 2048 triangles as standalone geometry
// costs one 48-byte instance record once it is a mesh instance, and moving it
// rebuilds a BVH over instances rather than over triangles.
//
// Everything is concatenated into single buffers because a compute shader
// cannot index an array of buffers - it needs one big StructuredBuffer plus a
// base offset per instance.

using MeshHandle = uint32_t;
inline constexpr MeshHandle kNoMesh = 0xFFFFFFFFu;

// 32 bytes. Scalars, NOT float3 - the same StructuredBuffer alignment rule that
// forced pad0/pad1/pad2 into Object_GPU. A float3 here would be bumped to a
// 16-byte boundary and desync the stride from the HLSL Vertex struct.
//
// Note there is no per-vertex colour. Mesh::Vertex carries r,g,b copied from
// the material, which duplicates the same three floats across every vertex of
// every mesh; colour belongs to the material, and dropping it here takes the
// vertex from 44 bytes to 32.
struct Vertex_GPU
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

static_assert(sizeof(Vertex_GPU) == 32, "Vertex_GPU must match the HLSL Vertex stride");

// Where one mesh lives inside the library's flat arrays. All bases are in
// ELEMENTS, not bytes - the shader indexes StructuredBuffers, which are
// element-addressed.
struct MeshRange
{
    uint32_t vertexBase = 0;
    uint32_t indexBase = 0; // == first triangle * 3
    uint32_t triangleCount = 0;
    uint32_t blasBase = 0;
    AABB localBounds = AABB::Empty();

    // Both faces visible, so the rasterizer must not cull it. True for open
    // surfaces such as quads; closed meshes get back-face culling for free.
    bool doubleSided = false;
};

class MeshLibrary
{
public:
    // Appends a mesh and builds its BLAS. Returns the handle, which is the
    // index into `ranges` and stays valid for the life of the library.
    MeshHandle Add(const Mesh &mesh, bool doubleSided = false);

    const MeshRange &Range(MeshHandle handle) const
    {
        SDL_assert(handle < ranges.size());
        return ranges[handle];
    }

    size_t Count() const { return ranges.size(); }
    bool Empty() const { return ranges.empty(); }

    const std::vector<Vertex_GPU> &Vertices() const { return vertices; }
    const std::vector<uint32_t> &Indices() const { return indices; }
    const std::vector<BVHNode_GPU> &BLASNodes() const { return blas; }

private:
    std::vector<Vertex_GPU> vertices;
    std::vector<uint32_t> indices;
    std::vector<BVHNode_GPU> blas;
    std::vector<MeshRange> ranges;
};

inline MeshHandle MeshLibrary::Add(const Mesh &mesh, bool doubleSided)
{
    MeshRange range = {};
    range.vertexBase = static_cast<uint32_t>(vertices.size());
    range.indexBase = static_cast<uint32_t>(indices.size());
    range.blasBase = static_cast<uint32_t>(blas.size());
    range.doubleSided = doubleSided;

    // 1. Vertices, verbatim and LOCAL. They stay local because that is the
    //    whole point of instancing: the same geometry is reused by every
    //    instance, so nothing appended here may depend on where an instance
    //    happens to sit in the world.
    const std::vector<Vertex> &src = mesh.GetVertices();
    AABB local = AABB::Empty();

    vertices.reserve(vertices.size() + src.size());
    for (const Vertex &v : src)
    {
        vertices.push_back(Vertex_GPU{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v});
        local = SurroundPoint(local, v.x, v.y, v.z);
    }
    range.localBounds = local;

    // 2. One AABB per triangle, in local space - all the builder needs.
    const std::vector<uint16_t> &srcIndices = mesh.GetIndices();
    SDL_assert(srcIndices.size() % 3 == 0 && "index buffer is not a triangle list");

    const uint32_t triangleCount = static_cast<uint32_t>(srcIndices.size() / 3);
    range.triangleCount = triangleCount;

    std::vector<AABB> triangleBounds;
    triangleBounds.reserve(triangleCount);

    for (uint32_t t = 0; t < triangleCount; t++)
    {
        const Vertex &a = src[srcIndices[t * 3 + 0]];
        const Vertex &b = src[srcIndices[t * 3 + 1]];
        const Vertex &c = src[srcIndices[t * 3 + 2]];

        const AABB edge = Surround2Points(Point3(a.x, a.y, a.z), Point3(b.x, b.y, b.z));
        triangleBounds.push_back(SurroundPoint(edge, c.x, c.y, c.z));
    }

    // 3. Build the BLAS. Node indices and leaf primitive slots come back
    //    MESH-RELATIVE, deliberately: the shader adds blasBase and indexBase,
    //    so a mesh can be appended anywhere in the global arrays without
    //    rewriting a single node.
    std::vector<BVHNode_GPU> nodes;
    std::vector<uint32_t> order;
    BuildBVH(triangleBounds, nodes, order);

    // 4. The step that is easy to miss: the builder REORDERED the triangles,
    //    and a leaf addresses them by their new slot. The index buffer has to
    //    be appended in that order or every leaf points at the wrong triangle -
    //    which renders as geometry that is *almost* right, the worst kind of
    //    bug to chase.
    indices.reserve(indices.size() + srcIndices.size());
    for (uint32_t slot : order)
    {
        indices.push_back(srcIndices[slot * 3 + 0]);
        indices.push_back(srcIndices[slot * 3 + 1]);
        indices.push_back(srcIndices[slot * 3 + 2]);
    }

    blas.insert(blas.end(), nodes.begin(), nodes.end());

    ranges.push_back(range);
    return static_cast<MeshHandle>(ranges.size() - 1);
}

#endif // MESH_LIBRARY_H
