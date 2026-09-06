#ifndef MATERIAL_H
#define MATERIAL_H

#include <SDL3/SDL.h>

#include "scene/texture.h"
#include "math/vec3.h"

// Uploaded verbatim as Object_GPU::colorType, so these values must stay in
// lockstep with the #defines at the top of shader.comp.hlsl.
enum class MaterialType : Uint32
{
    Lambertian = 0,   // matte; scatters into the hemisphere around the normal
    Metal = 1,        // mirror reflection, blurred by `fuzz`
    Dielectric = 2,   // glass/water; refracts by `ior`, reflects by Fresnel
    DiffuseLight = 3, // emits albedo * emission, never scatters
    Isotropic = 4,    // Participating mediums
};

// Material is a small value type - copy it around freely.
//
// Build one with a named constructor instead of a positional field list, so a
// material only ever carries parameters that mean something for its type. The
// old `Material(color, fuzz, refraction, type, tex)` form made every scene
// spell out two zeroes it didn't care about, and silently reused `fuzz` as the
// light intensity for emissive materials.
//
//     Material::Lambertian(Color(0.73f, 0.73f, 0.73f))
//     Material::Metal(Color(0.7f, 0.6f, 0.5f))          // omit fuzz = perfect mirror
//     Material::Metal(Color(0.7f, 0.6f, 0.5f), 0.05f)   // slightly brushed
//     Material::Dielectric(1.5f)                        // 1.5 = glass, 1.33 = water
//     Material::Dielectric(1.5f, Color(0.8f, 1.0f, 0.8f))  // tinted glass
//     Material::Emissive(Color(1.0f, 0.9f, 0.8f), 15.0f)   // warm area light
//
// Any of them takes an optional image texture, which *multiplies* the albedo -
// so albedo doubles as a tint. Leave the albedo white to show the texture as
// authored:
//
//     Material::Lambertian(Color(1, 1, 1)).Textured(scene.textures.Load("brick.jpg"))
class Material
{
public:
    Material() = default;

    static Material Lambertian(Color albedo)
    {
        Material material;
        material.type = MaterialType::Lambertian;
        material.albedo = albedo;
        return material;
    }

    static Material Metal(Color albedo, float fuzz = 0.0f)
    {
        Material material;
        material.type = MaterialType::Metal;
        material.albedo = albedo;
        material.fuzz = fuzz;
        return material;
    }

    static Material Dielectric(float ior, Color tint = Color(1.0f, 1.0f, 1.0f))
    {
        Material material;
        material.type = MaterialType::Dielectric;
        material.albedo = tint;
        material.ior = ior;
        return material;
    }

    static Material Emissive(Color color, float intensity = 1.0f)
    {
        Material material;
        material.type = MaterialType::DiffuseLight;
        material.albedo = color;
        material.emission = intensity;
        return material;
    }

    static Material Volume(Color albedo, float density)
    {
        Material m;
        m.type = MaterialType::Isotropic;
        m.albedo = albedo;
        m.density = density;
        return m;
    }

    // Returns a copy so it chains off a temporary:
    //     Material::Metal(white).Textured(tex)
    Material Textured(const Texture *tex) const
    {
        Material material = *this;
        material.texture = tex;
        return material;
    }

    // The layer the shader should sample, or kNoTexture for flat albedo.
    Uint32 TextureId() const { return texture ? texture->Id() : kNoTexture; }

    Color albedo{1.0f, 1.0f, 1.0f};
    float fuzz = 0.0f;     // Metal only: 0 = mirror, 1 = fully diffuse reflection
    float ior = 1.0f;      // Dielectric only: index of refraction
    float emission = 0.0f; // DiffuseLight only: radiance multiplier on albedo
    float density = 0.0f;   //Volume only
    MaterialType type = MaterialType::Lambertian;
    const Texture *texture = nullptr; // non-owning; owned by TextureLibrary
};

#endif
