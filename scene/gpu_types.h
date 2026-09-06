#ifndef GPU_TYPES_H
#define GPU_TYPES_H

#include <SDL3/SDL.h>
#include <cstddef>

// The CPU side of the shader ABI. Nothing in here knows about entities,
// components or scenes - it is purely the byte layout the compute shader
// reads, kept in its own header so the code that fills it in can change
// freely without touching the contract.

// Uploaded verbatim as Object_GPU::shapeType, so these values must stay in
// lockstep with SphereShapeType/QuadShapeType in path_trace.comp.hlsl.
enum class BodyShape : Uint32
{
    Sphere = 0,
    Quad = 1,
    Box = 2,
    Triangle = 3,
};

// Must match the HLSL Object struct byte for byte. Every float3 is followed by
// a pad float because the shader's structured-buffer layout aligns float3 to
// 16 bytes.
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
    // for spheres and boxes: rotating a sphere doesn't change its geometry, but
    // it does turn the texture on it, and a box is a centre plus half-extents
    // plus one yaw angle. Quads leave this at 0 - their rotation is carried by
    // the u/v edge vectors, which the CPU rotates before upload.
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
//   dxc -spirv -T cs_6_0 -E main path_trace.comp.hlsl -Fo out.spv
//   spirv-dis out.spv | grep "OpMemberDecorate %Object"
static_assert(sizeof(Object_GPU) == 128, "Object_GPU must match the HLSL Object stride");
static_assert(offsetof(Object_GPU, radius) == 28, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, rotation) == 76, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, density) == 80, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, emission) == 84, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, r) == 96, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, fuzz) == 108, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, textureID) == 124, "Object_GPU layout drifted from the shader");

#endif // GPU_TYPES_H
