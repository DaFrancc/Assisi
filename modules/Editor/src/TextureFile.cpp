/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/TextureFile.hpp>

#include <Assisi/Image/Decode.hpp>

#include <string>

namespace Assisi::Editor
{

std::expected<void, Core::AssetError> LoadTextureFile(Render::Texture &texture, nvrhi::IDevice *device,
                                                      std::string_view vpath, Image::ColorSpace colorSpace)
{
    std::expected<Image::DecodedImage, Core::AssetError> image = Image::DecodeImage(vpath, colorSpace);
    if (!image)
    {
        return std::unexpected(image.error());
    }
    texture.UploadDecoded(device, *image, std::string(vpath).c_str());
    return {};
}

} // namespace Assisi::Editor
