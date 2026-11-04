/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldCell.hpp
/// @brief The cells the world is divided into: cubes kWorldCellSize wide, one
/// centred on the origin and the rest centred on multiples of the size.
///
/// A cell is worked out from a position whenever it is wanted and never
/// stored. Centred on the origin rather than cornered there, so a level built
/// around the origin sits inside one cell, and a position is never more than
/// half a cell from its cell's centre on any axis.

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Math
{

/// Width of a world cell (m). A power of two, so a cell's centre, and every
/// step that divides the cell by a power of two, is exact in a double.
inline constexpr double kWorldCellSize = 512.0;

/// @brief The cell @p position is in: cell 0 spans [-256, 256) m on each axis.
/// Valid within about a billion kilometres of the origin, past which a cell
/// index no longer fits in an int.
[[nodiscard]] inline glm::ivec3 WorldCellOf(const glm::dvec3 &position)
{
    return glm::ivec3(glm::floor(position / kWorldCellSize + 0.5));
}

} // namespace Assisi::Math
