/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetSource.hpp
/// @brief Where the renderer's assets come from, as decoded values.
///
/// The editor works on the source tree — glTF, PNG and `.amat` text — and a
/// shipped game on cooked blobs in a pak. What the renderer needs from either is
/// the same: a mesh's arrays, a texture's mips, a material's factors, a shader's
/// SPIR-V. So the renderer asks this interface, and each executable installs the
/// implementation that matches the files it has. Only the one installed is linked
/// in, which is what keeps the source decoders out of a shipped game.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/CookedMesh.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Render
{

/// @brief Why an asset did not load.
enum class AssetLoadError : std::uint8_t
{
    UnknownAsset, ///< No asset has this id or path.
    Unreadable,   ///< The asset exists and its bytes could not be read.
    Undecodable,  ///< The bytes were read and are not the kind of asset asked for.
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(AssetLoadError error) noexcept;

/// @brief Decoded assets by id. Every call may run on a worker thread.
class AssetSource
{
public:
    virtual ~AssetSource() = default;

    /// @brief A mesh's arrays, tables and bounds, and the material each slot draws with.
    [[nodiscard]] virtual std::expected<Geometry::CookedMesh, AssetLoadError> LoadMesh(const Core::AssetId &id) const = 0;

    /// @brief A material's factors and the ids of its channel textures.
    [[nodiscard]] virtual std::expected<Geometry::MaterialData, AssetLoadError>
    LoadMaterial(const Core::AssetId &id) const = 0;

    /// @brief A texture's mips, for a channel that samples it in @p space as @p format.
    ///
    /// A source that holds textures already encoded returns them in the format
    /// they were encoded in; one decoding from an image file encodes to @p format.
    [[nodiscard]] virtual std::expected<Image::DecodedImage, AssetLoadError>
    LoadTexture(const Core::AssetId &id, Image::ColorSpace space, Image::PixelFormat format) const = 0;

    /// @brief The SPIR-V for a shader stage named by virtual path
    ///        ("shaders/mesh.vert.spv").
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, AssetLoadError>
    LoadShader(std::string_view vpath) const = 0;

    /// @brief The id of the asset at @p vpath.
    [[nodiscard]] virtual std::expected<Core::AssetId, AssetLoadError> Resolve(std::string_view vpath) const = 0;

    /// @brief How to name @p id in a log line: its path where one is known, else the id.
    [[nodiscard]] virtual std::string Describe(const Core::AssetId &id) const = 0;
};

/// @brief Install the source the renderer loads through, returning the one it
///        replaces. The caller keeps @p source alive while it is installed.
///
/// Install before the renderer or any cache starts: it is read without a lock.
const AssetSource *SetAssetSource(const AssetSource *source);

/// @brief The installed source, or null before one is installed.
[[nodiscard]] const AssetSource *GetAssetSource();

} // namespace Assisi::Render
