/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Gusts.hpp
/// @brief The level's gusts: one pattern of stronger and weaker wind that
/// drifts across the level with the steady wind and changes as it goes.
///
/// The pattern is gradient noise in a frame that moves with the wind, so a gust
/// travels downwind at the wind's speed, as the eddies near the ground do, and
/// lasts about as long as it takes to pass its own length. It depends only on
/// the wind and the time, so every machine sharing a clock feels the same gusts
/// without sending any. The steady wind is not changed by it: clouds move with
/// that.
///
/// **These functions are the specification and wind/gusts.glsl their
/// transcription**; the tests hold the CPU form.

#include <Assisi/Math/GLM.hpp>

#include <cstdint>

namespace Assisi::Render
{

/// @name The pattern's size
/// @{
/// A gust's length along the wind and across it, in metres. Eurocode puts the
/// average gust 10 m above the ground at 48 to 63 m long, and gusts are drawn
/// out along the wind.
inline constexpr float kGustLengthAlong = 60.0f;
inline constexpr float kGustLengthAcross = 30.0f;
/// The pattern repeats after this many gust lengths each way and this many
/// gust lifetimes: kilometres, and over half an hour in light air.
inline constexpr uint32_t kGustPeriod = 64u;
/// @}

/// @brief The wind the gusts ride on.
struct Gusts
{
    /// Which way the steady wind blows across the world, along x and z: a unit
    /// vector.
    glm::vec2 toward{0.0f, -1.0f};
    /// The steady wind, in metres per second.
    float speed = 0.0f;
    /// The strongest gust over the steady speed; 1 is a steady wind.
    float gustFactor = 1.0f;

    bool operator==(const Gusts &) const = default;
};

/// @brief Where @p position lands in @p gusts' pattern at @p seconds: in gust
/// lengths along the wind and across it, and in gust lifetimes, each wrapped
/// into the period. Double, because the wind carries the pattern kilometres in
/// an hour.
[[nodiscard]] glm::vec3 GustCell(const Gusts &gusts, glm::dvec2 position, double seconds);

/// @brief The pattern at @p cell: -1 in the deepest lull, 1 in the strongest
/// gust. Any cell is valid; the pattern repeats every kGustPeriod.
[[nodiscard]] float GustNoise(glm::vec3 cell);

/// @brief The wind's speed at @p position at @p seconds, in metres per second.
[[nodiscard]] float GustSpeed(const Gusts &gusts, glm::dvec2 position, double seconds);

/// @brief What a shader needs to find the gusts around the camera; mirrors
/// wind/gusts.glsl's GustConstants.
struct GustConstants
{
    /// xyz = the camera's cell; w = the gust factor.
    glm::vec4 cell{0.0f};
    /// xy = which way the steady wind blows; z = its speed.
    glm::vec4 wind{0.0f};
};

/// @brief The constants that find @p gusts around @p camera at @p seconds.
[[nodiscard]] GustConstants MakeGustConstants(const Gusts &gusts, glm::dvec3 camera, double seconds);

/// @brief The wind's speed at @p fromCamera metres from the camera along world x
/// and z, from @p constants: what the shader computes.
[[nodiscard]] float GustSpeedNear(const GustConstants &constants, glm::vec2 fromCamera);

} // namespace Assisi::Render
