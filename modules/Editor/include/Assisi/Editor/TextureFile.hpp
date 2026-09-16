/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TextureFile.hpp
/// @brief Uploading an image file from the source tree as a texture: for editor
///        chrome and previews, which draw the files an author has.

#include <expected>
#include <string_view>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/Errors.hpp>
#include <Assisi/Image/Image.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Editor
{

/// @brief Decode the image at @p vpath and upload it into @p texture, uncompressed,
///        on the calling thread.
///
/// @p colorSpace selects the on-GPU format and the space the mips are filtered in.
///
/// @return Success, or an AssetError if the file cannot be resolved, read or decoded.
std::expected<void, Core::AssetError> LoadTextureFile(Render::Texture &texture, nvrhi::IDevice *device,
                                                      std::string_view vpath,
                                                      Image::ColorSpace colorSpace = Image::ColorSpace::Srgb);

} // namespace Assisi::Editor
