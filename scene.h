#ifndef SCENE_H
#define SCENE_H

#include "material.h"
#include "object.h"
#include "texture.h"

#include <memory>
#include <vector>

class Scene;

// A handle to the run of objects one Add* call created, so a multi-part shape
// can be transformed as a single rigid thing. This matters: a box is six
// quads, and calling RotateY on each of them individually would spin every
// face about its own centre and tear the box apart. The group rotates them all
// about one shared pivot instead.
//
//     scene.AddBox(a, b, white).RotateY(degrees_to_radians(15.0f))
//                              .Translate(Vec3(265.0f, 0.0f, 295.0f));
//
// Holds indices rather than pointers, so it stays valid as more objects are
// added to the scene.
class ObjectGroup
{
public:
    ObjectGroup(Scene *scene, size_t first, size_t count)
        : scene(scene), first(first), count(count)
    {
    }

    ObjectGroup &Translate(const Vec3<float> &displacement);

    // Rotate about the group's own centre, then about an explicit pivot.
    ObjectGroup &RotateX(float radians);
    ObjectGroup &RotateY(float radians);
    ObjectGroup &RotateZ(float radians);
    ObjectGroup &RotateX(float radians, const point3 &pivot);
    ObjectGroup &RotateY(float radians, const point3 &pivot);
    ObjectGroup &RotateZ(float radians, const point3 &pivot);

    AABB Bounds() const;
    point3 Center() const;

private:
    Scene *scene;
    size_t first;
    size_t count;
};

class Scene
{
public:
    TextureLibrary textures;
    Camera camera;

    Scene(point3 pos, float yaw,float pitch) : camera(pos,yaw,pitch){

    }

    Scene() : camera(point3(0.0f,0.0f,0.0f),0.0f,0.0f){
        
    }

    // Maximum bounces per camera path. Scenes with participating media need far
    // more than solid geometry does: every scatter inside a medium spends one
    // bounce, so a path can exhaust its budget without ever reaching a light.
    // This is the single biggest cost of turning fog on.
    Uint32 maxDepth = 1; //For path tracing 5 for ray trace 2

    Sphere &AddSphere(point3 center, float radius, const Material &mat)
    {
        auto sphere = std::make_unique<Sphere>(center, radius, mat);
        Sphere &ref = *sphere;
        objects.push_back(std::move(sphere));
        return ref;
    }

    // `origin` is one corner; u and v are the full edge vectors out of it, so
    // the quad covers origin + [0,1]u + [0,1]v.
    Quad &AddQuad(point3 origin, Vec3<float> u, Vec3<float> v, const Material &mat)
    {
        SDL_assert(mat.type != MaterialType::Isotropic &&
                   "a quad is infinitely thin - a volume needs a Sphere or Box boundary");

        auto quad = std::make_unique<Quad>(origin, u, v, mat);
        Quad &ref = *quad;
        objects.push_back(std::move(quad));
        return ref;
    }

    // A box spanning the two opposite corners, as a single primitive. This is
    // the one to reach for: it's one BVH object instead of six, and it's the
    // only box form that can bound a volume, since a shell of six infinitely
    // thin quads has no interior to fill.
    //
    // The trade-off against AddBoxShell is that a centre + half-extents + yaw
    // can only rotate about Y, and the whole box takes one material.
    Box &AddBox(point3 a, point3 b, const Material &mat)
    {
        const point3 min(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
        const point3 max(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));

        const point3 center = 0.5f * (min + max);
        const Vec3<float> halfExtent = 0.5f * (max - min);

        auto box = std::make_unique<Box>(center, halfExtent, mat);
        Box &ref = *box;
        objects.push_back(std::move(box));
        return ref;
    }

    // A constant-density medium filling a box. Density is per world unit: the
    // chance of a ray crossing distance d without scattering is exp(-density*d),
    // so 0.01 over a 165-unit box gives a mean free path of 100 units - thin
    // smoke. Raise it for thick fog, lower it for haze.
    Box &AddVolume(point3 a, point3 b, Color albedo, float density)
    {
        return AddBox(a, b, Material::Volume(albedo, density));
    }

    // A constant-density medium filling a sphere.
    Sphere &AddVolume(point3 center, float radius, Color albedo, float density)
    {
        return AddSphere(center, radius, Material::Volume(albedo, density));
    }

    // A box built from six separate quads. Use this when you want per-face
    // materials or rotation about an axis other than Y; otherwise prefer
    // AddBox. Cannot bound a volume.
    ObjectGroup AddBoxShell(point3 a, point3 b, const Material &mat)
    {
        const size_t first = objects.size();

        const point3 min(std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z));
        const point3 max(std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z));

        const Vec3<float> dx(max.x - min.x, 0.0f, 0.0f);
        const Vec3<float> dy(0.0f, max.y - min.y, 0.0f);
        const Vec3<float> dz(0.0f, 0.0f, max.z - min.z);

        AddQuad(point3(min.x, min.y, max.z), dx, dy, mat);   // front
        AddQuad(point3(max.x, min.y, max.z), -dz, dy, mat);  // right
        AddQuad(point3(max.x, min.y, min.z), -dx, dy, mat);  // back
        AddQuad(point3(min.x, min.y, min.z), dz, dy, mat);   // left
        AddQuad(point3(min.x, max.y, max.z), dx, -dz, mat);  // top
        AddQuad(point3(min.x, min.y, min.z), dx, dz, mat);   // bottom

        return ObjectGroup(this, first, objects.size() - first);
    }

    // A handle to everything added so far, for transforming a whole scene.
    ObjectGroup All() { return ObjectGroup(this, 0, objects.size()); }

    // A handle to everything added since Count() returned `first`. Use it to
    // transform a cluster you built with a loop as one rigid body:
    //
    //     const size_t start = scene.Count();
    //     for (...) scene.AddSphere(...);
    //     scene.GroupSince(start).RotateY(r).Translate(d);
    ObjectGroup GroupSince(size_t first)
    {
        return ObjectGroup(this, first, objects.size() - first);
    }

    const std::vector<std::unique_ptr<Object>> &Objects() const { return objects; }
    std::vector<std::unique_ptr<Object>> &Objects() { return objects; }

    size_t Count() const { return objects.size(); }

    // Deep copy of the geometry, sharing the scene's textures. Used to keep an
    // immutable rest pose alongside the copy physics is allowed to mutate.
    std::vector<std::unique_ptr<Object>> CloneObjects() const
    {
        std::vector<std::unique_ptr<Object>> out;
        out.reserve(objects.size());
        for (const auto &object : objects)
            out.push_back(object->Clone());
        return out;
    }

private:
    std::vector<std::unique_ptr<Object>> objects;
};

inline ObjectGroup &ObjectGroup::Translate(const Vec3<float> &displacement)
{
    for (size_t i = first; i < first + count; i++)
        scene->Objects()[i]->Translate(displacement);
    return *this;
}

inline ObjectGroup &ObjectGroup::RotateX(float radians, const point3 &pivot)
{
    for (size_t i = first; i < first + count; i++)
        scene->Objects()[i]->RotateX(radians, pivot);
    return *this;
}

inline ObjectGroup &ObjectGroup::RotateY(float radians, const point3 &pivot)
{
    for (size_t i = first; i < first + count; i++)
        scene->Objects()[i]->RotateY(radians, pivot);
    return *this;
}

inline ObjectGroup &ObjectGroup::RotateZ(float radians, const point3 &pivot)
{
    for (size_t i = first; i < first + count; i++)
        scene->Objects()[i]->RotateZ(radians, pivot);
    return *this;
}

// Center() has to be evaluated before any object moves, otherwise each object
// would be rotated about a pivot that the previous one had already shifted.
inline ObjectGroup &ObjectGroup::RotateX(float radians) { return RotateX(radians, Center()); }
inline ObjectGroup &ObjectGroup::RotateY(float radians) { return RotateY(radians, Center()); }
inline ObjectGroup &ObjectGroup::RotateZ(float radians) { return RotateZ(radians, Center()); }

inline AABB ObjectGroup::Bounds() const
{
    AABB bounds = AABB::Empty();
    for (size_t i = first; i < first + count; i++)
        bounds = Surround(bounds, scene->Objects()[i]->Bounds());
    return bounds;
}

inline point3 ObjectGroup::Center() const
{
    const AABB bounds = Bounds();
    return point3(
        0.5f * (bounds.min_x + bounds.max_x),
        0.5f * (bounds.min_y + bounds.max_y),
        0.5f * (bounds.min_z + bounds.max_z));
}

#endif
