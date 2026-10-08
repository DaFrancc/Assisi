/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationImport.hpp
/// @brief Writes each animation in a glTF out as its own clip file, beside it,
///        and cooks a clip file into the keys the game loads.
///
/// Editor and cook only, so a shipped game never links the glTF parser.
/// Extraction writes into the asset tree, and runs when someone asks for it,
/// never on import.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/Errors.hpp>

namespace Assisi::Geometry
{

/// @brief The cook step: @p glb's one clip, as the payload LoadAnimation reads.
///        The file must hold exactly one animation and keep its buffers inside
///        itself, since a cook step reads only the file's own bytes.
[[nodiscard]] std::expected<std::vector<std::byte>, Core::AssetError> CookAnimation(std::span<const std::byte> glb);

/// @brief Why nothing could be extracted from a glTF.
enum class AnimationExtractError : uint8_t
{
    ReadFailed,   ///< The glTF, or a buffer beside it, could not be read or parsed.
    NoAnimations, ///< The glTF holds no animations.
    WriteFailed,  ///< A clip file or its sidecar could not be written.
    Count_,
};

/// @brief A short description, for the line the editor prints.
[[nodiscard]] std::string_view ToString(AnimationExtractError error) noexcept;

/// @brief How many clips an extraction wrote, and how many it left out because
///        they cannot play here. Each one left out was logged with its reason.
struct ExtractedAnimations
{
    uint32_t written = 0;
    uint32_t skipped = 0;
};

/// @brief Writes every animation in @p gltfVirtualPath as `<animation>.glb` in
///        a `<model>_animations` folder beside it: that animation and the joints
///        it moves, with their rest transforms, and no mesh.
///
/// Each file gets a sidecar naming it an "animation". A file written before is
/// overwritten and keeps its sidecar, so its id, and everything that plays it,
/// stays. Two animations with the same name are told apart by their position in
/// the source. An animation that cannot play here is left out with a warning.
[[nodiscard]] std::expected<ExtractedAnimations, AnimationExtractError>
ExtractGltfAnimations(std::string_view gltfVirtualPath);

} // namespace Assisi::Geometry
