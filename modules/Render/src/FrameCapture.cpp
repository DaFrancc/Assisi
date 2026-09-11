/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/FrameCapture.hpp>

#include <Assisi/Core/Logger.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstddef>
#include <vector>

namespace Assisi::Render
{
namespace
{
/// Bytes per pixel of the 8-bit RGBA formats this reads, and of the PNG it writes.
constexpr std::size_t kChannels = 4;

/// Written into every pixel's alpha: a screenshot carrying the scene's alpha reads
/// as transparent in every viewer that honours it.
constexpr std::uint8_t kOpaque = 255;

/// Whether a format stores blue in the first byte. Swapchains come in both orders.
[[nodiscard]] bool IsBlueFirst(nvrhi::Format format)
{
    return format == nvrhi::Format::BGRA8_UNORM || format == nvrhi::Format::SBGRA8_UNORM;
}
} // namespace

bool FrameCapture::Record(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, nvrhi::ITexture *source)
{
    if (device == nullptr || commandList == nullptr || source == nullptr)
    {
        return false;
    }

    const nvrhi::TextureDesc &sourceDesc = source->getDesc();
    nvrhi::TextureDesc stagingDesc = sourceDesc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isShaderResource = false;
    stagingDesc.isUAV = false;
    stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "FrameCapture::Staging";

    _staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (_staging == nullptr)
    {
        Core::Log::Error("FrameCapture: could not allocate a {}x{} staging texture.", sourceDesc.width,
                         sourceDesc.height);
        return false;
    }
    _width = sourceDesc.width;
    _height = sourceDesc.height;
    _format = sourceDesc.format;
    commandList->copyTexture(_staging, nvrhi::TextureSlice(), source, nvrhi::TextureSlice());
    return true;
}

bool FrameCapture::Write(nvrhi::IDevice *device, const std::string &path)
{
    if (device == nullptr || _staging == nullptr || path.empty())
    {
        Core::Log::Error("FrameCapture: nothing was recorded to write to '{}'.", path);
        return false;
    }

    device->waitForIdle();

    std::size_t rowPitch = 0;
    const auto *const mapped = static_cast<const std::uint8_t *>(
        device->mapStagingTexture(_staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mapped == nullptr)
    {
        Core::Log::Error("FrameCapture: could not map the staging texture.");
        return false;
    }

    // Repacked row by row: the driver's row pitch is routinely wider than the image.
    const auto width = static_cast<std::size_t>(_width);
    const auto height = static_cast<std::size_t>(_height);
    const std::size_t red = IsBlueFirst(_format) ? 2u : 0u;
    const std::size_t blue = 2u - red;
    std::vector<std::uint8_t> pixels(width * height * kChannels);
    for (std::size_t y = 0; y < height; ++y)
    {
        const std::uint8_t *const row = mapped + y * rowPitch;
        std::uint8_t *const out = pixels.data() + y * width * kChannels;
        for (std::size_t x = 0; x < width; ++x)
        {
            out[x * kChannels + 0u] = row[x * kChannels + red];
            out[x * kChannels + 1u] = row[x * kChannels + 1u];
            out[x * kChannels + 2u] = row[x * kChannels + blue];
            out[x * kChannels + 3u] = kOpaque;
        }
    }
    device->unmapStagingTexture(_staging);
    _staging = nullptr;

    const int written = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                                       static_cast<int>(kChannels), pixels.data(), static_cast<int>(width * kChannels));
    if (written == 0)
    {
        Core::Log::Error("FrameCapture: could not write '{}'.", path);
        return false;
    }

    Core::Log::Info("FrameCapture: wrote {} ({}x{}).", path, width, height);
    return true;
}

} // namespace Assisi::Render
