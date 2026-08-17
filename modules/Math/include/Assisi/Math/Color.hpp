/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Color.hpp
/// @brief Linear-RGB colour types — a vector in memory, its own type to an editor.
///
/// Color3/Color4 are layout- and codec-identical to glm::vec3/glm::vec4: the
/// same floats in the same order, and reflection serializes them through the
/// same JSON array, so a file written before a field changed type still loads
/// and a re-save is byte-identical. What the distinct type buys is at the point
/// of use — the reflected editor offers a colour picker for a Color and three
/// drag boxes for a direction, decided by the field's type rather than by a hint
/// that could be attached to any vector by mistake.
///
/// Colours here are **linear**, never sRGB. The conversion is a property of a
/// texture channel or a display transform, not of a value in memory, so nothing
/// in this header encodes or decodes a transfer function.
///
/// Derived from the glm type rather than wrapping one so that `.x`, the swizzles,
/// and every arithmetic operator keep working, and a Color passes anywhere its
/// vector is expected. Only one class in the hierarchy declares data members, so
/// the types stay standard-layout and offsetof stays valid — which reflection
/// depends on.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Math
{

/// @brief Linear RGB.
struct Color3 : glm::vec3
{
    using glm::vec3::vec3;
    constexpr Color3(const glm::vec3 &v) : glm::vec3(v) {}
};

/// @brief Linear RGB with alpha. Alpha is coverage, and is never premultiplied.
struct Color4 : glm::vec4
{
    using glm::vec4::vec4;
    constexpr Color4(const glm::vec4 &v) : glm::vec4(v) {}
};

static_assert(sizeof(Color3) == sizeof(glm::vec3), "Color3 must stay layout-identical to its vector.");
static_assert(sizeof(Color4) == sizeof(glm::vec4), "Color4 must stay layout-identical to its vector.");

} /* namespace Assisi::Math */
