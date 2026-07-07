/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide stb_image function bodies in exactly this translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <Assisi/Render/Texture.hpp>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Render
{

namespace
{
void UploadRgba8(nvrhi::IDevice *device, nvrhi::TextureHandle &texture, const unsigned char *pixels, uint32_t width,
                 uint32_t height, const char *debugName)
{
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.debugName = debugName != nullptr ? debugName : "Texture";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    texture = device->createTexture(desc);

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    commandList->writeTexture(texture, 0, 0, pixels, static_cast<size_t>(width) * 4);
    commandList->close();
    device->executeCommandList(commandList);
}
} // namespace

std::expected<void, Assisi::Core::AssetError> Texture::LoadFromAssets(nvrhi::IDevice *device,
                                                                       std::string_view vpath) noexcept
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

    UploadRgba8(device, _texture, data, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
               std::string(vpath).c_str());

    stbi_image_free(data);
    return {};
}

void Texture::UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                               unsigned char a, const char *debugName)
{
    const unsigned char pixel[4] = {r, g, b, a};
    UploadRgba8(device, _texture, pixel, 1, 1, debugName);
}

} // namespace Assisi::Render
