/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Angles.hpp
/// @brief Limits on angles that several unrelated places have to agree about.
///
/// Here rather than beside any one of them because agreement is the point: a
/// number two modules each write out separately is a number that drifts, and the
/// drift shows up as two things describing one light differently.

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

} // namespace Assisi::Math
