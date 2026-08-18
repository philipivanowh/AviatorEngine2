#ifndef TEXTURE_H
#define TEXTURE_H

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "external/stb_image.h"
#include "common.h"

#include <cstdlib>
#include <iostream>

class Texture
{
public:
    // Loads a texture from an image file on disk.
    Texture(SDL_GPUDevice *device, const char *filepath)
    {
        unsigned char *cpuPixels = stbi_load(filepath, &image_width, &image_height, &channels, 4);
        if (!cpuPixels)
        {
            SDL_Log("STB failed to load image '%s': %s", filepath, stbi_failure_reason());
            return;
        }
        UploadPixels(device, cpuPixels, image_width, image_height);
        stbi_image_free(cpuPixels);
    }

    // Builds a texture directly from raw RGBA8 pixel data already in memory -
    // no file, no stb_image. Used for procedural/fallback textures such as
    // the default white texture padded into unused GlobalTextures[] slots.
    Texture(SDL_GPUDevice *device, const unsigned char *rgbaPixels, int width, int height)
    {
        image_width = width;
        image_height = height;
        channels = 4;
        UploadPixels(device, rgbaPixels, width, height);
    }

    // Convenience factory: a 1x1 texture of a single solid color.
    static Texture *CreateSolidColor(SDL_GPUDevice *device, Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255)
    {
        const unsigned char pixel[4] = {r, g, b, a};
        return new Texture(device, pixel, 1, 1);
    }

    ~Texture()
    {
        if (gpuTexture && owningDevice)
        {
            SDL_ReleaseGPUTexture(owningDevice, gpuTexture);
        }
    }

    int Width() const
    {
        return image_width;
    }

    int Height() const
    {
        return image_height;
    }

    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;

    SDL_GPUTexture *gpuTexture = nullptr;
    int image_width = 0;
    int image_height = 0;

private:
    void UploadPixels(SDL_GPUDevice *device, const unsigned char *pixels, int width, int height)
    {
        owningDevice = device;
        Uint32 imageSizeInBytes = static_cast<Uint32>(width) * static_cast<Uint32>(height) * 4;

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.size = imageSizeInBytes;
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

        SDL_GPUTransferBuffer *textureBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!textureBuffer)
        {
            SDL_Log("Failed to create texture transfer buffer: %s", SDL_GetError());
            return;
        }

        void *textureData = SDL_MapGPUTransferBuffer(device, textureBuffer, false);
        SDL_memcpy(textureData, pixels, imageSizeInBytes);
        SDL_UnmapGPUTransferBuffer(device, textureBuffer);

        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.width = static_cast<Uint32>(width);
        textureInfo.height = static_cast<Uint32>(height);
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

        gpuTexture = SDL_CreateGPUTexture(device, &textureInfo);
        if (!gpuTexture)
        {
            SDL_Log("Failed to create GPU texture: %s", SDL_GetError());
            SDL_ReleaseGPUTransferBuffer(device, textureBuffer);
            return;
        }

        SDL_GPUCommandBuffer *cmdBuffer = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmdBuffer);

        SDL_GPUTextureTransferInfo srcTransfer{};
        srcTransfer.transfer_buffer = textureBuffer;
        srcTransfer.offset = 0;
        srcTransfer.pixels_per_row = static_cast<Uint32>(width);
        srcTransfer.rows_per_layer = static_cast<Uint32>(height);

        SDL_GPUTextureRegion destRegion{};
        destRegion.texture = gpuTexture;
        destRegion.w = static_cast<Uint32>(width);
        destRegion.h = static_cast<Uint32>(height);
        destRegion.d = 1;

        SDL_UploadToGPUTexture(copyPass, &srcTransfer, &destRegion, false);

        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(cmdBuffer);

        SDL_ReleaseGPUTransferBuffer(device, textureBuffer);
    }

    SDL_GPUDevice *owningDevice = nullptr;

    int channels = 0;
};

#endif