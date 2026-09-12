#ifndef MESH_H
#define MESH_H

#include <vector>
#include "math/vec3.h"
#include "core/common.h"
#include "scene/material.h"
#include "scene/gpu_types.h"
#include "scene/bvh.h"

struct Vertex
{
    float x, y, z;    // Position
    float r, g, b;    // Color
    float nx, ny, nz; // Normal
    float u, v;       // Texture coordinates
};


class Mesh
{
public:
    Mesh() = default;
    Mesh(const std::vector<Vertex> &vertices, const std::vector<uint16_t> &indices, const AABB &bounds);

    /*
    Creates a box mesh
    @param halfExtent The half extent of the box in each axis
    @param material The material to apply to the box
    @return A Mesh object representing the box
    */
    static Mesh CreateBox(const Vec3<float> &halfExtent, const Material &material, const AABB &bounds);

    static Mesh CreateSphere(float radius, const uint16_t segments, uint16_t rings, const Material &material);

    /*
    Creates a flat parallelogram spanning the edge vectors u and v, centred on
    its own middle - the mesh twin of an analytic quad. Register it with
    MeshLibrary::Add(mesh, true): a quad has two visible sides.
    */
    static Mesh CreateQuad(const Vec3<float> &u, const Vec3<float> &v, const Material &material);

    // Data Accessors and Modifiers

    /*
    Set the vertex data for the mesh
    @param verticies Vector of verticies
    @param bounds The axis-aligned bounding box of the mesh
    */
    void SetVertices(const std::vector<Vertex> &vertices, const AABB &bounds);

    /*
    Set the index data for the mesh
    @param indices Vector of indices
    */
    void SetIndices(const std::vector<uint16_t> &indices);

    const std::vector<Vertex> &GetVertices() const { return vertices; }
    const std::vector<uint16_t> &GetIndices() const { return indices; }

    /*
    Get the number of vertices in the mesh
    @return The number of vertices
    */
    uint32_t GetVertexCount() const { return vertices.size(); }

    /*
    Get the number of indices in the mesh
    @return The number of indices
    */
    uint32_t GetIndexCount() const { return indices.size(); }

    /*
    Clear the mesh data
    */
    void Clear();

    bool IsValid() const { return !vertices.empty() && !indices.empty(); }

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
};

inline Mesh::Mesh(const std::vector<Vertex> &verts, const std::vector<uint16_t> &inds, const AABB &bounds)
{
    SetVertices(verts, bounds);
    SetIndices(inds);
}

inline void Mesh::SetVertices(const std::vector<Vertex> &verts, const AABB &bounds)
{
    vertices.clear();
    if (verts.empty())
        return;

    float cx = (bounds.min_x + bounds.max_x) * 0.5f;
    float cy = (bounds.min_y + bounds.max_y) * 0.5f;
    float cz = (bounds.min_z + bounds.max_z) * 0.5f;

    vertices.reserve(verts.size());
    for (const auto &v : verts)
    {
        Vertex vv = v;
        vv.x -= cx;
        vv.y -= cy;
        vv.z -= cz;
        vertices.push_back(vv);
    }
}

inline void Mesh::SetIndices(const std::vector<uint16_t> &inds)
{
    indices = inds;
}

inline void Mesh::Clear()
{
    vertices.clear();
    indices.clear();
}

inline Mesh Mesh::CreateBox(const Vec3<float> &halfExtent, const Material &material, const AABB &bounds)
{
    std::vector<Vertex> verts = {
        // Front face (+Z) - normal (0, 0, 1)
        {-halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        {-halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},

        // Back face (-Z) - normal (0, 0, -1)
        {halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f},
        {-halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f},
        {-halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f},
        {halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f},

        // Left face (-X) - normal (-1, 0, 0)
        {-halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {-halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {-halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        {-halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f},

        // Right face (+X) - normal (1, 0, 0)
        {halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        {halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},

        // Top face (+Y) - normal (0, 1, 0)
        {-halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {halfExtent.x, halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
        {-halfExtent.x, halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},

        // Bottom face (-Y) - normal (0, -1, 0)
        {-halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f},
        {halfExtent.x, -halfExtent.y, -halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f},
        {halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f},
        {-halfExtent.x, -halfExtent.y, halfExtent.z, material.albedo.x, material.albedo.y, material.albedo.z, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f}

    };

    std::vector<uint16_t> inds = {
        0, 1, 2, 0, 2, 3,       // Front face
        4, 5, 6, 4, 6, 7,       // Back face
        8, 9, 10, 8, 10, 11,    // Left face
        12, 13, 14, 12, 14, 15, // Right face
        16, 17, 18, 16, 18, 19, // Top face
        20, 21, 22, 20, 22, 23  // Bottom face
    };

    return Mesh(verts, inds, bounds);
}

inline Mesh Mesh::CreateSphere(float radius, const uint16_t segments, uint16_t rings, const Material &material)
{
    std::vector<Vertex> verts;
    std::vector<uint16_t> inds;

    for (uint16_t ring = 0; ring <= rings; ++ring)
    {
        float phi = 3.1415926535f * static_cast<float>(ring) / static_cast<float>(rings); // from 0 to 2PI
        float sinPath = std::sin(phi);
        float cosPath = std::cos(phi);

        for (uint16_t seg = 0; seg <= segments; ++seg)
        {
            float theta = 2.0f * 3.1415926535f * static_cast<float>(seg) / static_cast<float>(segments); // from 0 to PI
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            float x = radius * sinPath * cosTheta;
            float y = radius * cosPath;
            float z = radius * sinPath * sinTheta;

            float nx = sinPath * cosTheta;
            float ny = cosPath;
            float nz = sinPath * sinTheta;

    

            // Texture coordinates (u, v)
            float u = static_cast<float>(seg) / static_cast<float>(segments);
            float v = static_cast<float>(ring) / static_cast<float>(rings);

            verts.push_back({x, y, z, material.albedo.x, material.albedo.y, material.albedo.z, nx, ny, nz, u, v});
        }
    }

    for (uint16_t ring = 0; ring < rings; ++ring)
    {
        for (uint16_t seg = 0; seg < segments; ++seg)
        {
            uint16_t a = ring * (segments + 1) + seg;
            uint16_t b = a + 1;
            uint16_t c = (ring + 1) * (segments + 1) + seg;
            uint16_t d = c + 1;

            // Winding matters: cross(v1 - v0, v2 - v0) is the geometric normal,
            // and the ray tracer uses its sign to decide front face from back.
            // The order used to be (a, c, b) / (b, c, d), which at the equator
            // gives edges pointing south and east - and cross(south, east)
            // points INTO the sphere. Every hit then read as a back face, the
            // shading normal was flipped inward, and the sphere rendered
            // unlit. CreateBox above was always wound correctly, which is why
            // only spheres showed it.
            //
            // Counter-clockwise seen from outside, matching CreateBox.
            inds.push_back(a);
            inds.push_back(b);
            inds.push_back(c);

            inds.push_back(b);
            inds.push_back(d);
            inds.push_back(c);
            }
    }

    return Mesh(verts, inds, AABB{-radius, -radius, -radius, radius, radius, radius});
}

// Corners are authored at 0, u, u + v and v. Their bounding box is centred on
// (u + v) / 2 - a parallelogram is symmetric about its middle - so SetVertices
// re-centres the vertices there, and an instance of this mesh belongs at
// corner + (u + v) / 2. UVs run along u and v exactly like HitQuad's
// (alpha, beta), and the winding is counter-clockwise seen from the side
// cross(u, v) points to.
inline Mesh Mesh::CreateQuad(const Vec3<float> &u, const Vec3<float> &v, const Material &material)
{
    const Vec3<float> n = normalize(cross(u, v));
    const Vec3<float> corners[4] = {Vec3<float>(0.0f, 0.0f, 0.0f), u, u + v, v};
    const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};

    std::vector<Vertex> verts;
    verts.reserve(4);
    AABB bounds = AABB::Empty();
    for (int i = 0; i < 4; i++)
    {
        const Vec3<float> &p = corners[i];
        verts.push_back({p.x, p.y, p.z,
                         material.albedo.x, material.albedo.y, material.albedo.z,
                         n.x, n.y, n.z,
                         uvs[i][0], uvs[i][1]});
        bounds = SurroundPoint(bounds, p.x, p.y, p.z);
    }

    return Mesh(verts, {0, 1, 2, 0, 2, 3}, bounds);
}

#endif // MESH_H
