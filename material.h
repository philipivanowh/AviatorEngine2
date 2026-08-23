#ifndef MATERIAL_H
#define MATERIAL_H

#define LAMBERTIAN 0
#define METAL 1
#define DIAELECTRIC 2
#define DIFFUSE_LIGHT 3

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "texture.h"
#include "vec3.h"

class Material
{
public:
    Material(Color color, float fuzz, float refraction, Uint32 type, Texture *tex = nullptr)
    {
        this->color = color;
        this->fuzz = fuzz;
        this->refraction = refraction;
        this->type = type;
        this->texture = tex;
    }

    Color color;
    float fuzz;
    float refraction;
    Uint32 type;
    Texture *texture;
};

#endif