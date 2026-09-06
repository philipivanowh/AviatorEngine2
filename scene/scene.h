#ifndef SCENE_H
#define SCENE_H

#include <vector>

#include <SDL3/SDL.h>
#include <entt/entt.hpp>

#include "scene/aabb.h"
#include "scene/components.h"
#include "scene/material.h"
#include "scene/shapes.h"
#include "scene/texture.h"

// A handle to the entities one Add* call created, so a multi-part shape can be
// transformed as a single rigid thing. This matters at authoring time: a box
// shell is six quads, and rotating each of them about its own centre would
// tear the box apart. The group rotates them all about one shared pivot
// instead.
//
//     scene.AddBoxShell(reg, a, b, white).RotateY(degrees_to_radians(15.0f))
//                                        .Translate(Vec3(265.0f, 0.0f, 295.0f));
//
// Holds entity handles rather than indices into a scene-owned vector. Handles
// are generational, so they stay valid as entities are added *and* destroyed,
// which an index range never was.
//
// Scope note: this is an authoring-time builder, applied once and discarded.
// It is deliberately NOT the runtime grouping mechanism - once physics is
// running, "these six quads are one rigid body" has to be recorded in the
// registry where a solver can see it every step. That is what
// ShapeOfComponent is for.
class EntityGroup
{
public:
    EntityGroup(entt::registry &registry, std::vector<entt::entity> entities)
        : registry(&registry), entities(std::move(entities))
    {
    }

    EntityGroup(entt::registry &registry, entt::entity entity)
        : registry(&registry), entities{entity}
    {
    }

    EntityGroup &Translate(const Vec3<float> &displacement)
    {
        for (entt::entity e : entities)
            registry->get<TransformComponent>(e).position += displacement;
        return *this;
    }

    // Rotate about an explicit pivot.
    EntityGroup &Rotate(const Quat<float> &q, const Point3 &pivot)
    {
        for (entt::entity e : entities)
            RotateTransform(registry->get<TransformComponent>(e), q, pivot);
        return *this;
    }

    // Rotate about the group's own centre. Center() has to be evaluated before
    // anything moves, otherwise each entity would be rotated about a pivot the
    // previous one had already shifted.
    EntityGroup &Rotate(const Quat<float> &q) { return Rotate(q, Center()); }

    EntityGroup &RotateX(float radians) { return Rotate(QuatX(radians)); }
    EntityGroup &RotateY(float radians) { return Rotate(QuatY(radians)); }
    EntityGroup &RotateZ(float radians) { return Rotate(QuatZ(radians)); }

    EntityGroup &RotateX(float radians, const Point3 &pivot) { return Rotate(QuatX(radians), pivot); }
    EntityGroup &RotateY(float radians, const Point3 &pivot) { return Rotate(QuatY(radians), pivot); }
    EntityGroup &RotateZ(float radians, const Point3 &pivot) { return Rotate(QuatZ(radians), pivot); }

    // Snapshot the current pose as the one physics should reset to. Call it
    // after the transforms are final, i.e. at the end of a builder chain.
    EntityGroup &MarkRestPose()
    {
        for (entt::entity e : entities)
        {
            const auto &t = registry->get<TransformComponent>(e);
            registry->emplace_or_replace<RestTransformComponent>(e, t.position, t.rotation);
        }
        return *this;
    }

    AABB Bounds() const
    {
        AABB bounds = AABB::Empty();
        for (entt::entity e : entities)
            bounds = Surround(bounds, EntityBounds(*registry, e));
        return bounds;
    }

    Point3 Center() const
    {
        const AABB bounds = Bounds();
        return Point3(
            0.5f * (bounds.min_x + bounds.max_x),
            0.5f * (bounds.min_y + bounds.max_y),
            0.5f * (bounds.min_z + bounds.max_z));
    }

    // The single entity, for a group of one. This is how a builder chain hands
    // back something you can attach a rigid body or a tag to.
    entt::entity Entity() const
    {
        SDL_assert(entities.size() == 1 && "Entity() is for single-entity groups; use Entities()");
        return entities.front();
    }

    const std::vector<entt::entity> &Entities() const { return entities; }

private:
    entt::registry *registry;
    std::vector<entt::entity> entities;
};

// Scene is a builder, not a container. It no longer owns geometry - the
// registry does - so all it keeps is the texture library and a log of what it
// created, in creation order, so GroupSince() can hand back a range.
class Scene
{
public:
    TextureLibrary textures;

    Scene() = default;

    // Maximum bounces per camera path. Scenes with participating media need far
    // more than solid geometry does: every scatter inside a medium spends one
    // bounce, so a path can exhaust its budget without ever reaching a light.
    // This is the single biggest cost of turning fog on.
    Uint32 maxDepth = 10; // For path tracing 5 for ray trace 2

    EntityGroup AddSphere(entt::registry &registry, Point3 center, float radius, const Material &mat)
    {
        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Sphere");
        registry.emplace<TransformComponent>(e, center, Identity());
        registry.emplace<RestTransformComponent>(e, center, Identity());
        registry.emplace<MaterialComponent>(e, mat);
        registry.emplace<SphereComponent>(e, radius);
        registry.emplace<MeshComponent>(e, Mesh::CreateSphere(radius, 32, 16, mat));

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // `origin` is one corner; u and v are the full edge vectors out of it, so
    // the quad covers origin + [0,1]u + [0,1]v. They are stored unrotated -
    // orientation lives in the transform and is applied at upload.
    EntityGroup AddQuad(entt::registry &registry, Point3 origin, Vec3<float> u, Vec3<float> v, const Material &mat)
    {
        SDL_assert(mat.type != MaterialType::Isotropic &&
                   "a quad is infinitely thin - a volume needs a Sphere or Box boundary");

        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Quad");
        registry.emplace<TransformComponent>(e, origin, Identity());
        registry.emplace<RestTransformComponent>(e, origin, Identity());
        registry.emplace<MaterialComponent>(e, mat);
        registry.emplace<QuadComponent>(e, u, v);

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // A box spanning the two opposite corners, as a single primitive. This is
    // the one to reach for: it's one BVH object instead of six, and it's the
    // only box form that can bound a volume, since a shell of six infinitely
    // thin quads has no interior to fill.
    //
    // The trade-off against AddBoxShell is that a centre + half-extents + yaw
    // can only rotate about Y, and the whole box takes one material.
    EntityGroup AddBox(entt::registry &registry, Point3 a, Point3 b, const Material &mat)
    {
        const Point3 min(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
        const Point3 max(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));

        const Point3 center = 0.5f * (min + max);
        const Vec3<float> halfExtent = 0.5f * (max - min);

        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Box");
        registry.emplace<TransformComponent>(e, center, Identity());
        registry.emplace<RestTransformComponent>(e, center, Identity());
        registry.emplace<MaterialComponent>(e, mat);
        registry.emplace<BoxComponent>(e, halfExtent);

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // A constant-density medium filling a box. Density is per world unit: the
    // chance of a ray crossing distance d without scattering is exp(-density*d),
    // so 0.01 over a 165-unit box gives a mean free path of 100 units - thin
    // smoke. Raise it for thick fog, lower it for haze.
    EntityGroup AddVolume(entt::registry &registry, Point3 a, Point3 b, Color albedo, float density)
    {
        return AddBox(registry, a, b, Material::Volume(albedo, density));
    }

    // A constant-density medium filling a sphere.
    EntityGroup AddVolume(entt::registry &registry, Point3 center, float radius, Color albedo, float density)
    {
        return AddSphere(registry, center, radius, Material::Volume(albedo, density));
    }

    // A box built from six separate quads. Use this when you want per-face
    // materials or rotation about an axis other than Y; otherwise prefer
    // AddBox. Cannot bound a volume.
    EntityGroup AddBoxShell(entt::registry &registry, Point3 a, Point3 b, const Material &mat)
    {
        const size_t first = created.size();

        const Point3 min(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
        const Point3 max(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));

        const Vec3<float> dx(max.x - min.x, 0.0f, 0.0f);
        const Vec3<float> dy(0.0f, max.y - min.y, 0.0f);
        const Vec3<float> dz(0.0f, 0.0f, max.z - min.z);

        AddQuad(registry, Point3(min.x, min.y, max.z), dx, dy, mat);  // front
        AddQuad(registry, Point3(max.x, min.y, max.z), -dz, dy, mat); // right
        AddQuad(registry, Point3(max.x, min.y, min.z), -dx, dy, mat); // back
        AddQuad(registry, Point3(min.x, min.y, min.z), dz, dy, mat);  // left
        AddQuad(registry, Point3(min.x, max.y, max.z), dx, -dz, mat); // top
        AddQuad(registry, Point3(min.x, min.y, min.z), dx, dz, mat);  // bottom

        return GroupSince(registry, first);
    }

    // A handle to everything added so far, for transforming a whole scene.
    EntityGroup All(entt::registry &registry) { return EntityGroup(registry, created); }

    // A handle to everything added since Count() returned `first`. Use it to
    // transform a cluster you built with a loop as one rigid body:
    //
    //     const size_t start = scene.Count();
    //     for (...) scene.AddSphere(...);
    //     scene.GroupSince(reg, start).RotateY(r).Translate(d);
    EntityGroup GroupSince(entt::registry &registry, size_t first)
    {
        SDL_assert(first <= created.size());
        return EntityGroup(registry,
                           std::vector<entt::entity>(created.begin() + static_cast<ptrdiff_t>(first),
                                                     created.end()));
    }

    size_t Count() const { return created.size(); }

private:
    static Quat<float> Identity() { return Quat<float>(1.0f, 0.0f, 0.0f, 0.0f); }

    // Creation order, so GroupSince/All can hand back a range. This is a log of
    // what the builder made, not an object store - the components are the
    // single source of truth.
    std::vector<entt::entity> created;
};

#endif
