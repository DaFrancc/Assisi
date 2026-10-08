/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationClip.hpp
/// @brief One animation clip: for each joint it moves, keyed rotations,
///        translations and scales over time.
///
/// Joints are named, not numbered, so one clip plays on every skeleton that
/// has joints of those names; Geometry::BindClip turns the names into a
/// skeleton's indices.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief What an "animation" file's sidecar names as its kind.
inline constexpr std::string_view kAnimationKindName = "animation";

inline constexpr Core::AssetKindId kAnimationKind{kAnimationKindName};

/// @brief Version of a cooked clip's layout. Raising it re-cooks every clip.
inline constexpr uint32_t kAnimationPayloadVersion = 1;

/// @brief How a channel moves between two keys.
///
/// Stored per channel in the cooked clip, so a mode with more data per key
/// (cubic tangents) is a new enumerator and a payload version, not a new format.
enum class Interpolation : uint8_t
{
    Linear, ///< Straight between keys; rotations take the shorter arc.
    Step,   ///< Holds each key until the next.
    Count_,
};

/// @brief One property of one joint over time: a value per key time.
template <typename T> struct Channel
{
    std::vector<float> Times; ///< Seconds, strictly increasing.
    std::vector<T> Values;    ///< One per time.
    Interpolation Mode = Interpolation::Linear;

    [[nodiscard]] bool Empty() const { return Times.empty(); }
};

/// @brief What a clip does to one joint. A channel with no keys leaves that
///        property as it was.
struct JointTrack
{
    std::string Joint;
    Channel<glm::quat> Rotation;
    Channel<glm::vec3> Translation;
    Channel<glm::vec3> Scale;
};

/// @brief One clip: a track per joint it moves, and how long it runs.
struct AnimationClip
{
    std::string Name;
    std::vector<JointTrack> Tracks;
    /// The last key time across every channel; a loop wraps at it.
    float Duration = 0.f;
};

} // namespace Assisi::Geometry
