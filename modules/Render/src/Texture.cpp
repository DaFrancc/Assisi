/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Texture.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Render
{

namespace
{
/* Uploads refused for a malformed mip chain. Atomic because a decode worker may
   create a texture while the main thread publishes another. */
std::atomic<std::uint32_t> gUploadFailures{0};

/* Edge of one square in the error checkerboard, in texels. */
constexpr std::uint32_t kErrorPatternSquare = 8;

/* Texels along each edge of the error checkerboard.
   Sixteen squares to a side: the pattern is stretched across whatever UVs the
   broken material had, so a coarse one covers a whole surface in a single square
   and reads as flat magenta — which looks like a colour somebody chose rather
   than a texture that failed. */
constexpr std::uint32_t kErrorPatternExtent = kErrorPatternSquare * 16;

/* The two colours of the error checkerboard. Magenta against black because
   nothing authored looks like it. */
constexpr std::array<unsigned char, 4> kErrorPatternMagenta{255, 0, 255, 255};
constexpr std::array<unsigned char, 4> kErrorPatternBlack{0, 0, 0, 255};
} // namespace

nvrhi::Format NvrhiFormatFor(Image::PixelFormat format, Image::ColorSpace colorSpace)
{
    const bool srgb = colorSpace == Image::ColorSpace::Srgb;
    switch (format)
    {
    // The single- and two-channel formats carry data, so there is no gamma to
    // undo and no sRGB enumerator to pick.
    case Image::PixelFormat::Bc4:
        return nvrhi::Format::BC4_UNORM;
    case Image::PixelFormat::Bc5:
        return nvrhi::Format::BC5_UNORM;
    case Image::PixelFormat::Bc7:
        return srgb ? nvrhi::Format::BC7_UNORM_SRGB : nvrhi::Format::BC7_UNORM;
    case Image::PixelFormat::Rgba8:
        return srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    case Image::PixelFormat::R8:
        return nvrhi::Format::R8_UNORM;
    // A format with no mapping would otherwise be uploaded as whatever the
    // fallback happens to be, which samples as noise rather than failing.
    default:
        ASSISI_ASSERT(false, "NvrhiFormatFor reached a format with no GPU equivalent");
        Core::Log::Error("Texture: no GPU format for this pixel format; falling back to RGBA8");
        return srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    }
}

std::uint32_t Texture::UploadFailureCount()
{
    return gUploadFailures.load(std::memory_order_relaxed);
}

void Texture::ResetUploadFailureCount()
{
    gUploadFailures.store(0, std::memory_order_relaxed);
}

nvrhi::TextureHandle Texture::CreateImage(nvrhi::IDevice *device, const Image::DecodedImage &image,
                                          const char *debugName)
{
    if (!Image::ValidateMipChain(image))
    {
        gUploadFailures.fetch_add(1, std::memory_order_relaxed);
        Core::Log::Warn("Texture: '{}' has a mip chain that does not match its format; not uploading",
                        debugName != nullptr ? debugName : "<unnamed>");
        return nullptr;
    }

    nvrhi::TextureDesc desc;
    desc.width            = image.width;
    desc.height           = image.height;
    desc.format           = NvrhiFormatFor(image.format, image.colorSpace);
    desc.mipLevels        = static_cast<std::uint32_t>(image.mips.size());
    desc.debugName        = debugName != nullptr ? debugName : "Texture";
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return device->createTexture(desc);
}

void Texture::RecordMips(nvrhi::ICommandList *commandList, nvrhi::ITexture *texture, const Image::DecodedImage &image)
{
    const std::uint32_t mipLevels = static_cast<std::uint32_t>(image.mips.size());
    for (std::uint32_t level = 0; level < mipLevels; ++level)
    {
        if (image.mips[level].empty())
        {
            continue; // a downsample that failed while the chain was built
        }
        const Image::LevelLayout layout = Image::LayoutFor(image.format, Image::MipExtent(image.width, level),
                                                           Image::MipExtent(image.height, level));
        commandList->writeTexture(texture, 0, level, image.mips[level].data(), layout.rowPitch);
    }
}

void Texture::UploadDecoded(nvrhi::IDevice *device, const Image::DecodedImage &image, const char *debugName,
                            nvrhi::ICommandList *sharedList)
{
    _texture = CreateImage(device, image, debugName);
    if (_texture == nullptr)
    {
        return;
    }

    // Record into the caller's shared list when given one (it opens/closes/executes
    // and batches many uploads into a single submit); otherwise run self-contained.
    nvrhi::CommandListHandle ownList;
    nvrhi::ICommandList *commandList = sharedList;
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

void Texture::UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                               unsigned char a, Image::ColorSpace colorSpace, const char *debugName,
                               nvrhi::ICommandList *sharedList)
{
    Image::DecodedImage image;
    image.width      = 1;
    image.height     = 1;
    image.format     = Image::PixelFormat::Rgba8;
    image.colorSpace = colorSpace;
    image.mips.push_back({r, g, b, a}); // single 1x1 level — nothing to downsample
    UploadDecoded(device, image, debugName, sharedList);
}

void Texture::UploadErrorPattern(nvrhi::IDevice *device, const char *debugName, nvrhi::ICommandList *sharedList)
{
    Image::DecodedImage image;
    image.width  = kErrorPatternExtent;
    image.height = kErrorPatternExtent;
    image.format = Image::PixelFormat::Rgba8;
    // Linear, so the pattern is the same two colours whatever channel it stands in
    // for — this marks a failure rather than carrying content to be read.
    image.colorSpace = Image::ColorSpace::Linear;

    image.mips.resize(1);
    image.mips[0].resize(Image::LayoutFor(Image::PixelFormat::Rgba8, image.width, image.height).byteSize);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const bool magenta = ((x / kErrorPatternSquare) + (y / kErrorPatternSquare)) % 2 == 0;
            const std::array<unsigned char, 4> &colour = magenta ? kErrorPatternMagenta : kErrorPatternBlack;

            const std::size_t texel =
                (static_cast<std::size_t>(y) * image.width + x) * Image::kRgba8BytesPerTexel;
            std::copy(colour.begin(), colour.end(), image.mips[0].begin() + static_cast<std::ptrdiff_t>(texel));
        }
    }
    UploadDecoded(device, image, debugName != nullptr ? debugName : "TextureError", sharedList);
}

} // namespace Assisi::Render
