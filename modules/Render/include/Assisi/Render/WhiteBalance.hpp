/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WhiteBalance.hpp
/// @brief Which colour of light the picture shows as white, as a camera's
/// white balance does.
///
/// Set to the colour temperature of the light in a scene, light of that colour
/// comes out white: a room lit by 3000 K lamps, balanced to 3000 K, looks
/// neutral, and daylight in it looks blue. Balanced higher than the light, the
/// picture turns warmer; lower, cooler. The tint moves the white along the
/// other axis, toward green or magenta.
///
/// The correction scales each of the eye's three cone responses, the Bradford
/// method, so white comes out white at its own brightness and every other
/// colour shifts with it.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @name Ranges the white balance is held inside
/// @{
/// Kelvin. Below candlelight or past a clear blue sky the curve the
/// temperature is read from no longer holds.
inline constexpr float kMinWhiteBalanceKelvin = 1700.0f;
inline constexpr float kMaxWhiteBalanceKelvin = 20000.0f;
/// The white balance that changes nothing.
inline constexpr float kNeutralWhiteBalanceKelvin = 6500.0f;
/// From -1, the picture toward green, to 1, toward magenta.
inline constexpr float kMinWhiteBalanceTint = -1.0f;
inline constexpr float kMaxWhiteBalanceTint = 1.0f;
/// @}

/// @brief The chromaticity, CIE 1931 xy, of light of @p kelvin moved by
/// @p tint across the line black bodies trace. Positive tint moves it toward
/// green, so balancing for it turns the picture magenta.
[[nodiscard]] glm::vec2 WhiteBalanceChromaticity(float kelvin, float tint);

/// @brief The matrix that turns linear sRGB as lit by light of @p kelvin and
/// @p tint into linear sRGB with that light white. The identity at
/// kNeutralWhiteBalanceKelvin and no tint.
[[nodiscard]] glm::mat3 WhiteBalanceMatrix(float kelvin, float tint);

/// @brief The linear sRGB of the white @p kelvin and @p tint name, at a
/// luminance of one.
[[nodiscard]] glm::vec3 WhiteBalanceWhite(float kelvin, float tint);

} // namespace Assisi::Render
