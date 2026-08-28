#ifndef OBJECT_H
#define OBJECT_H

#include "aabb.h"
#include "vec3.h"
#include "material.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>

enum class ObjectType
{
    Physical,
    Light
};
enum class RigidbodyType
{
    Static,
    Dynamic
};
// Uploaded verbatim as Object_GPU::shapeType, so these values must stay in
// lockstep with SphereShapeType/QuadShapeType in shader.comp.hlsl.
enum class BodyShape : Uint32
{
    Sphere = 0,
    Quad = 1,
    Box = 2,
};

// Here are the shapes struct uploaded to the GPU - must match the HLSL Object
// struct byte for byte. Every float3 is followed by a pad float because the
// shader's structured-buffer layout aligns float3 to 16 bytes.

struct Object_GPU
{
    // Position
    float x, y, z;
    float pad0;
    // Second Position
    float x2, y2, z2;

    // Sphere Radius
    float radius;

    // Quad UVs
    float u_x;
    float u_y;
    float u_z;

    float pad1;

    float v_x;
    float v_y;
    float v_z;

    float pad2;

    // Spin of the surface parameterisation about Y, in radians. Only meaningful
    // for spheres: rotating a sphere doesn't change its geometry, but it does
    // turn the texture on it. Quads leave this at 0 - rotating a quad moves its
    // u/v edge vectors, which carries the texture along for free.
    //
    // Position and rotation are otherwise baked into the geometry on the CPU,
    // so there is no per-object transform for the shader to undo.
    float half_x, half_y, half_z; // 64  box half-extents
    float rotation;               // 76  Y rotation: sphere UV spin / box yaw
    float density;                // 80  volume density; 0 = solid surface
    float emission;               // 84
    float pad3[2];                // 88

    // Materials
    float r, g, b;
    float fuzz;
    float refraction;

    Uint32 shapeType; // BodyShape
    Uint32 colorType; // MaterialType
    Uint32 textureID; // texture array layer, or kNoTexture
};

// These offsets are what `spirv-dis` reports for the HLSL Object struct. A
// mismatch here is invisible at runtime - the shader just reads the wrong
// fields and renders garbage - so pin them down at compile time instead.
// Re-check with:
//   dxc -spirv -T cs_6_0 -E main shader.comp.hlsl -Fo out.spv
//   spirv-dis out.spv | grep "OpMemberDecorate %Object"
static_assert(sizeof(Object_GPU) == 128, "Object_GPU must match the HLSL Object stride");
static_assert(offsetof(Object_GPU, radius) == 28, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, rotation) == 76, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, density) == 80, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, emission) == 84, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, r) == 96, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, fuzz) == 108, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, textureID) == 124, "Object_GPU layout drifted from the shader");

class Object
{
public:
    Object(point3 pos, Material mat, BodyShape shape) : pos(pos), mat(mat), shape(shape)
    {
    }

    virtual ~Object()
    {
    }

    enum class Axis
    {
        X,
        Y,
        Z
    };

    // Transforms are baked straight into the geometry here on the CPU, rather
    // than stored per-object and undone against the ray in the shader. For the
    // primitives we have, that is strictly better:
    //
    //  - It's exact. A quad is defined by a corner and two edge vectors, so
    //    rotating those three is the rotation; a sphere is rotationally
    //    symmetric, so only its centre has to move.
    //  - Bounds() then reports the real world-space box for free, which is what
    //    BuildBVH() needs. Shader-side transforms would have to compute that
    //    same rotated box anyway to keep the BVH correct.
    //  - It costs nothing per ray. The half-written version transformed the ray
    //    by two sin and two cos for every object in every leaf it tested, on
    //    every ray - all to apply an angle that was always zero.
    //
    // Angles are in radians. The no-pivot overloads rotate about the object's
    // own centre, so a lone quad spins in place.
    // All of these return *this so transforms chain the same way they do on an
    // ObjectGroup: scene.AddBox(...).RotateY(r).Translate(d)
    Object &Translate(const Vec3<float> &displacement)
    {
        pos += displacement;
        return *this;
    }

    Object &RotateX(float radians) { return RotateX(radians, Center()); }
    Object &RotateY(float radians) { return RotateY(radians, Center()); }
    Object &RotateZ(float radians) { return RotateZ(radians, Center()); }

    Object &RotateX(float radians, const point3 &pivot) { return Rotate(Axis::X, radians, pivot); }
    Object &RotateY(float radians, const point3 &pivot) { return Rotate(Axis::Y, radians, pivot); }
    Object &RotateZ(float radians, const point3 &pivot) { return Rotate(Axis::Z, radians, pivot); }

    // The point the no-pivot rotations turn about.
    virtual point3 Center() const = 0;

    virtual AABB Bounds() const = 0;

    virtual Object_GPU CreateObjectGPU() = 0;

    virtual std::unique_ptr<Object> Clone() const = 0;

    point3 pos;
    BodyShape shape;
    Material mat;

protected:
    using RotationFunction = Vec3<float> (*)(const Vec3<float> &, float);

    // Swings the object's anchor point around the pivot, then lets the subclass
    // rotate whatever else defines its shape.
    Object &Rotate(Axis axis, float radians, const point3 &pivot)
    {
        const RotationFunction rotate = Rotator(axis);
        pos = pivot + rotate(pos - pivot, radians);
        RotateLocal(axis, rotate, radians);
        return *this;
    }

    // Rotates the shape about its own anchor. Spheres are symmetric so only
    // their texture turns; quads carry their edge vectors around.
    virtual void RotateLocal(Axis axis, RotationFunction rotate, float radians) = 0;

    static RotationFunction Rotator(Axis axis)
    {
        switch (axis)
        {
        case Axis::X:
            return RotateVectorX;
        case Axis::Z:
            return RotateVectorZ;
        case Axis::Y:
        default:
            return RotateVectorY;
        }
    }

    // Fills in everything that doesn't depend on the shape: position, the
    // transform, and the whole material block. Subclasses then only write
    // their own geometry fields.
    Object_GPU BaseObjectGPU() const
    {
        Object_GPU obj_gpu = {};

        obj_gpu.x = pos.x;
        obj_gpu.y = pos.y;
        obj_gpu.z = pos.z;

        // Position2 is the shutter-close / previous-frame centre used for
        // motion blur. Static for now, so it matches Position.
        obj_gpu.x2 = pos.x;
        obj_gpu.y2 = pos.y;
        obj_gpu.z2 = pos.z;

        obj_gpu.r = mat.albedo.x;
        obj_gpu.g = mat.albedo.y;
        obj_gpu.b = mat.albedo.z;

        obj_gpu.fuzz = mat.fuzz;
        obj_gpu.refraction = mat.ior;
        obj_gpu.emission = mat.emission;
        obj_gpu.density = mat.density;

        obj_gpu.shapeType = static_cast<Uint32>(shape);
        obj_gpu.colorType = static_cast<Uint32>(mat.type);
        obj_gpu.textureID = mat.TextureId();

        return obj_gpu;
    }

    static Vec3<float> RotateVectorX(const Vec3<float> &vector, float radians)
    {
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        return {vector.x, cosine * vector.y - sine * vector.z, sine * vector.y + cosine * vector.z};
    }

    static Vec3<float> RotateVectorY(const Vec3<float> &vector, float radians)
    {
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        return {cosine * vector.x + sine * vector.z, vector.y, -sine * vector.x + cosine * vector.z};
    }

    static Vec3<float> RotateVectorZ(const Vec3<float> &vector, float radians)
    {
        const float sine = std::sin(radians);
        const float cosine = std::cos(radians);
        return {cosine * vector.x - sine * vector.y, sine * vector.x + cosine * vector.y, vector.z};
    }
};

class Sphere : public Object
{
public:
    Sphere(point3 pos, float radius, Material mat) : Object(pos, mat, BodyShape::Sphere)
    {
        SetRadius(radius);
    }

    point3 Center() const override { return pos; }

    AABB Bounds() const override
    {
        float r = radius;
        return AABB{pos.x - r, pos.y - r, pos.z - r, pos.x + r, pos.y + r, pos.z + r};
    }

    std::unique_ptr<Object> Clone() const override
    {
        return std::make_unique<Sphere>(*this);
    }

    float GetRadius() { return this->radius; }
    void SetRadius(float newRadius) { this->radius = std::max(newRadius, MIN_RADIUS); }

    Object_GPU CreateObjectGPU() override
    {
        Object_GPU obj_gpu = BaseObjectGPU();
        obj_gpu.radius = radius;
        obj_gpu.rotation = uvRotation;
        return obj_gpu;
    }

protected:
    // A sphere is rotationally symmetric, so spinning one about its own centre
    // leaves the geometry alone - the only thing that actually turns is the
    // texture. Tracked about Y only, which is the axis the sphere's UV
    // parameterisation is built around.
    void RotateLocal(Axis axis, RotationFunction rotate, float radians) override
    {
        (void)rotate;
        if (axis == Axis::Y)
        {
            uvRotation += radians;
        }
    }

private:
    const float MIN_RADIUS = 0.01f;
    float radius;
    float uvRotation = 0.0f;
};

class Quad : public Object
{
public:
    Quad(point3 pos, Vec3<float> u, Vec3<float> v, Material mat) : Object(pos, mat, BodyShape::Quad)
    {

        SetU(u);
        SetV(v);
    }

    // The quad's centre, i.e. the midpoint of the parallelogram.
    point3 Center() const override { return pos + 0.5f * (u + v); }

    AABB Bounds() const override
    {
        AABB bbox_diagonal1 = Surround2Points(this->pos, this->pos + this->u + this->v);
        AABB bbox_diagonal2 = Surround2Points(this->pos + this->u, this->pos + this->v);

        AABB aabb = Surround(bbox_diagonal1, bbox_diagonal2);

        return aabb;
    }

    std::unique_ptr<Object> Clone() const override
    {
        return std::make_unique<Quad>(*this);
    }

    Vec3<float> GetU() { return this->u; }

    Vec3<float> GetV() { return this->v; }

    void SetU(Vec3<float> newU)
    {
        if (!newU.near_zero(0.01f))
            this->u = newU;
        else
            std::cerr << "The vector provided is near zero vector: " << newU.toString() << std::endl;
    }
    void SetV(Vec3<float> newV)
    {
        if (!newV.near_zero(0.01f))
            this->v = newV;
        else
            std::cerr << "The vector provided is near zero vector: " << newV.toString() << std::endl;
    }

    Object_GPU CreateObjectGPU() override
    {
        Object_GPU obj_gpu = BaseObjectGPU();

        obj_gpu.u_x = u.x;
        obj_gpu.u_y = u.y;
        obj_gpu.u_z = u.z;

        obj_gpu.v_x = v.x;
        obj_gpu.v_y = v.y;
        obj_gpu.v_z = v.z;

        return obj_gpu;
    }

protected:
    // A quad is a corner plus two edge vectors, so rotating those edges *is*
    // the rotation - and it carries the texture with it, since the shader's UVs
    // are the quad's own (alpha, beta) coordinates along u and v.
    void RotateLocal(Axis axis, RotationFunction rotate, float radians) override
    {
        (void)axis;
        u = rotate(u, radians);
        v = rotate(v, radians);
    }

private:
    Vec3<float> u;
    Vec3<float> v;
};

class Box : public Object
{
public:
    Box(point3 center, Vec3<float> halfExtent, Material mat)
        : Object(center, mat, BodyShape::Box), halfExtent(halfExtent) {}

    point3 Center() const override { return pos; }

    // World-space AABB of the yaw-rotated box. Rotating the half-extent vector
    // and taking absolute values gives the extent along each world axis.
    AABB Bounds() const override
    {
        const float s = std::fabs(std::sin(yaw));
        const float c = std::fabs(std::cos(yaw));
        const float ex = c * halfExtent.x + s * halfExtent.z;
        const float ez = s * halfExtent.x + c * halfExtent.z;
        return AABB{pos.x - ex, pos.y - halfExtent.y, pos.z - ez,
                    pos.x + ex, pos.y + halfExtent.y, pos.z + ez};
    }

    Object_GPU CreateObjectGPU() override
    {
        Object_GPU o = BaseObjectGPU();
        o.half_x = halfExtent.x;
        o.half_y = halfExtent.y;
        o.half_z = halfExtent.z;
        o.rotation = yaw;
        return o;
    }

    std::unique_ptr<Object> Clone() const override { return std::make_unique<Box>(*this); }

protected:
    void RotateLocal(Axis axis, RotationFunction rotate, float radians) override
    {
        (void)rotate;
        // A centre + half-extents + yaw can only express rotation about Y.
        // X/Z would need a full OBB (three orthonormal axes instead of one
        // angle) - fail loudly rather than silently doing nothing.
        SDL_assert(axis == Axis::Y && "Box only supports RotateY; use a quad shell for other axes");
        if (axis == Axis::Y)
            yaw += radians;
    }

private:
    Vec3<float> halfExtent;
    float yaw = 0.0f;
};

#endif
