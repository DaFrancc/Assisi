/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Angles.hpp
/// @brief Limits on angles that several unrelated places have to agree about,
/// and the measurement that has to survive the small ones.
///
/// Here rather than beside any one of them because agreement is the point: a
/// number two modules each write out separately is a number that drifts, and the
/// drift shows up as two things describing one light differently.

#include <algorithm>
#include <cmath>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Math
{

/// @brief The widest half-angle a cone may take, in degrees.
///
/// A cone at a right angle has no rim: its radius is `tan(halfAngle)` times its
/// height, and that diverges at ninety. Everything built from a cone inherits
/// the same wall — a spot light's shadow map is a perspective frustum whose
/// projection needs `tan(fov / 2)`, and the wireframe an author aims that cone
/// by needs the rim itself.
///
/// One degree short is as close as anything comes. Wider than this a spot light
/// is a point light with five faces missing, so nothing is authored toward it
/// and the clamp costs nobody anything.
inline constexpr float kMaxConeHalfAngleDegrees = 89.0f;

/// @brief The half-angle a spot light's cone takes when nothing has said.
///
/// Both the default of an unaimed light and the substitute for an angle that is
/// not a number, so those two cases produce the same cone rather than two
/// different wrong ones. Ninety degrees across: wide enough to read as a light,
/// narrow enough not to read as a point light.
inline constexpr float kDefaultSpotOuterAngleDegrees = 45.0f;

/// @brief The angle between two directions, in radians. Neither has to be a unit
/// vector; a zero-length one answers zero rather than a NaN.
///
/// From the chord between the normalised directions rather than from their dot
/// product, which is what `glm::angle` and every other `acos(dot(a, b))` does.
/// The difference is the whole reason this exists: near zero the cosine is 1
/// minus something under the float epsilon, so the dot product has already
/// rounded to exactly 1 and the arc cosine of it returns exactly 0 — every angle
/// below about a thousandth of a radian measures as no angle at all. The chord
/// keeps its significant digits all the way down, and the angles that decide
/// anything in a day-night cycle are millionths of a radian.
///
/// The chord form loses its precision at the opposite end instead, where two
/// directions are nearly antiparallel. Nothing here measures those: this is for
/// telling small angles from smaller ones.
[[nodiscard]] inline float AngleBetween(const glm::vec3 &lhs, const glm::vec3 &rhs)
{
    const float lhsLength = std::sqrt(glm::dot(lhs, lhs));
    const float rhsLength = std::sqrt(glm::dot(rhs, rhs));
    if (!(lhsLength > 0.f) || !(rhsLength > 0.f) || !std::isfinite(lhsLength) || !std::isfinite(rhsLength))
    {
        return 0.f;
    }
    const glm::vec3 chord = (rhs / rhsLength) - (lhs / lhsLength);
    const float half = 0.5f * std::sqrt(glm::dot(chord, chord));
    return 2.f * std::asin(std::min(half, 1.f));
}

} // namespace Assisi::Math
