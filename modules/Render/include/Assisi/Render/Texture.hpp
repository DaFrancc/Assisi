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
/// @brief How a texture's stored 8-bit values map to the linear values shaders
/// work in. Colour/albedo maps are authored in sRGB (Srgb) so the GPU decodes
/// them to linear at sample time — which also makes hardware filtering and mip
/// generation correct, since both must happen in linear space. Data maps
/// (normals, metallic/roughness, masks) hold linear values already and must use
/// Linear so they aren't gamma-mangled.
enum class ColorSpace : std::uint8_t
{
    Srgb,
    Linear,
};

/// @brief Owner of an NVRHI 2D texture, RGBA8, with a full mip chain.
class Texture
{
  public:
    Texture() = default;

    /// @brief Loads an image from a virtual asset path (via stb_image) and uploads it.
    ///
    /// Always decoded to 4 channels (RGBA8). Loaded top-down, matching Vulkan/NVRHI's
    /// UV convention (V=0 at the top) — unlike the OpenGL texture loader this replaces,
    /// no vertical flip is applied. A full mip chain is generated on the CPU (with
    /// stb's colour-space-aware resizer) so distant surfaces don't alias; @p colorSpace
    /// selects the on-GPU format (SRGBA8 for albedo, RGBA8 for data maps) and the
    /// filtering space used to build the mips.
    ///
    /// @return Success, or an AssetError if the file cannot be resolved/read/decoded.
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(nvrhi::IDevice *device, std::string_view vpath,
                                                                  ColorSpace colorSpace = ColorSpace::Srgb) noexcept;

    /// @brief Uploads a solid 1x1 color — used for default/placeholder textures.
    /// Single mip level (nothing to downsample).
    void UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                          unsigned char a, ColorSpace colorSpace = ColorSpace::Srgb,
                          const char *debugName = nullptr);

    nvrhi::ITexture *NativeTexture() const { return _texture; }
    bool IsValid() const { return _texture != nullptr; }

    /// @brief Sentinel for "not yet registered in a bindless descriptor table".
    static constexpr uint32_t kInvalidBindlessIndex = UINT32_MAX;

    /// @brief This texture's slot in the AssetCache's bindless descriptor table
    /// (GPU-driven stage D). Assigned once by the cache at resolve time; shared
    /// textures keep one slot. kInvalidBindlessIndex until registered.
    uint32_t BindlessIndex() const { return _bindlessIndex; }
    void SetBindlessIndex(uint32_t index) { _bindlessIndex = index; }

  private:
    nvrhi::TextureHandle _texture;
    uint32_t             _bindlessIndex = kInvalidBindlessIndex;
};
} /* namespace Assisi::Render */
