/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide stb_image / stb_image_resize function bodies in exactly this
   translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <Assisi/Render/Texture.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstddef>
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

/* Builds `image.mips` from a base RGBA8 image: mip 0 is the base itself, and when
   `generateMips` is set the rest of the chain is filled by downsampling on the
   CPU. stb's resizer is colour-space-aware — sRGB is filtered in linear space
   (RGB treated as sRGB, alpha as linear), which is the whole point of generating
   mips from an sRGB source correctly. Resizing from the base (not the previous
   level) each time avoids compounding filter error. Runs off the base pointer
   only; no device, so it is thread-safe (the worker half of async loading). */
void BuildMipChain(DecodedImage &image, const unsigned char *base, bool generateMips)
{
    const uint32_t width     = image.width;
    const uint32_t height    = image.height;
    const uint32_t mipLevels = generateMips ? MipLevelCount(width, height) : 1;

    image.mips.resize(mipLevels);
    image.mips[0].assign(base, base + static_cast<size_t>(width) * height * kChannels);

    for (uint32_t level = 1; level < mipLevels; ++level)
    {
        const uint32_t mipWidth  = std::max(1u, width >> level);
        const uint32_t mipHeight = std::max(1u, height >> level);

        std::vector<unsigned char> mip(static_cast<size_t>(mipWidth) * mipHeight * kChannels);

        const int inStride  = static_cast<int>(width * kChannels);
        const int outStride = static_cast<int>(mipWidth * kChannels);

        const unsigned char *ok =
            image.colorSpace == ColorSpace::Srgb
                ? stbir_resize_uint8_srgb(base, static_cast<int>(width), static_cast<int>(height), inStride, mip.data(),
                                          static_cast<int>(mipWidth), static_cast<int>(mipHeight), outStride, STBIR_RGBA)
                : stbir_resize_uint8_linear(base, static_cast<int>(width), static_cast<int>(height), inStride,
                                            mip.data(), static_cast<int>(mipWidth), static_cast<int>(mipHeight),
                                            outStride, STBIR_RGBA);
        if (ok == nullptr)
        {
            // Don't leave a hole: UploadDecoded skips empty levels, but the GPU
            // texture is still created with that many mips and createTexture does
            // not zero its memory, so an unwritten level samples whatever the
            // driver had there. Truncate the chain instead — the texture then has
            // only the levels we actually wrote, and sampling falls back to the
            // coarsest real one.
            Assisi::Core::Log::Warn("Texture: mip level {} downsample failed; truncating the chain to {} level(s)",
                                    level, level);
            image.mips.resize(level);
            return;
        }
        image.mips[level] = std::move(mip);
    }
}
} // namespace

std::expected<DecodedImage, Assisi::Core::AssetError> Texture::DecodeImage(std::string_view vpath,
                                                                           ColorSpace colorSpace) noexcept
{
    const auto resolved = Assisi::Core::AssetSystem::Resolve(vpath);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char *data     = stbi_load(resolved->string().c_str(), &width, &height, &channels, 4);
    if (data == nullptr)
    {
        Assisi::Core::Log::Error("Texture: stbi_load failed for '{}'", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }

    DecodedImage image;
    image.width      = static_cast<uint32_t>(width);
    image.height     = static_cast<uint32_t>(height);
    image.colorSpace = colorSpace;
    BuildMipChain(image, data, /*generateMips=*/true);

    stbi_image_free(data);
    return image;
}

std::expected<std::vector<DecodedImage>, Assisi::Core::AssetError>
Texture::DecodeAnimatedWebp(std::string_view vpath, ColorSpace colorSpace) noexcept
{
    const std::expected<std::vector<std::byte>, Assisi::Core::AssetError> bytes =
        Assisi::Core::AssetSystem::ReadBinary(vpath);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }

    WebPData webpData;
    webpData.bytes = reinterpret_cast<const uint8_t *>(bytes->data());
    webpData.size  = bytes->size();

    // AnimDecoder composites frame disposal/blending for us and hands back full
    // 256x256-ish RGBA canvases — exactly what we upload. MODE_RGBA matches our
    // top-down RGBA8 convention (WebP rows are top-down like Vulkan's V=0).
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options))
    {
        Assisi::Core::Log::Error("Texture: WebPAnimDecoderOptionsInit failed for '{}'", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }
    options.color_mode  = MODE_RGBA;
    options.use_threads = 0;

    WebPAnimDecoder *decoder = WebPAnimDecoderNew(&webpData, &options);
    if (decoder == nullptr)
    {
        Assisi::Core::Log::Error("Texture: WebPAnimDecoderNew failed for '{}' (not a valid WebP?)", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }

    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(decoder, &info))
    {
        Assisi::Core::Log::Error("Texture: WebPAnimDecoderGetInfo failed for '{}'", vpath);
        WebPAnimDecoderDelete(decoder);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }

    std::vector<DecodedImage> frames;
    frames.reserve(info.frame_count);
    while (WebPAnimDecoderHasMoreFrames(decoder))
    {
        uint8_t *frameRgba = nullptr; // owned by the decoder; valid only until the next call
        int      timestamp = 0;       // milliseconds (unused: playback speed is set by the caller)
        if (!WebPAnimDecoderGetNext(decoder, &frameRgba, &timestamp))
        {
            Assisi::Core::Log::Warn("Texture: WebPAnimDecoderGetNext failed at frame {} of '{}'", frames.size(), vpath);
            break; // keep whatever decoded cleanly rather than dropping the whole animation
        }

        DecodedImage image;
        image.width      = info.canvas_width;
        image.height     = info.canvas_height;
        image.colorSpace = colorSpace;
        // No mip chain: the spinner is drawn near its native size, and a chain per
        // frame would multiply the (already many) uploads for no visible gain.
        BuildMipChain(image, frameRgba, /*generateMips=*/false);
        frames.push_back(std::move(image));
    }
    WebPAnimDecoderDelete(decoder);

    if (frames.empty())
    {
        Assisi::Core::Log::Error("Texture: decoded no frames from WebP '{}'", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }
    return frames;
}

nvrhi::TextureHandle Texture::CreateImage(nvrhi::IDevice *device, const DecodedImage &image, const char *debugName)
{
    nvrhi::TextureDesc desc;
    desc.width            = image.width;
    desc.height           = image.height;
    desc.format           = FormatFor(image.colorSpace);
    desc.mipLevels        = static_cast<uint32_t>(image.mips.size());
    desc.debugName        = debugName != nullptr ? debugName : "Texture";
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return device->createTexture(desc);
}

void Texture::RecordMips(nvrhi::ICommandList *commandList, nvrhi::ITexture *texture, const DecodedImage &image)
{
    const uint32_t mipLevels = static_cast<uint32_t>(image.mips.size());
    for (uint32_t level = 0; level < mipLevels; ++level)
    {
        if (image.mips[level].empty())
        {
            continue; // a downsample that failed in BuildMipChain
        }
        const uint32_t mipWidth = std::max(1u, image.width >> level);
        commandList->writeTexture(texture, 0, level, image.mips[level].data(),
                                  static_cast<size_t>(mipWidth) * kChannels);
    }
}

void Texture::UploadDecoded(nvrhi::IDevice *device, const DecodedImage &image, const char *debugName,
                            nvrhi::ICommandList *sharedList)
{
    _texture = CreateImage(device, image, debugName);

    // Record into the caller's shared list when given one (it opens/closes/executes
    // and batches many uploads into a single submit); otherwise run self-contained.
    nvrhi::CommandListHandle ownList;
    nvrhi::ICommandList     *commandList = sharedList;
    if (commandList == nullptr)
    {
        ownList     = device->createCommandList();
        commandList = ownList;
        commandList->open();
    }

    RecordMips(commandList, _texture, image);

    if (ownList != nullptr)
    {
        ownList->close();
        device->executeCommandList(ownList);
    }
}

std::expected<void, Assisi::Core::AssetError> Texture::LoadFromAssets(nvrhi::IDevice *device, std::string_view vpath,
                                                                       ColorSpace colorSpace) noexcept
{
    std::expected<DecodedImage, Assisi::Core::AssetError> image = DecodeImage(vpath, colorSpace);
    if (!image)
    {
        return std::unexpected(image.error());
    }
    UploadDecoded(device, *image, std::string(vpath).c_str());
    return {};
}

void Texture::UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                               unsigned char a, ColorSpace colorSpace, const char *debugName,
                               nvrhi::ICommandList *sharedList)
{
    DecodedImage image;
    image.width      = 1;
    image.height     = 1;
    image.colorSpace = colorSpace;
    image.mips.push_back({r, g, b, a}); // single 1x1 level — nothing to downsample
    UploadDecoded(device, image, debugName, sharedList);
}

} // namespace Assisi::Render
