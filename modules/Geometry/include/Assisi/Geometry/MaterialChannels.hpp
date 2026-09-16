/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MaterialChannels.hpp
/// @brief A material's texture channels, and the on-GPU format each one wants.
///
/// A texture's compressed format follows from the channel that binds it, not
/// from the file: the same pixels are BC7 in sRGB as a colour map and BC5 in
/// linear as a normal map, because one carries light and the other carries a
/// direction. So the table is per channel, and it lives here — beside the
/// material that declares the channels — rather than inside the renderer.
///
/// **Two consumers, one table.** The cooker compresses offline and the render
/// AssetCache compresses on load, and they must reach the same answer for the
/// same channel or a cooked texture is not the one the sampler was built for.
/// Two copies of this would agree by coincidence and drift by ordinary edits.

#include <array>
#include <cstddef>
#include <cstdint>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Geometry
{

/// @brief The texture slots a material binds, in the order the renderer's
///        constant block lists them.
enum class MaterialChannel : std::uint8_t
{
    BaseColor,
    Normal,
    MetallicRoughness,
    Occlusion,
    Emissive,
    Count,
};

/// @brief What a channel's texture is on the GPU.
struct MaterialChannelFormat
{
    Image::PixelFormat format;  ///< The block format, before any device or knob narrows it.
    Image::ColorSpace space;    ///< Whether the hardware undoes gamma at sample time.
};

/// @brief The format and colour space @p channel's texture is stored in.
///
/// BC5 for normals and BC7 for the rest. A normal map's two stored channels
/// reconstruct Z in the shader, which is why it is the one slot that is not a
/// three-channel colour format; everything else carries colour or a packed
/// scalar set that BC7 fits without a second rule.
///
/// sRGB only where the content is light the eye sees — base colour and
/// emissive. A normal, a roughness, a metalness and an occlusion value are all
/// data, and decoding gamma out of them would change the number the shader
/// reads.
[[nodiscard]] constexpr MaterialChannelFormat FormatFor(MaterialChannel channel)
{
    switch (channel)
    {
    case MaterialChannel::BaseColor:
        return {Image::PixelFormat::Bc7, Image::ColorSpace::Srgb};
    case MaterialChannel::Normal:
        return {Image::PixelFormat::Bc5, Image::ColorSpace::Linear};
    case MaterialChannel::MetallicRoughness:
        return {Image::PixelFormat::Bc7, Image::ColorSpace::Linear};
    case MaterialChannel::Occlusion:
        return {Image::PixelFormat::Bc7, Image::ColorSpace::Linear};
    case MaterialChannel::Emissive:
        return {Image::PixelFormat::Bc7, Image::ColorSpace::Srgb};
    // A channel added to the enum and not to this switch would otherwise be
    // cooked as a colour map and sampled as whatever it actually is.
    default:
        return {Image::PixelFormat::Rgba8, Image::ColorSpace::Linear};
    }
}

/// @brief The texture @p material binds to @p channel, or a nil id when the
///        channel is factor-only.
[[nodiscard]] constexpr Core::AssetId ChannelTexture(const MaterialData &material, MaterialChannel channel)
{
    switch (channel)
    {
    case MaterialChannel::BaseColor:
        return material.BaseColorTexture;
    case MaterialChannel::Normal:
        return material.NormalTexture;
    case MaterialChannel::MetallicRoughness:
        return material.MetallicRoughnessTexture;
    case MaterialChannel::Occlusion:
        return material.OcclusionTexture;
    case MaterialChannel::Emissive:
        return material.EmissiveTexture;
    // As above: a channel with no accessor reads as factor-only, which silently
    // drops whatever texture the material bound to it.
    default:
        return {};
    }
}

/// @brief A short human-readable name for a channel — what a cook failure names
///        when two materials disagree about one texture.
[[nodiscard]] constexpr std::string_view ToString(MaterialChannel channel)
{
    switch (channel)
    {
    case MaterialChannel::BaseColor:
        return "BaseColor";
    case MaterialChannel::Normal:
        return "Normal";
    case MaterialChannel::MetallicRoughness:
        return "MetallicRoughness";
    case MaterialChannel::Occlusion:
        return "Occlusion";
    case MaterialChannel::Emissive:
        return "Emissive";
    default:
        return "unknown";
    }
}

/// @brief Every channel, for a caller that walks all of them.
inline constexpr std::array<MaterialChannel, static_cast<std::size_t>(MaterialChannel::Count)> kMaterialChannels{
    MaterialChannel::BaseColor, MaterialChannel::Normal, MaterialChannel::MetallicRoughness,
    MaterialChannel::Occlusion, MaterialChannel::Emissive};

} // namespace Assisi::Geometry
