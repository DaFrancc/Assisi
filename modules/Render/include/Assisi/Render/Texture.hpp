/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Texture.hpp
/// @brief GPU-side 2D texture storage backed by an NVRHI texture.

#include <cstdint>
#include <expected>
#include <string_view>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetSystem.hpp>

namespace Assisi::Render
{
/// @brief Owner of an NVRHI 2D texture, single mip level, RGBA8.
class Texture
{
  public:
    Texture() = default;

    /// @brief Loads an image from a virtual asset path (via stb_image) and uploads it.
    ///
    /// Always decoded to 4 channels (RGBA8). Loaded top-down, matching Vulkan/NVRHI's
    /// UV convention (V=0 at the top) — unlike the OpenGL texture loader this replaces,
    /// no vertical flip is applied.
    ///
    /// @return Success, or an AssetError if the file cannot be resolved/read/decoded.
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(nvrhi::IDevice *device,
                                                                  std::string_view vpath) noexcept;

    /// @brief Uploads a solid 1x1 color — used for default/placeholder textures.
    void UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                          unsigned char a, const char *debugName = nullptr);

    nvrhi::ITexture *NativeTexture() const { return _texture; }
    bool IsValid() const { return _texture != nullptr; }

  private:
    nvrhi::TextureHandle _texture;
};
} /* namespace Assisi::Render */
