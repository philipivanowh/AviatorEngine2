// G-buffer fragment stage. Writes surface attributes, NOT lighting - every
// light in this renderer is evaluated by the compute pass with real shadow
// rays. If you find yourself adding a light loop here, you have accidentally
// rebuilt a forward renderer.

#define INVALID_TEXTURE 0xFFFFFFFFu

struct PSInput
{
    float4 clipPosition : SV_Position;
    float3 worldPosition: TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float2 uv           : TEXCOORD2;
};

struct PSOutput
{
    float4 position : SV_Target0;  // xyz world, w = object index
    float4 normal   : SV_Target1;  // xyz world normal, w = coverage
    float4 albedo   : SV_Target2;  // rgb albedo, a = 0 for view-dependent (metal, glass)
};

// Fragment-stage samplers live in space2, uniforms in space3.
Texture2DArray GlobalTextureArray : register(t0, space2);
SamplerState   GlobalSampler      : register(s0, space2);

cbuffer SurfaceBlock : register(b0, space3)
{
    float4 Albedo;       // rgb = Object.Albedo (a tint, not a replacement)
    uint   TextureID;    // layer in GlobalTextureArray, or INVALID_TEXTURE
    uint   ObjectIndex;  // slot in the `objects` StructuredBuffer
    float  TextureTint;  // how strongly Albedo tints the texture, 0..1
    uint   ColorType;    // MaterialType; decides the albedo alpha below
};

// Must match MaterialType in material.h.
#define METAL 1u
#define DIELECTRIC 2u

PSOutput main(PSInput input)
{
    PSOutput output;

    // The same formula as SurfaceAlbedo() in the compute shaders - it has to
    // be, or a rasterized mesh and its ray traced reflection disagree in colour.
    float3 albedo = Albedo.rgb;
    if (TextureID != INVALID_TEXTURE)
    {
        const float3 texel = GlobalTextureArray.Sample(GlobalSampler, float3(input.uv, TextureID)).rgb;
        albedo = texel * lerp(float3(1.0f, 1.0f, 1.0f), Albedo.rgb, TextureTint);
    }

    // Object index goes through a float. Exact up to 2^24 in R32F, which is
    // several orders of magnitude past any scene this engine will hold.
    output.position = float4(input.worldPosition, (float)ObjectIndex);

    // Coverage in .w, not an alpha test: the compute pass needs to distinguish
    // "background" from "a surface whose normal happens to be zero".
    output.normal   = float4(normalize(input.worldNormal), 1.0f);
    // Alpha flags view-dependent surfaces for temporal reprojection: a
    // reflection or refraction does not move with the surface it is seen in,
    // so upsample.comp keeps only a short history for these while the camera
    // moves, where diffuse lighting can safely keep a long one.
    const bool viewDependent = ColorType == METAL || ColorType == DIELECTRIC;
    output.albedo   = float4(albedo, viewDependent ? 0.0f : 1.0f);
    return output;
}