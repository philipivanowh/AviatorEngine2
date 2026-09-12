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
    Mesh = 4,
};

// Must match the HLSL Object struct byte for byte. Every float3 is followed by
// a pad float because the shader's structured-buffer layout aligns float3 to
// 16 bytes.
struct Object_GPU
{
    // Position
    float x, y, z;

    // Last frame's transform: the motion vector temporal reprojection follows,
    // which is what lets it track a moving body instead of the camera alone.
    // The rotation is the VECTOR part of a unit quaternion - the packer flips
    // the sign when w < 0, so the shader rebuilds w = sqrt(1 - |xyz|^2) and a
    // whole transform fits in padding this struct was already paying for.
    float prevRotX;

    // Last frame's position. Equal to Position for anything that did not move,
    // which is what makes static geometry reproject exactly onto itself.
    float x2, y2, z2;

    // Sphere Radius
    float radius;

    // Quad UVs
    float u_x;
    float u_y;
    float u_z;

    float prevRotY;

    float v_x;
    float v_y;
    float v_z;

    float prevRotZ;

    // Spin of the surface parameterisation about Y, in radians. Only meaningful
    // for spheres and boxes: rotating a sphere doesn't change its geometry, but
    // it does turn the texture on it, and a box is a centre plus half-extents
    // plus one yaw angle. Quads leave this at 0 - their rotation is carried by
    // the u/v edge vectors, which the CPU rotates before upload.
    float half_x, half_y, half_z; // 64  box half-extents
    float rotation;               // 76  Y rotation: sphere UV spin / box yaw
    float density;                // 80  volume density; 0 = solid surface
    float emission;               // 84
    Uint32 instanceIndex;                // 88

    // How strongly the albedo tints the texture, 0..1 (Material::textureTint).
    // Was pad3 - the slot was free, so the stride stays 128.
    float textureTint;            // 92

    // Materials
    float r, g, b;
    float fuzz;
    float refraction;

    Uint32 shapeType; // BodyShape
    Uint32 colorType; // MaterialType
    Uint32 textureID; // texture array layer, or kNoTexture
};

// 48 bytes. Rigid transform only - position + unit quaternion, no scale.
struct MeshInstance_GPU
{
    float px, py, pz;          //  0  instance origin in world space
    float pad0;                // 12
    float qx, qy, qz, qw;      // 16  world rotation; matches Quat<float>'s
                               //     MEMORY layout (x,y,z,w) and HLSL float4.
                               //     Note Quat's 4-arg CTOR is (w,x,y,z).
    Uint32 vertexBase;         // 32
    Uint32 indexBase;          // 36
    Uint32 triangleCount;      // 40
    Uint32 blasBase;           // 44
};
static_assert(sizeof(MeshInstance_GPU) == 48, "must match the HLSL MeshInstance stride");

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
static_assert(offsetof(Object_GPU, textureTint) == 92, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, r) == 96, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, fuzz) == 108, "Object_GPU layout drifted from the shader");
static_assert(offsetof(Object_GPU, textureID) == 124, "Object_GPU layout drifted from the shader");

#endif // GPU_TYPES_H
