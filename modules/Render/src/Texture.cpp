/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide stb_image / stb_image_resize function bodies in exactly this
   translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <Assisi/Render/Texture.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Assisi::Render
{

namespace
{
/* Every texture is decoded/stored as 4-channel RGBA8. */
constexpr uint32_t kChannels = 4;

nvrhi::Format FormatFor(ColorSpace colorSpace)
{
    return colorSpace == ColorSpace::Srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
}

/* Number of mip levels in a full chain down to 1x1. */
uint32_t MipLevelCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while (width > 1 || height > 1)
    {
        width  = std::max(1u, width >> 1);
        height = std::max(1u, height >> 1);
        ++levels;
    }
    return levels;
}

/* Uploads `pixels` (RGBA8, width x height) as mip 0 and, when `generateMips` is
   set, fills the rest of the chain by downsampling on the CPU. stb's resizer is
   colour-space-aware — sRGB textures are filtered in linear space (RGB treated
   as sRGB, alpha as linear), which is the whole point of generating mips from an
   sRGB source correctly. All levels ship in one command list. */
void UploadTexture(nvrhi::IDevice *device, nvrhi::TextureHandle &texture, const unsigned char *pixels, uint32_t width,
                   uint32_t height, ColorSpace colorSpace, bool generateMips, const char *debugName)
{
    const char    *name      = debugName != nullptr ? debugName : "Texture";
    const uint32_t mipLevels = generateMips ? MipLevelCount(width, height) : 1;

    nvrhi::TextureDesc desc;
    desc.width            = width;
    desc.height           = height;
    desc.format           = FormatFor(colorSpace);
    desc.mipLevels        = mipLevels;
    desc.debugName        = name;
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    texture               = device->createTexture(desc);

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();

    /* Base level, straight from the source pixels. */
    commandList->writeTexture(texture, 0, 0, pixels, static_cast<size_t>(width) * kChannels);

    /* Remaining levels: downsample the base image directly to each level's size.
       Resizing from the base (not the previous level) each time avoids
       compounding filter error across the chain. */
    for (uint32_t level = 1; level < mipLevels; ++level)
    {
        const uint32_t mipWidth  = std::max(1u, width >> level);
        const uint32_t mipHeight = std::max(1u, height >> level);

        std::vector<unsigned char> mip(static_cast<size_t>(mipWidth) * mipHeight * kChannels);

        const int inStride  = static_cast<int>(width) * kChannels;
        const int outStride = static_cast<int>(mipWidth) * kChannels;

        const unsigned char *ok =
            colorSpace == ColorSpace::Srgb
                ? stbir_resize_uint8_srgb(pixels, static_cast<int>(width), static_cast<int>(height), inStride,
                                          mip.data(), static_cast<int>(mipWidth), static_cast<int>(mipHeight),
                                          outStride, STBIR_RGBA)
                : stbir_resize_uint8_linear(pixels, static_cast<int>(width), static_cast<int>(height), inStride,
                                            mip.data(), static_cast<int>(mipWidth), static_cast<int>(mipHeight),
                                            outStride, STBIR_RGBA);
        if (ok == nullptr)
        {
            Assisi::Core::Log::Warn("Texture \"{}\": mip level {} downsample failed; skipping it", name, level);
            continue;
        }

        commandList->writeTexture(texture, 0, level, mip.data(), static_cast<size_t>(mipWidth) * kChannels);
    }

    commandList->close();
    device->executeCommandList(commandList);
}
} // namespace

std::expected<void, Assisi::Core::AssetError> Texture::LoadFromAssets(nvrhi::IDevice *device, std::string_view vpath,
                                                                       ColorSpace colorSpace) noexcept
{
    const auto resolved = Assisi::Core::AssetSystem::Resolve(vpath);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data = stbi_load(resolved->string().c_str(), &width, &height, &channels, 4);
    if (data == nullptr)
    {
        Assisi::Core::Log::Error("Texture: stbi_load failed for '{}'", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }

    UploadTexture(device, _texture, data, static_cast<uint32_t>(width), static_cast<uint32_t>(height), colorSpace,
                  /*generateMips=*/true, std::string(vpath).c_str());

    stbi_image_free(data);
    return {};
}

void Texture::UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                               unsigned char a, ColorSpace colorSpace, const char *debugName)
{
    const unsigned char pixel[4] = {r, g, b, a};
    UploadTexture(device, _texture, pixel, 1, 1, colorSpace, /*generateMips=*/false, debugName);
}

} // namespace Assisi::Render
