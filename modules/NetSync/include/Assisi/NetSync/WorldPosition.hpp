/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldPosition.hpp
/// @brief A world position on the wire: which world cell it is in, as a change
/// from a cell the reader already has, and where inside that cell.
///
/// Every position is rounded to one grid that covers the whole world, a step of
/// Math::kWorldCellSize / 2^positionBits (2^-9 m, about 2 mm, at the default
/// 18 bits). Cell edges lie on that grid, so which cell a position is in never
/// changes where it rounds to: a body crossing an edge moves on the other
/// machine as smoothly as anywhere else. A grid point comes back as exactly the
/// double it was, so a position read and written again is unchanged.
///
/// Inside its cell a position takes positionBits per axis. The cell takes one
/// bit when it is the same as the reference's, two more per axis when it is a
/// neighbour, and a varint per axis beyond that, as after a teleport.

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cstdint>

namespace Assisi::NetSync
{

/// The furthest cell from the origin a position is sent in, on any axis: about
/// 537,000 km out. A position past it is sent at its edge, and a cell past it on
/// the wire is refused. Small enough that a grid position at the finest step
/// still fits a double's 53 bits exactly.
inline constexpr std::int32_t kMaxWorldCell = 1 << 20;

/// @brief The cell a run of positions in one packet is written against.
///
/// Each position's cell goes as its change from the cell of the position before
/// it in the run, and the first one's from the cell at the origin. Writer and
/// reader each start one fresh at the start of the run and pass it to every
/// position in the run, in order. Nothing in it outlives the packet, so a lost
/// or reordered packet never leaves the two ends disagreeing about it.
struct CellReference
{
    glm::ivec3 cell{0};
};

/// @brief Write @p position against @p reference, and make its cell the
/// reference for the next. A position that is not finite is sent as the origin.
void WriteWorldPosition(const glm::dvec3 &position, CellReference &reference, Core::BitWriter &writer);

/// @brief Inverse of WriteWorldPosition. Returns false on a truncated record or
/// a cell past kMaxWorldCell, having invalidated the reader.
bool ReadWorldPosition(Core::BitReader &reader, CellReference &reference, glm::dvec3 &outPosition);

} // namespace Assisi::NetSync
