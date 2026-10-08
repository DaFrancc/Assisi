/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationFile.hpp
/// @brief The "animation" asset kind's file: a `.glb` holding one clip and the
///        joints it moves, cooked into the clip's keys and loaded from them.
///
/// Such files come from Geometry::ExtractGltfAnimations, but any `.glb` with one
/// animation and its buffers inside it is one. Cooking is in AnimationImport.hpp,
/// which a shipped game does not link.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/Errors.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>

namespace Assisi::Geometry
{

/// @brief Why a glTF animation cannot become a clip.
enum class AnimationReadError : uint8_t
{
    ParseFailed,     ///< The file is not a glTF the parser can read.
    NotOneClip,      ///< The file holds more or fewer than one animation.
    ExternalBuffer,  ///< The file names a buffer outside itself.
    UnnamedJoint,    ///< The animation moves a node with no name, which no skeleton can match.
    DuplicateJoint,  ///< Two nodes the animation moves share a name.
    CubicSpline,     ///< A channel uses cubic-spline keys, which are not supported yet.
    MorphWeights,    ///< A channel animates morph-target weights, which are not supported yet.
    BadKeys,         ///< Key times do not increase, a value is not finite, or counts disagree.
    NoKeys,          ///< The animation keys nothing.
    Count_,
};

/// @brief A short description, for the line a skipped clip or a failed cook prints.
[[nodiscard]] std::string_view ToString(AnimationReadError error) noexcept;

/// @brief @p error as an asset error: what this build cannot play is
///        UnsupportedEncoding, anything else a corrupt file.
[[nodiscard]] Core::AssetError ToAssetError(AnimationReadError error) noexcept;

/// @brief The clip in a payload CookAnimation wrote.
[[nodiscard]] std::expected<AnimationClip, Core::AssetError> LoadAnimation(std::span<const std::byte> payload);

} // namespace Assisi::Geometry
