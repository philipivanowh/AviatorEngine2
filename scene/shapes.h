#ifndef SHAPES_H
#define SHAPES_H

#include <cmath>
#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "scene/aabb.h"
#include "scene/components.h"
#include "scene/gpu_types.h"
#include "math/quat.hpp"
#include "math/vec3.h"


// Returns true when q is a rotation about Y alone, i.e. when squeezing it into
// one float loses nothing.
inline bool YawOnly(const Quat<float> &q, float tolerance = 1e-4f)
{
    return std::fabs(q.x) < tolerance && std::fabs(q.z) < tolerance;
}

// ---------------------------------------------------------------------------
// Transform helpers
// ---------------------------------------------------------------------------

// Rotate a transform by a world-space rotation about an arbitrary pivot. The
// position swings around the pivot; the orientation composes on the left,
// because q is expressed in world space and the existing rotation in the
// object's own.
//
// This replaces Object::Rotate plus the RotateLocal virtual. The old version
// had to ask each subclass to rotate whatever else defined its shape; here the
// shape data never moves - only the transform does.
inline void RotateTransform(TransformComponent &transform, const Quat<float> &q, const Point3 &pivot)
{
    transform.position = pivot + rotate(q, transform.position - pivot);
    transform.rotation = normalize(q * transform.rotation);
}

// ---------------------------------------------------------------------------
// Per-shape geometry
// ---------------------------------------------------------------------------

// The point a pivot-free rotation turns about. For a sphere or box that is the
// transform's position; a quad is anchored at a corner, so its centre is half
// a diagonal away.

inline Point3 SphereCenter(const TransformComponent &t, const SphereComponent &) { return t.position; }
inline Point3 BoxCenter(const TransformComponent &t, const BoxComponent &) { return t.position; }

inline Point3 QuadCenter(const TransformComponent &t, const QuadComponent &q)
{
    return t.position + 0.5f * (rotate(t.rotation, q.u) + rotate(t.rotation, q.v));
}

// World-space bounds. These are what the BVH is built over, so they have to
// account for the transform - an object whose bounds ignore its rotation gets
// missed by rays that should hit it.

inline AABB SphereBounds(const TransformComponent &t, const SphereComponent &s)
{
    // Rotation is irrelevant: a sphere is rotationally symmetric.
    const float r = s.radius;
    return AABB{t.position.x - r, t.position.y - r, t.position.z - r,
                t.position.x + r, t.position.y + r, t.position.z + r};
}

inline AABB QuadBounds(const TransformComponent &t, const QuadComponent &q)
{
    const Vec3<float> u = rotate(t.rotation, q.u);
    const Vec3<float> v = rotate(t.rotation, q.v);

    // The two diagonals between them touch all four corners.
    const AABB d1 = Surround2Points(t.position, t.position + u + v);
    const AABB d2 = Surround2Points(t.position + u, t.position + v);
    return Surround(d1, d2);
}

inline AABB BoxBounds(const TransformComponent &t, const BoxComponent &b)
{
    // World AABB of the yaw-rotated box: rotating the half-extent vector and
    // taking absolute values gives the extent along each world axis.
    const float yaw = YawOf(t.rotation);
    const float s = std::fabs(std::sin(yaw));
    const float c = std::fabs(std::cos(yaw));
    const float ex = c * b.halfExtent.x + s * b.halfExtent.z;
    const float ez = s * b.halfExtent.x + c * b.halfExtent.z;
    return AABB{t.position.x - ex, t.position.y - b.halfExtent.y, t.position.z - ez,
                t.position.x + ex, t.position.y + b.halfExtent.y, t.position.z + ez};
}

// ---------------------------------------------------------------------------
// GPU packing
// ---------------------------------------------------------------------------

// Everything that does not depend on the shape: position and the whole
// material block. The old Object::BaseObjectGPU, minus the inheritance.
inline Object_GPU BaseGPU(const TransformComponent &t, const Material &mat, BodyShape shape)
{
    Object_GPU o = {};

    o.x = t.position.x;
    o.y = t.position.y;
    o.z = t.position.z;

    // Position2 is the shutter-close / previous-frame centre used for motion
    // blur. Static for now, so it matches Position. Once physics runs, this is
    // where last frame's position goes.
    o.x2 = t.position.x;
    o.y2 = t.position.y;
    o.z2 = t.position.z;

    o.r = mat.albedo.x;
    o.g = mat.albedo.y;
    o.b = mat.albedo.z;

    o.fuzz = mat.fuzz;
    o.refraction = mat.ior;
    o.emission = mat.emission;
    o.density = mat.density;

    o.shapeType = static_cast<Uint32>(shape);
    o.colorType = static_cast<Uint32>(mat.type);
    o.textureID = mat.TextureId();

    return o;
}

inline Object_GPU SphereToGPU(const TransformComponent &t, const SphereComponent &s, const Material &mat)
{
    Object_GPU o = BaseGPU(t, mat, BodyShape::Sphere);
    o.radius = s.radius;
    // Only the texture turns when a sphere rotates, and the shader's UV
    // parameterisation is built around Y.
    o.rotation = YawOf(t.rotation);
    return o;
}

inline Object_GPU QuadToGPU(const TransformComponent &t, const QuadComponent &q, const Material &mat)
{
    Object_GPU o = BaseGPU(t, mat, BodyShape::Quad);

    // The rotation is applied here rather than stored: the shader's UVs are the
    // quad's own (alpha, beta) coordinates along u and v, so rotating the edge
    // vectors carries the texture along for free and costs the shader nothing
    // per ray. The authored u/v in the component are untouched, so this stays
    // exact no matter how many times physics has spun the transform.
    const Vec3<float> u = rotate(t.rotation, q.u);
    const Vec3<float> v = rotate(t.rotation, q.v);

    o.u_x = u.x;
    o.u_y = u.y;
    o.u_z = u.z;

    o.v_x = v.x;
    o.v_y = v.y;
    o.v_z = v.z;

    return o;
}

inline Object_GPU BoxToGPU(const TransformComponent &t, const BoxComponent &b, const Material &mat)
{
    Object_GPU o = BaseGPU(t, mat, BodyShape::Box);
    o.half_x = b.halfExtent.x;
    o.half_y = b.halfExtent.y;
    o.half_z = b.halfExtent.z;

    // A centre + half-extents + yaw can only express rotation about Y. X/Z
    // would need a full OBB (three orthonormal axes instead of one angle) in
    // both this struct and HitBox - fail loudly rather than silently rendering
    // the box unrotated.
    SDL_assert(YawOnly(t.rotation) &&
               "Box only supports rotation about Y; use a quad shell for other axes");
    o.rotation = YawOf(t.rotation);
    return o;
}

// ---------------------------------------------------------------------------
// Entity dispatchers
// ---------------------------------------------------------------------------

// Which shape component an entity carries is the dispatch. try_get returns
// nullptr for the ones it does not have, so this is three pool lookups against
// one vtable hop - and unlike the vtable it degrades gracefully: an entity with
// a transform but no shape is simply not renderable, rather than impossible to
// express.

inline bool IsRenderable(const entt::registry &registry, entt::entity e)
{
    return registry.all_of<TransformComponent, MaterialComponent>(e) &&
           registry.any_of<SphereComponent, QuadComponent, BoxComponent>(e);
}

inline AABB EntityBounds(const entt::registry &registry, entt::entity e)
{
    const auto &t = registry.get<TransformComponent>(e);

    if (const auto *s = registry.try_get<SphereComponent>(e))
        return SphereBounds(t, *s);
    if (const auto *q = registry.try_get<QuadComponent>(e))
        return QuadBounds(t, *q);
    if (const auto *b = registry.try_get<BoxComponent>(e))
        return BoxBounds(t, *b);

    return AABB::Empty();
}

inline Point3 EntityCenter(const entt::registry &registry, entt::entity e)
{
    const auto &t = registry.get<TransformComponent>(e);

    if (const auto *s = registry.try_get<SphereComponent>(e))
        return SphereCenter(t, *s);
    if (const auto *q = registry.try_get<QuadComponent>(e))
        return QuadCenter(t, *q);
    if (const auto *b = registry.try_get<BoxComponent>(e))
        return BoxCenter(t, *b);

    return t.position;
}

inline Object_GPU EntityToGPU(const entt::registry &registry, entt::entity e)
{
    const auto &t = registry.get<TransformComponent>(e);
    const auto &mat = registry.get<MaterialComponent>(e).material;

    if (const auto *s = registry.try_get<SphereComponent>(e))
        return SphereToGPU(t, *s, mat);
    if (const auto *q = registry.try_get<QuadComponent>(e))
        return QuadToGPU(t, *q, mat);
    if (const auto *b = registry.try_get<BoxComponent>(e))
        return BoxToGPU(t, *b, mat);

    return Object_GPU{};
}

// ---------------------------------------------------------------------------
// Frame gather
// ---------------------------------------------------------------------------

// Collect everything the BVH should be built over, with its bounds. One pass,
// no per-object allocation, no clone - this is the whole of what
// Scene::GetObjects() used to do with a vector of unique_ptr and a virtual
// Clone() per shape.
//
// The three views are walked separately so each one is a tight iteration over
// entities that all have the same components, which is the layout entt is
// actually fast at. Order is stable for a given registry, and the BVH reorders
// anyway.
inline void GatherRenderables(const entt::registry &registry,
                              std::vector<entt::entity> &outEntities,
                              std::vector<AABB> &outBounds)
{
    outEntities.clear();
    outBounds.clear();

    {
        auto view = registry.view<TransformComponent, MaterialComponent, SphereComponent>();
        for (auto e : view)
        {
            outEntities.push_back(e);
            outBounds.push_back(SphereBounds(view.get<TransformComponent>(e), view.get<SphereComponent>(e)));
        }
    }
    {
        auto view = registry.view<TransformComponent, MaterialComponent, QuadComponent>();
        for (auto e : view)
        {
            outEntities.push_back(e);
            outBounds.push_back(QuadBounds(view.get<TransformComponent>(e), view.get<QuadComponent>(e)));
        }
    }
    {
        auto view = registry.view<TransformComponent, MaterialComponent, BoxComponent>();
        for (auto e : view)
        {
            outEntities.push_back(e);
            outBounds.push_back(BoxBounds(view.get<TransformComponent>(e), view.get<BoxComponent>(e)));
        }
    }
}

#endif // SHAPES_H
