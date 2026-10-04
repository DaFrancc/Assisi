/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Matrix.hpp
/// @brief Named access to the columns of an affine world matrix.
///
/// GLM matrices are column-major: `m[i]` is column i. For a matrix built as
/// translate * rotate * scale, the first three columns are the local axes in
/// world space, scaled, and the fourth is the translation.

#include <Assisi/Math/GLM.hpp>

#include <cstdint>

namespace Assisi::Math
{

/// @brief The columns of an affine world matrix.
enum class MatrixColumn : uint8_t
{
    Right,       ///< Local +X.
    Up,          ///< Local +Y.
    Back,        ///< Local +Z; forward is its negation.
    Translation, ///< The origin of local space, in world space.
    Count,
};

/// @brief Column @p column of @p matrix, without its w component.
[[nodiscard]] inline glm::vec3 ColumnOf(const glm::mat4 &matrix, MatrixColumn column)
{
    return glm::vec3(matrix[static_cast<glm::length_t>(column)]);
}

/// @brief Where @p matrix puts the origin of its local space.
[[nodiscard]] inline glm::vec3 TranslationOf(const glm::mat4 &matrix)
{
    return ColumnOf(matrix, MatrixColumn::Translation);
}

/// @brief Moves the origin of @p matrix's local space to @p translation.
inline void SetTranslation(glm::mat4 &matrix, const glm::vec3 &translation)
{
    matrix[static_cast<glm::length_t>(MatrixColumn::Translation)] = glm::vec4(translation, 1.f);
}

} // namespace Assisi::Math
