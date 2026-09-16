/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

// Texture::LoadFromAssets alone, apart from the rest of Texture: it decodes an
// image file from the source tree, and keeping it in its own object is what lets
// an executable that never calls it — a shipped game — link Texture without the
// image decoder.

#include <Assisi/Render/Texture.hpp>

#include <Assisi/Image/Decode.hpp>

#include <string>

namespace Assisi::Render
{

std::expected<void, Assisi::Core::AssetError> Texture::LoadFromAssets(nvrhi::IDevice *device, std::string_view vpath,
                                                                      Image::ColorSpace colorSpace) noexcept
{
    std::expected<Image::DecodedImage, Assisi::Core::AssetError> image = Image::DecodeImage(vpath, colorSpace);
    if (!image)
    {
        return std::unexpected(image.error());
    }
    UploadDecoded(device, *image, std::string(vpath).c_str());
    return {};
}

} // namespace Assisi::Render
