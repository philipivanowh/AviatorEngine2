#ifndef SCENE_H
#define SCENE_H

#include <array>
#include <map>
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

// Surface response for the colliders the Add*Physics* helpers create. Friction
// and bounce belong to the surface rather than to the body, which is why they
// live here and in ColliderComponent instead of in RigidbodyComponent - a
// static ramp has both, and it has no rigid body at all.
struct PhysicsSurface
{
    float friction = 0.5f;    // Coulomb coefficient; 0 is ice
    float restitution = 0.0f; // 0 = no bounce, 1 = perfectly elastic
};

// Scene is a builder, not a container. It no longer owns geometry - the
// registry does - so all it keeps is the texture library and a log of what it
// created, in creation order, so GroupSince() can hand back a range.
class Scene
{
public:
    TextureLibrary textures;
    MeshLibrary meshes;

    Scene() = default;

    // Maximum bounces per camera path. Scenes with participating media need far
    // more than solid geometry does: every scatter inside a medium spends one
    // bounce, so a path can exhaust its budget without ever reaching a light.
    // This is the single biggest cost of turning fog on.
    Uint32 maxDepth = 8; // For path tracing 5 for ray trace 2

    // AddSphere and AddQuad create MESH INSTANCES: the rasterizer draws them
    // straight into the G-buffer, so primary visibility never walks the BVH for
    // them. Geometry is shared per distinct size (SphereMesh and friends
    // below), so a thousand equal spheres are one mesh. AddBox is the
    // exception - see there.
    //
    // Two kinds of material are routed to the analytic versions instead,
    // automatically, because a triangle surface cannot do their job:
    //   - emissive: direct lighting samples a light by its shape (SampleLight
    //     in the shaders) and only understands analytic quads and spheres, so
    //     a mesh light would stop lighting anything directly
    //   - volumes: a medium needs the entry/exit span of a closed analytic
    //     shape
    //
    // Call AddAnalytic* yourself for exact curvature (a very large sphere is
    // visibly faceted as a mesh) or for the cheaper secondary-ray test: one
    // equation per ray instead of a walk through the mesh's triangles.
    EntityGroup AddSphere(entt::registry &registry, Point3 center, float radius, const Material &mat)
    {
        if (!MeshEligible(mat))
        {
            return AddAnalyticSphere(registry, center, radius, mat);
        }
        return TagAs(registry, AddMeshInstance(registry, SphereMesh(radius), center, mat), "Sphere");
    }

    // --- Physics bodies ---------------------------------------------------
    //
    // Each helper creates the render shape AND the physics components in one
    // call, so an object can never end up visible but unsimulated, or simulated
    // but invisible. IPhysicsWorld::Build reads them straight out of the
    // registry afterwards - nothing here knows that Newton exists.
    //
    //     scene.AddPhysicsGround(reg, 0.0f, 40.0f, concrete);
    //     scene.AddPhysicsBox(reg, Point3(0.0f, 5.0f, 0.0f), Vec3<float>(0.5f), red);
    //     scene.AddPhysicsSphere(reg, Point3(0.0f, 9.0f, 0.0f), 0.5f, steel,
    //                            RigidbodyComponent{.mass = 20.0f},
    //                            PhysicsSurface{0.4f, 0.7f});

    // A box that physics owns. Rendered as a MESH box rather than an analytic
    // one: a tumbling body comes back rotated about every axis, and a centre +
    // half-extents + yaw cannot express that - BoxToGPU asserts on it.
    EntityGroup AddPhysicsBox(entt::registry &registry, Point3 center, Vec3<float> halfExtent,
                              const Material &mat,
                              const RigidbodyComponent &body = RigidbodyComponent(),
                              const PhysicsSurface &surface = PhysicsSurface())
    {
        SDL_assert(MeshEligible(mat) &&
                   "a physics box needs a mesh-eligible material: not emissive, not a volume");

        EntityGroup group = AddMeshBox(registry, center - halfExtent, center + halfExtent, mat);
        registry.emplace<RigidbodyComponent>(group.Entity(), body);
        registry.emplace<ColliderComponent>(group.Entity(), BoxCollider(halfExtent, surface));
        return group;
    }

    // A sphere that physics owns. Analytic on purpose: a sphere is exact at any
    // size, costs one equation per secondary ray instead of a walk through a
    // 1024-triangle BLAS, and rotating it only turns its texture.
    EntityGroup AddPhysicsSphere(entt::registry &registry, Point3 center, float radius,
                                 const Material &mat,
                                 const RigidbodyComponent &body = RigidbodyComponent(),
                                 const PhysicsSurface &surface = PhysicsSurface())
    {
        // A MESH sphere rather than an analytic one, which is the opposite of
        // what a still scene wants. Only rasterized geometry reaches the
        // G-buffer, and below full trace resolution upsample.comp reprojects
        // every pixel from that G-buffer: an analytic sphere has no record
        // there, so a moving one would reproject against whatever is behind it
        // and smear. Use AddStaticSphere for anything that never moves.
        SDL_assert(MeshEligible(mat) && "a physics sphere needs a mesh-eligible material");

        EntityGroup group =
            TagAs(registry, AddMeshInstance(registry, SphereMesh(radius), center, mat), "Sphere");
        registry.emplace<RigidbodyComponent>(group.Entity(), body);
        registry.emplace<ColliderComponent>(group.Entity(), SphereCollider(radius, surface));
        return group;
    }

    // Static collision geometry with a mesh box for a render shape: walls,
    // platforms, and ramps - a ramp being a box rotated about X or Z, which is
    // exactly what an analytic box cannot draw.
    EntityGroup AddStaticBox(entt::registry &registry, Point3 center, Vec3<float> halfExtent,
                             const Material &mat,
                             const PhysicsSurface &surface = PhysicsSurface())
    {
        SDL_assert(MeshEligible(mat) && "a static physics box needs a mesh-eligible material");

        EntityGroup group = AddMeshBox(registry, center - halfExtent, center + halfExtent, mat);
        registry.emplace<ColliderComponent>(group.Entity(), BoxCollider(halfExtent, surface));
        return group;
    }

    // Static collision geometry, sphere flavour. Also the building block for
    // MakeCompoundBody, which welds parts like this into one rigid body.
    EntityGroup AddStaticSphere(entt::registry &registry, Point3 center, float radius,
                                const Material &mat,
                                const PhysicsSurface &surface = PhysicsSurface())
    {
        EntityGroup group = AddAnalyticSphere(registry, center, radius, mat);
        registry.emplace<ColliderComponent>(group.Entity(), SphereCollider(radius, surface));
        return group;
    }

    // The floor: a slab whose TOP face sits exactly at `height`, with a collider
    // that matches it. A thick box rather than an infinitely thin plane, so
    // nothing can tunnel through from below - and it stays on the analytic
    // raster-proxy fast path, because a floor never rotates.
    EntityGroup AddPhysicsGround(entt::registry &registry, float height, float halfSize,
                                 const Material &mat,
                                 const PhysicsSurface &surface = PhysicsSurface{0.9f, 0.0f})
    {
        const Vec3<float> halfExtent(halfSize, 0.5f, halfSize);
        const Point3 center(0.0f, height - halfExtent.y, 0.0f);

        EntityGroup group = AddAnalyticBox(registry, center - halfExtent, center + halfExtent, mat);
        registry.emplace<ColliderComponent>(group.Entity(), BoxCollider(halfExtent, surface));
        return group;
    }

    // Welds collider-only entities into ONE rigid body: a new pivot entity
    // carries the RigidbodyComponent at the parts' centre, and every part
    // becomes a ShapeOfComponent of it, keeping the local pose it has right
    // now. Physics then moves the parts with the body every step, which is what
    // keeps a multi-part thing rigid (see ShapeOfComponent in components.h).
    //
    //     const size_t first = scene.Count();
    //     scene.AddStaticSphere(reg, Point3(-0.9f, 6.0f, 0.0f), 0.45f, steel);
    //     scene.AddStaticSphere(reg, Point3( 0.9f, 6.0f, 0.0f), 0.45f, steel);
    //     scene.AddStaticBox(reg, Point3(0.0f, 6.0f, 0.0f), Vec3<float>(0.9f, 0.12f, 0.12f), chrome);
    //     scene.MakeCompoundBody(reg, scene.GroupSince(reg, first), RigidbodyComponent{.mass = 14.0f});
    EntityGroup MakeCompoundBody(entt::registry &registry, const EntityGroup &parts,
                                 const RigidbodyComponent &body = RigidbodyComponent())
    {
        SDL_assert(!parts.Entities().empty() && "a compound body needs at least one part");

        const Point3 pivot = parts.Center();

        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Body");
        registry.emplace<TransformComponent>(e, pivot, Identity());
        registry.emplace<RestTransformComponent>(e, pivot, Identity());
        registry.emplace<RigidbodyComponent>(e, body);

        for (entt::entity part : parts.Entities())
        {
            SDL_assert(registry.all_of<ColliderComponent>(part) &&
                       "a compound part needs a ColliderComponent - build it with AddStatic*");

            // The pivot is created unrotated, so a part's local pose is simply
            // its offset from the pivot plus its own rotation.
            const TransformComponent &t = registry.get<TransformComponent>(part);
            registry.remove<RigidbodyComponent>(part); // the body moves the part, not the reverse
            registry.emplace_or_replace<ShapeOfComponent>(part, e, t.position - pivot, t.rotation);
        }

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // The two colliders the helpers above hand out. Public because authoring
    // code sometimes wants to attach one by hand - a mesh instance that should
    // collide as a box, say.
    static ColliderComponent BoxCollider(const Vec3<float> &halfExtent, const PhysicsSurface &surface)
    {
        ColliderComponent collider;
        collider.shape = ColliderShape::Box;
        collider.halfExtent = halfExtent;
        collider.friction = surface.friction;
        collider.restitution = surface.restitution;
        return collider;
    }

    static ColliderComponent SphereCollider(float radius, const PhysicsSurface &surface)
    {
        ColliderComponent collider;
        collider.shape = ColliderShape::Sphere;
        collider.radius = radius;
        collider.friction = surface.friction;
        collider.restitution = surface.restitution;
        return collider;
    }

    EntityGroup AddAnalyticSphere(entt::registry &registry, Point3 center, float radius, const Material &mat)
    {
        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Sphere");
        registry.emplace<TransformComponent>(e, center, Identity());
        registry.emplace<RestTransformComponent>(e, center, Identity());
        registry.emplace<MaterialComponent>(e, mat);
        registry.emplace<SphereComponent>(e, radius);

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // A mesh instance: shared geometry from the library, positioned by this
    // entity's transform. Prefer this over exploding a mesh into individual
    // triangle entities - a tessellated sphere is ~2000 triangles, which as
    // standalone objects is 256 KB of Object_GPU and a full BVH rebuild every
    // time it moves, against 128 bytes and one instance here.
    EntityGroup AddMeshInstance(entt::registry &registry, MeshHandle mesh, Point3 origin, const Material &mat)
    {
        SDL_assert(mat.type != MaterialType::Isotropic &&
                   "a mesh surface cannot bound a volume - use a Sphere or Box");
        SDL_assert(mesh != kNoMesh && "AddMeshInstance needs a mesh registered with scene.meshes");

        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Mesh");
        registry.emplace<TransformComponent>(e, origin, Identity());
        registry.emplace<RestTransformComponent>(e, origin, Identity());
        registry.emplace<MaterialComponent>(e, mat);

        // The local bounds are cached on the component so bounds and BVH
        // gathering stay pure functions of the registry - see MeshBounds.
        registry.emplace<MeshComponent>(e, mesh, meshes.Range(mesh).localBounds);

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // `origin` is one corner; u and v are the full edge vectors out of it, so
    // the quad covers origin + [0,1]u + [0,1]v. As a mesh the instance sits at
    // the quad's middle (see Mesh::CreateQuad) and is drawn two-sided.
    EntityGroup AddQuad(entt::registry &registry, Point3 origin, Vec3<float> u, Vec3<float> v, const Material &mat)
    {
        SDL_assert(mat.type != MaterialType::Isotropic &&
                   "a quad is infinitely thin - a volume needs a Sphere or Box boundary");

        if (!MeshEligible(mat))
        {
            return AddAnalyticQuad(registry, origin, u, v, mat);
        }
        return TagAs(registry, AddMeshInstance(registry, QuadMesh(u, v), origin + 0.5f * (u + v), mat), "Quad");
    }

    // The analytic quad stores u and v unrotated - orientation lives in the
    // transform and is applied at upload.
    EntityGroup AddAnalyticQuad(entt::registry &registry, Point3 origin, Vec3<float> u, Vec3<float> v, const Material &mat)
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

    EntityGroup AddTriangle(entt::registry &registry, Point3 a, Point3 b, Point3 c, const Material &mat)
    {
        SDL_assert(mat.type != MaterialType::Isotropic &&
                   "a triangle is infinitely thin - a volume needs a Sphere or Box boundary");

        const entt::entity e = registry.create();
        registry.emplace<TagComponent>(e, "Triangle");
        registry.emplace<TransformComponent>(e, a, Identity());
        registry.emplace<RestTransformComponent>(e, a, Identity());
        registry.emplace<MaterialComponent>(e, mat);
        registry.emplace<TriangleComponent>(e, b - a, c - a);

        created.push_back(e);
        return EntityGroup(registry, e);
    }

    // A box spanning the two opposite corners - ANALYTIC, unlike AddSphere and
    // AddQuad, and still rasterized. The renderer draws an exact stretched-cube
    // proxy for every plain box (IsRasterProxy in renderer.h), so primary
    // visibility comes from the G-buffer either way. What differs is every
    // bounce and shadow ray: against the analytic box that is one slab test,
    // against a mesh box it is an instance transform plus a walk through a
    // 12-triangle BLAS with index and vertex fetches per triangle. A 400-box
    // ground is tested by nearly every secondary ray, so the mesh version cost
    // frame rate for no visual difference.
    //
    // A proxy needs an unrotated, untextured, non-volume box; any other box is
    // traced for primary visibility too. Use AddMeshBox for a box rotated about
    // X or Z, which the analytic box cannot express.
    EntityGroup AddBox(entt::registry &registry, Point3 a, Point3 b, const Material &mat)
    {
        return AddAnalyticBox(registry, a, b, mat);
    }

    // A box as a mesh instance: rotates freely about any axis, at the cost of a
    // BLAS walk for every secondary ray that tests it.
    EntityGroup AddMeshBox(entt::registry &registry, Point3 a, Point3 b, const Material &mat)
    {
        if (!MeshEligible(mat))
        {
            return AddAnalyticBox(registry, a, b, mat);
        }

        const Point3 min(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
        const Point3 max(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));

        return TagAs(registry,
                     AddMeshInstance(registry, BoxMesh(0.5f * (max - min)), 0.5f * (min + max), mat),
                     "Box");
    }

    // A box spanning the two opposite corners, as a single analytic primitive:
    // one BVH object instead of six, and the only box form that can bound a
    // volume, since a shell of six infinitely thin quads has no interior to
    // fill. A centre + half-extents + yaw can only rotate about Y.
    EntityGroup AddAnalyticBox(entt::registry &registry, Point3 a, Point3 b, const Material &mat)
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
        return AddAnalyticBox(registry, a, b, Material::Volume(albedo, density));
    }

    // A constant-density medium filling a sphere.
    EntityGroup AddVolume(entt::registry &registry, Point3 center, float radius, Color albedo, float density)
    {
        return AddAnalyticSphere(registry, center, radius, Material::Volume(albedo, density));
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

    // Tessellation of mesh spheres: 32 x 16 is 1024 triangles, which shades
    // round at the sizes the feature scene uses. Every triangle is paid for by
    // each secondary ray that enters the sphere's BLAS, so lower it for dense
    // clusters of small spheres and raise it for ones that fill the screen.
    static constexpr uint16_t kSphereSegments = 32;
    static constexpr uint16_t kSphereRings = 16;

    static bool MeshEligible(const Material &mat)
    {
        return mat.type != MaterialType::DiffuseLight && mat.type != MaterialType::Isotropic;
    }

    // AddMeshInstance tags every entity "Mesh"; keep the shape's own name, which
    // is what anything reading TagComponent while debugging expects to see.
    static EntityGroup TagAs(entt::registry &registry, EntityGroup group, const char *tag)
    {
        registry.get<TagComponent>(group.Entity()).tag = tag;
        return group;
    }

    // One mesh per distinct size, shared by every instance of that size. The
    // material is not part of the key: mesh vertices carry no colour, the
    // instance's MaterialComponent does.
    MeshHandle SphereMesh(float radius)
    {
        const auto found = sphereMeshes.find(radius);
        if (found != sphereMeshes.end())
        {
            return found->second;
        }
        const MeshHandle handle = meshes.Add(Mesh::CreateSphere(radius, kSphereSegments, kSphereRings, Material()));
        sphereMeshes.emplace(radius, handle);
        return handle;
    }

    MeshHandle BoxMesh(const Vec3<float> &halfExtent)
    {
        const std::array<float, 3> key = {halfExtent.x, halfExtent.y, halfExtent.z};
        const auto found = boxMeshes.find(key);
        if (found != boxMeshes.end())
        {
            return found->second;
        }
        // Local bounds centred on the origin - CreateBox re-centres vertices on
        // them, so world placement belongs to the instance alone.
        const MeshHandle handle = meshes.Add(Mesh::CreateBox(
            halfExtent, Material(),
            AABB{-halfExtent.x, -halfExtent.y, -halfExtent.z, halfExtent.x, halfExtent.y, halfExtent.z}));
        boxMeshes.emplace(key, handle);
        return handle;
    }

    MeshHandle QuadMesh(const Vec3<float> &u, const Vec3<float> &v)
    {
        const std::array<float, 6> key = {u.x, u.y, u.z, v.x, v.y, v.z};
        const auto found = quadMeshes.find(key);
        if (found != quadMeshes.end())
        {
            return found->second;
        }
        const MeshHandle handle = meshes.Add(Mesh::CreateQuad(u, v, Material()), true /* doubleSided */);
        quadMeshes.emplace(key, handle);
        return handle;
    }

    std::map<float, MeshHandle> sphereMeshes;
    std::map<std::array<float, 3>, MeshHandle> boxMeshes;
    std::map<std::array<float, 6>, MeshHandle> quadMeshes;

    // Creation order, so GroupSince/All can hand back a range. This is a log of
    // what the builder made, not an object store - the components are the
    // single source of truth.
    std::vector<entt::entity> created;
};

#endif
