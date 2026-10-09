/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BlendSpace.hpp
/// @brief A blend space: clips placed at points on a plane, mixed by where a
///        parameter falls among them. A `.ablnd` file.
///
/// A space whose points lie on one line is a 1D space: the weights between two
/// neighbours on a line are the straight-line mix of the two, whatever the
/// other points do. So idle, walk and run by speed are three points on the x
/// axis, read by the parameter's x.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Errors.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief What a blend space file's sidecar names as its kind.
inline constexpr std::string_view kBlendSpaceKindName = "blend space";

inline constexpr Core::AssetKindId kBlendSpaceKind{kBlendSpaceKindName};

/// @brief Two points closer than this are one position, and a space cannot
///        tell their clips apart.
inline constexpr float kBlendPointSpacing = 1e-4f;

/// @brief One clip in a blend space, and where it plays at full weight.
ASTRUCT()
struct BlendPoint
{
    AFIELD() Core::AssetId Clip;
    AFIELD() glm::vec2 Position{0.f, 0.f};
};

/// @brief The clips a space mixes. Weights come from the parameter's distance
///        to each point; see Geometry::BlendWeights.
AASSET()
struct BlendSpace
{
    AFIELD() std::vector<BlendPoint> Points;
};

/// @brief Why a blend space file was refused.
enum class BlendSpaceError : uint8_t
{
    NotADocument,   ///< Not a blend space document.
    NoPoints,       ///< Nothing to play.
    PointHasNoClip, ///< A point names no clip, so the space would never play.
    BadPosition,    ///< A position is not a finite number.
    SharedPosition, ///< Two points at one position.
    Count_,
};

/// @brief A short description, for the line a failed cook or load prints.
[[nodiscard]] std::string_view ToString(BlendSpaceError error) noexcept;

/// @brief @p error as an asset error: always a corrupt file.
[[nodiscard]] Core::AssetError ToAssetError(BlendSpaceError error) noexcept;

/// @brief The blend space a file holds, if it is one a player can play.
///
/// Both the cook and the load read through this, so a file the cook passed
/// cannot fail to load, and one written into a package by hand is still checked.
[[nodiscard]] std::expected<BlendSpace, BlendSpaceError> ReadBlendSpace(std::span<const std::byte> bytes);

} // namespace Assisi::Geometry
