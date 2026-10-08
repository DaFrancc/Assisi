/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GltfAnimation.hpp
/// @brief A glTF animation read into a clip, and a clip written back out as a
///        `.glb` of its own. Internal to Geometry: both speak fastgltf's types.

#include <cstddef>
#include <expected>
#include <vector>

#include <fastgltf/types.hpp>

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/AnimationFile.hpp>

namespace Assisi::Geometry
{

/// @brief A clip read from a glTF, and the node each of its tracks came from.
struct GltfClip
{
    AnimationClip clip;
    std::vector<std::size_t> nodeOfTrack; ///< Into the source asset's nodes, one per track.
};

/// @brief @p asset's animation @p index as a clip, its tracks named after the
///        nodes they move.
[[nodiscard]] std::expected<GltfClip, AnimationReadError> ReadGltfAnimation(const fastgltf::Asset &asset,
                                                                            std::size_t index);

/// @brief A `.glb` holding @p clip and, from @p source, the nodes its tracks
///        came from and their ancestors, with their names and rest transforms.
///        Empty when the exporter refuses it.
[[nodiscard]] std::vector<std::byte> WriteClipGlb(const GltfClip &clip, const fastgltf::Asset &source);

} // namespace Assisi::Geometry
