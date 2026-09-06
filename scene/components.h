#ifndef COMPONENTS_H
#define COMPONENTS_H

#include <string>

#include <entt/entt.hpp>

#include "math/vec3.h"
#include "math/quat.hpp"
#include "core/color.h"
#include "scene/material.h"
#include "scene/mesh.h"

// Components are plain data. Everything that used to be a virtual on Object
// lives in shapes.h as a free function that reads these - see the note there
// on why the split is worth it.
//
// The rule for shape components: they hold LOCAL, unrotated geometry. A quad's
// u/v are its edge vectors with the object unrotated; a box's halfExtent is
// axis-aligned. World space is TransformComponent applied on top, at the point
// of use (bounds, GPU upload). Nothing bakes a transform into geometry, because
// a physics integrator rewrites the transform every step and baking would
// accumulate drift into the authored shape.

struct TagComponent
{
    std::string tag;
};

struct TransformComponent
{
    Point3 position = Point3(0.0f, 0.0f, 0.0f);
    Quat<float> rotation = Quat<float>(1.0f, 0.0f, 0.0f, 0.0f);
};

// The authored pose, so a body can be reset after physics has moved it. This
// replaces the deep-copied "rest pose" vector of Objects: keeping a second
// transform per entity costs 28 bytes, keeping a second object graph cost a
// full clone plus a virtual Clone() on every shape.
struct RestTransformComponent
{
    Point3 position = Point3(0.0f, 0.0f, 0.0f);
    Quat<float> rotation = Quat<float>(1.0f, 0.0f, 0.0f, 0.0f);
};

struct MaterialComponent
{
    Material material;
};

// --- Shape components. Exactly one per renderable entity; which one an entity
// has is what BodyShape used to encode, and dispatching on it is a view lookup
// instead of a vtable hop.

struct SphereComponent
{
    float radius = 1.0f;
};

struct QuadComponent
{
    Vec3<float> u = Vec3<float>(1.0f, 0.0f, 0.0f);
    Vec3<float> v = Vec3<float>(0.0f, 1.0f, 0.0f);
};

struct BoxComponent
{
    Vec3<float> halfExtent = Vec3<float>(1.0f, 1.0f, 1.0f);
};

// Placeholder for the vertex-buffer path. When it lands it slots in as a
// fourth shape component - a new struct, a new case in the shapes.h
// dispatchers, and nothing existing changes. Note that meshes want a two-level
// BVH (a per-mesh BLAS in local space, a TLAS over instances) rather than the
// single flat BVH the analytic shapes use.
struct MeshComponent
{
    Mesh mesh;
};

// --- Physics.

enum class RigidbodyType
{
    Static,
    Dynamic
};

struct RigidBodyComponent
{
    Vec3<float> velocity = Vec3<float>(0.0f);
    Vec3<float> angularVelocity = Vec3<float>(0.0f);
    float invMass = 0.0f; // 0 = infinite mass, i.e. immovable
    RigidbodyType type = RigidbodyType::Static;
};

// Attached to collider entities that belong to a compound body: the shape's
// pose is derived from the body's transform each step rather than integrated
// on its own. This is what keeps a multi-part rigid thing rigid - the job
// ObjectGroup used to do at authoring time, but recorded in the registry so it
// still exists at frame N.
struct ShapeOfComponent
{
    entt::entity body = entt::null;
    Point3 localPosition = Point3(0.0f, 0.0f, 0.0f);
    Quat<float> localRotation = Quat<float>(1.0f, 0.0f, 0.0f, 0.0f);
};

struct CameraComponent
{
    Vec3<float> target = Vec3<float>(0.0f, 0.0f, 0.0f);
    float yaw = 0.0f;   // radians, rotation around world Y
    float pitch = 0.0f; // radians, clamped to +-PITCH
    float fov = 40.0f;
    float focus_dist = 10.0f;
    float defocus_angle = 0.2f;
    Color sky{0.0f, 0.0f, 0.0f};
    Color horizon{0.0f, 0.0f, 0.0f};
    float CAMERA_MOVE_SPEED = 10.0f; // units/sec at normal (non-sprint) speed
    float CAMERA_SPRINT_MULTIPLIER = 3.0f;
    float MOUSE_SENSITIVITY = 0.0025f; // radians of turn per pixel of mouse delta
    float PITCH_LIMIT = 1.5533f;       // ~89 degrees; stops the camera flipping over at the poles
};

#endif
