#ifndef OBJECT_H
#define OBJECT_H

#include "vec3.h"
#include "material.h"
#include <SDL3/SDL.h>

#include <iostream>
#include <algorithm>

enum class ObjectType {Physical, Light};
enum class RigidbodyType {Static, Dynamic};
enum class BodyShape {Sphere, Quad};

// Here are the shapes struct uploaded to the GPU - must match the HLSL Sphere struct

struct Sphere_GPU
{
    float x, y, z;
    float pad0;
    float x2, y2, z2;
    float radius;
    float r, g, b;
    float fuzz;
    float refraction;
    Uint32 type;
    Uint32 textureID;
    Uint32 padding;
};

// struct Quad_GPU
// {
//     float x, y, z;

// }



class Object{
    public:
        Object(point3 pos, Material mat, BodyShape shape) : pos(pos), mat(mat), shape(shape){
        }
        ~Object(){

        }

        point3 pos;
        BodyShape shape;
        Material mat;
};


class Sphere : public Object{
    public:
        Sphere(point3 pos, float radius ,Material mat) : Object(pos, mat, BodyShape::Sphere) {
            SetRadius(radius);
        }
        float GetRadius() {return this->radius;}
        void SetRadius(float newRadius) {this->radius = std::max(newRadius, MIN_RADIUS);}

    private:
        const float MIN_RADIUS = 0.01f;
        float radius;
};

class Quad : public Object{
    public:
        Quad(point3 pos, Vec3<float> u, Vec3<float> v, Material mat) : Object(pos, mat, BodyShape::Quad){
            
            SetU(u);
            SetV(v);
        }
        Vec3<float> GetU() {return this->u;}
        Vec3<float> GetV() {return this->v;}
        void SetU(Vec3<float> newU) {
            if(!newU.near_zero(0.01f)) 
                this->u = newU;
            else 
                std::cerr << "The vector provided is near zero vector: " << newU.toString() << std::endl;
        }
        void SetV(Vec3<float> newV) {
            if(!newV.near_zero(0.01f)) 
                this->v = newV;
            else 
                std::cerr << "The vector provided is near zero vector: " <<  newV.toString() << std::endl;
        }
    
    private:
        Vec3<float> u;
        Vec3<float> v;
};

#endif