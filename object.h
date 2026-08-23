#ifndef OBJECT_H
#define OBJECT_H

#define SphereShapeType 0
#define QuadShapeType 1
#include "vec3.h"
#include "material.h"
#include "bvh.h"
#include <SDL3/SDL.h>

#include <iostream>
#include <algorithm>

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
enum class BodyShape
{
    Sphere,
    Quad
};

// Here are the shapes struct uploaded to the GPU - must match the HLSL Sphere struct

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

    //Translation
    float offset_x;
    float offset_y;
    float offset_z;
    float pad3;

    float angle;
    float pad4[3];

    // Materials
    float r, g, b;
    float fuzz;
    float refraction;


    Uint32 shapeType; // Sphere = 0, Quad = 1
    Uint32 colorType;
    Uint32 textureID;
};

class Object
{
public:
    Object(point3 pos, Material mat, BodyShape shape) : pos(pos), mat(mat), shape(shape)
    {
    }

    virtual ~Object()
    {
    }

    virtual void Rotate_Y()
    {
    }

    virtual AABB Bounds() const = 0;

    virtual Object_GPU CreateObjectGPU() = 0;

    virtual std::unique_ptr<Object> Clone() const = 0;

    point3 pos;
    point3 offset;
    BodyShape shape;
    Material mat;
    BVH_node &bvh_node;
    bool aabbUpdated = false;
};

class Sphere : public Object
{
public:
    Sphere(point3 pos, float radius, Material mat) : Object(pos, mat, BodyShape::Sphere)
    {
        SetRadius(radius);
    }

    AABB Bounds() const override
    {
        float r = radius;
        return AABB{pos.x - r, pos.y - r, pos.z - r, pos.x + r, pos.y + r, pos.z + r};
    }

    void Rotate_Y(float angle) override{
        float sin_theta = std::sin(angle);
        float cos_theta = std::cos(angle);

        point3 min(infinity, infinity, infinity);
        point3 max(-infinity, -infinity, infinity);

        for(int i = 0; i < 2; i++){
            for(int j = 0; j < 2; j++){
                for(int k = 0; k < 2; k++){

                    auto x = i*bvh_node.bbox.max_x + (1-i)*bvh_node.bbox.min_x;
                    auto y = j*bvh_node.bbox.max_y + (1-j)*bvh_node.bbox.min_y;
                    auto z = k*bvh_node.bbox.max_z + (1-k)*bvh_node.bbox.min_z;

                    auto newx =  cos_theta*x + sin_theta*z;
                    auto newz = -sin_theta*x + cos_theta*z;

                    Vec3 tester(newx, y, newz);

                    for (int c = 0; c < 3; c++) {
                        min[c] = std::fmin(min[c], tester[c]);
                        max[c] = std::fmax(max[c], tester[c]);
                    }

                }
            }
        }
    }

    void Translate(point3 newOffset)
    {
        this->offset = newOffset;
        Bounds();
    }

    std::unique_ptr<Object> Clone() const override
    {
        return std::make_unique<Sphere>(*this);
    }

    float GetRadius() { return this->radius; }
    void SetRadius(float newRadius) { this->radius = std::max(newRadius, MIN_RADIUS); }

    Object_GPU CreateObjectGPU() override
    {
        Object_GPU obj_gpu = Object_GPU{
            .x = pos.x,
            .y = pos.y,
            .z = pos.z,

            .x2 = pos.x,
            .y2 = pos.y,
            .z2 = pos.z,

            .radius = radius,

            .r = mat.color.x,
            .g = mat.color.y,
            .b = mat.color.z,

            .fuzz = mat.fuzz,
            .refraction = mat.refraction,
            .shapeType = SphereShapeType,
            .colorType = mat.type};

        return obj_gpu;
    }

private:
    const float MIN_RADIUS = 0.01f;
    float radius;
};

class Quad : public Object
{
public:
    Quad(point3 pos, Vec3<float> u, Vec3<float> v, Material mat) : Object(pos, mat, BodyShape::Quad)
    {

        SetU(u);
        SetV(v);
    }

    AABB Bounds() const override
    {
        AABB bbox_diagonal1 = Surround2Points(this->pos, this->pos + this->u + this->v);
        AABB bbox_diagonal2 = Surround2Points(this->pos + this->u, this->pos + this->v);

        AABB aabb = Surround(bbox_diagonal1, bbox_diagonal2);

        return aabb;
    }

    void Translate(point3 newPos)
    {
        this->pos = pos;
        Bounds();
    }

    void Rotate_Y(float angle) override{
        this->
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
        Object_GPU obj_gpu = Object_GPU{
            .x = pos.x,
            .y = pos.y,
            .z = pos.z,

            .x2 = pos.x,
            .y2 = pos.y,
            .z2 = pos.z,

            .u_x = u.x,
            .u_y = u.y,
            .u_z = u.z,

            .v_x = v.x,
            .v_y = v.y,
            .v_z = v.z,

            .r = mat.color.x,
            .g = mat.color.y,
            .b = mat.color.z,

            .fuzz = mat.fuzz,
            .refraction = mat.refraction,
            .shapeType = QuadShapeType,
            .colorType = mat.type};
        return obj_gpu;
    }

private:
    Vec3<float> u;
    Vec3<float> v;
};

#endif