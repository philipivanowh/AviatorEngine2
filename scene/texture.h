#ifndef TEXTURE_H
#define TEXTURE_H

// stb_image is compiled once, in scene/stb_image_impl.cpp - this header only
// pulls in the declarations.
#define STBI_FAILURE_USERMSG
#include "external/stb_image.h"
#include "core/common.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Sentinel stored in Object_GPU::textureID when a material has no image
// texture. 
static constexpr Uint32 kNoTexture = 0xFFFFFFFFu;

// The shader samples one Texture2DArray
static constexpr int kMaxTextureArrayDimension = 2048;

class Texture
{
public:
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    int Width() const { return width; }
    int Height() const { return height; }

    // Layer index into the GPU texture array. Assigned at registration time.
    Uint32 Id() const { return id; }

    bool Valid() const { return width > 0 && height > 0 && !pixels.empty(); }

private:
    friend class TextureLibrary;

    Texture(Uint32 id, std::vector<unsigned char> rgba, int width, int height)
        : pixels(std::move(rgba)), width(width), height(height), id(id)
    {
    }

    std::vector<unsigned char> pixels; // RGBA8, row-major, width*height*4 bytes
    int width = 0;
    int height = 0;
    Uint32 id = kNoTexture;
};

// Owns every texture in a scene and builds the single GPU texture array the
// shader samples. Ids are handed out on registration and are exactly the layer
// index, so there is no separate "register the textures" pass to forget.
class TextureLibrary
{
public:
    // A 1x1 texture of one colour. Rarely what you want - prefer just setting
    // the material albedo - but handy for testing UV mapping.
    Texture *Solid(Color color)
    {
        const std::vector<unsigned char> rgba = {
            ToByte(color.x), ToByte(color.y), ToByte(color.z), 255};
        return Add(rgba, 1, 1);
    }

    // Loads an image off disk. Relative paths are tried against the working
    // directory first, then against the directory the executable lives in, so
    // it works whether you launch from the repo root or from build/bin.
    // Returns nullptr (and logs) if the file can't be read - a material with a
    // null texture simply falls back to its flat albedo.
    Texture *Load(const char *filepath)
    {
        int width = 0;
        int height = 0;
        int channels = 0;

        unsigned char *cpuPixels = stbi_load(filepath, &width, &height, &channels, 4);

        if (!cpuPixels)
        {
            if (const char *basePath = SDL_GetBasePath())
            {
                const std::string fallback = std::string(basePath) + filepath;
                cpuPixels = stbi_load(fallback.c_str(), &width, &height, &channels, 4);
            }
        }

        if (!cpuPixels)
        {
            SDL_Log("Failed to load texture '%s': %s", filepath, stbi_failure_reason());
            return nullptr;
        }

        std::vector<unsigned char> rgba(
            cpuPixels,
            cpuPixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(cpuPixels);

        SDL_Log("Loaded texture '%s' (%dx%d) as layer %zu", filepath, width, height, textures.size());
        return Add(rgba, width, height);
    }

    size_t Count() const { return textures.size(); }

    // Packs every registered texture into one 2D array texture, resampling each
    // to the common layer size. Always returns a bindable texture: with no
    // registered textures you get a single white 1x1 layer, so the shader's
    // sampler binding is never null.
    SDL_GPUTexture *BuildGPUArray(SDL_GPUDevice *device) const
    {
        const Uint32 layerCount = textures.empty() ? 1u : static_cast<Uint32>(textures.size());

        int layerWidth = 1;
        int layerHeight = 1;
        for (const auto &texture : textures)
        {
            layerWidth = std::max(layerWidth, texture->Width());
            layerHeight = std::max(layerHeight, texture->Height());
        }
        layerWidth = std::min(layerWidth, kMaxTextureArrayDimension);
        layerHeight = std::min(layerHeight, kMaxTextureArrayDimension);

        const size_t layerBytes = static_cast<size_t>(layerWidth) * layerHeight * 4;
        const size_t totalBytes = layerBytes * layerCount;

        SDL_Log(
            "Texture array: %u layer(s) at %dx%d (%.1f MB)",
            layerCount,
            layerWidth,
            layerHeight,
            totalBytes / (1024.0 * 1024.0));

        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        
        
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<Uint32>(layerWidth);
        info.height = static_cast<Uint32>(layerHeight);
        info.layer_count_or_depth = layerCount;
        info.num_levels = 1;

        SDL_GPUTexture *array = SDL_CreateGPUTexture(device, &info);
        if (!array)
        {
            SDL_Log("Failed to create texture array: %s", SDL_GetError());
            return nullptr;
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(totalBytes);

        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!transfer)
        {
            SDL_Log("Failed to create texture array transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUTexture(device, array);
            return nullptr;
        }

        unsigned char *staging =
            static_cast<unsigned char *>(SDL_MapGPUTransferBuffer(device, transfer, false));
        if (!staging)
        {
            SDL_Log("Failed to map texture array transfer buffer: %s", SDL_GetError());
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            SDL_ReleaseGPUTexture(device, array);
            return nullptr;
        }

        if (textures.empty())
        {
            SDL_memset(staging, 0xFF, layerBytes); // opaque white
        }
        else
        {
            for (size_t i = 0; i < textures.size(); i++)
            {
                Resample(*textures[i], staging + i * layerBytes, layerWidth, layerHeight);
            }
        }

        SDL_UnmapGPUTransferBuffer(device, transfer);

        SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);

        for (Uint32 layer = 0; layer < layerCount; layer++)
        {
            SDL_GPUTextureTransferInfo source{};
            source.transfer_buffer = transfer;
            source.offset = static_cast<Uint32>(layer * layerBytes);
            source.pixels_per_row = static_cast<Uint32>(layerWidth);
            source.rows_per_layer = static_cast<Uint32>(layerHeight);

            SDL_GPUTextureRegion destination{};
            destination.texture = array;
            destination.layer = layer;
            destination.w = static_cast<Uint32>(layerWidth);
            destination.h = static_cast<Uint32>(layerHeight);
            destination.d = 1;

            SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
        }

        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        SDL_ReleaseGPUTransferBuffer(device, transfer);

        return array;
    }

private:
    Texture *Add(const std::vector<unsigned char> &rgba, int width, int height)
    {
        const Uint32 id = static_cast<Uint32>(textures.size());
        textures.push_back(std::unique_ptr<Texture>(new Texture(id, rgba, width, height)));
        return textures.back().get();
    }

    static unsigned char ToByte(float component)
    {
        const float scaled = component * 255.0f;
        return static_cast<unsigned char>(scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled));
    }

    // Nearest-neighbour resample of one texture into its slot in the staging
    // buffer. Point sampling is fine here: the shader samples the array with a
    // linear filter anyway, and this only runs once at load.
    static void Resample(const Texture &texture, unsigned char *destination, int width, int height)
    {
        const unsigned char *source = texture.pixels.data();
        const int sourceWidth = texture.Width();
        const int sourceHeight = texture.Height();

        for (int y = 0; y < height; y++)
        {
            const int sourceY = (sourceHeight == height)
                                    ? y
                                    : std::min(sourceHeight - 1, (y * sourceHeight) / height);

            for (int x = 0; x < width; x++)
            {
                const int sourceX = (sourceWidth == width)
                                        ? x
                                        : std::min(sourceWidth - 1, (x * sourceWidth) / width);

                SDL_memcpy(
                    destination + (static_cast<size_t>(y) * width + x) * 4,
                    source + (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4,
                    4);
            }
        }
    }

    std::vector<std::unique_ptr<Texture>> textures;
};

#endif
