/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/WorldPosition.hpp>

#include <Assisi/Math/Float.hpp>
#include <Assisi/Math/WorldCell.hpp>
#include <Assisi/NetSync/BodyState.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Assisi::NetSync
{
namespace
{

/// How one axis of a cell that is not the reference's differs from it.
enum class CellStep : std::uint32_t
{
    Same,
    Up,     ///< One cell further along the axis.
    Down,   ///< One cell back.
    Escape, ///< By a zigzag varint that follows.
    Count_
};

/// Bits a CellStep takes on the wire.
constexpr std::uint32_t kCellStepBits = 2;
static_assert(static_cast<std::uint32_t>(CellStep::Count_) <= (1u << kCellStepBits));

/// How far apart two cells inside the world can be on one axis.
constexpr std::int64_t kWidestCellChange = 2 * static_cast<std::int64_t>(kMaxWorldCell);

/// One axis of a position on the grid: its cell, and its steps from the
/// cell's low edge, the cell's centre being half a cell of steps in.
struct AxisOnGrid
{
    std::int32_t cell;
    std::uint32_t offset;
};

AxisOnGrid OnGrid(double value, std::uint32_t bits)
{
    const double edge = static_cast<double>(kMaxWorldCell) * Math::kWorldCellSize;
    const double inside = Math::IsFinite(value) ? std::clamp(value, -edge, edge) : 0.0;

    const std::int64_t stepsPerCell = std::int64_t{1} << bits;
    const double stepsPerMetre = static_cast<double>(stepsPerCell) / Math::kWorldCellSize;
    const std::int64_t steps = std::llround(inside * stepsPerMetre);

    // Measured from the low edge of cell 0, so a right shift floors to the
    // cell for negative positions as well as positive ones.
    const std::int64_t fromEdge = steps + stepsPerCell / 2;
    const std::int64_t cell = fromEdge >> bits;
    return AxisOnGrid{.cell = static_cast<std::int32_t>(cell),
                      .offset = static_cast<std::uint32_t>(fromEdge - cell * stepsPerCell)};
}

/// Exact: the step count fits a double's mantissa, and the step is a power of
/// two.
double FromGrid(std::int32_t cell, std::uint32_t offset, std::uint32_t bits)
{
    const std::int64_t stepsPerCell = std::int64_t{1} << bits;
    const std::int64_t steps =
        static_cast<std::int64_t>(cell) * stepsPerCell + static_cast<std::int64_t>(offset) - stepsPerCell / 2;
    return static_cast<double>(steps) * (Math::kWorldCellSize / static_cast<double>(stepsPerCell));
}

std::uint64_t ZigZag(std::int64_t value)
{
    return (static_cast<std::uint64_t>(value) << 1u) ^ static_cast<std::uint64_t>(value >> 63);
}

std::int64_t UnZigZag(std::uint64_t value)
{
    return static_cast<std::int64_t>(value >> 1u) ^ -static_cast<std::int64_t>(value & 1u);
}

void WriteCellChange(const glm::ivec3 &cell, const glm::ivec3 &reference, Core::BitWriter &writer)
{
    const bool changed = cell != reference;
    writer.WriteBool(changed);
    if (!changed)
    {
        return;
    }
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        const std::int64_t change = static_cast<std::int64_t>(cell[axis]) - static_cast<std::int64_t>(reference[axis]);
        CellStep step = CellStep::Escape;
        if (change == 0)
        {
            step = CellStep::Same;
        }
        else if (change == 1)
        {
            step = CellStep::Up;
        }
        else if (change == -1)
        {
            step = CellStep::Down;
        }
        writer.WriteBits(static_cast<std::uint32_t>(step), kCellStepBits);
        if (step == CellStep::Escape)
        {
            writer.WriteVarUInt64(ZigZag(change));
        }
    }
}

/// Moves @p cell, the reference's on the way in, to the one on the wire; false
/// for a cell outside the world.
bool ReadCellChange(Core::BitReader &reader, glm::ivec3 &cell)
{
    if (!reader.ReadBool())
    {
        return true;
    }
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        std::int64_t change = 0;
        switch (static_cast<CellStep>(reader.ReadBits(kCellStepBits)))
        {
        case CellStep::Same:
            break;
        case CellStep::Up:
            change = 1;
            break;
        case CellStep::Down:
            change = -1;
            break;
        case CellStep::Escape:
            change = UnZigZag(reader.ReadVarUInt64());
            break;
        case CellStep::Count_:
            return false;
        }
        if (change < -kWidestCellChange || change > kWidestCellChange)
        {
            return false;
        }
        const std::int64_t moved = static_cast<std::int64_t>(cell[axis]) + change;
        if (moved < -kMaxWorldCell || moved > kMaxWorldCell)
        {
            return false;
        }
        cell[axis] = static_cast<std::int32_t>(moved);
    }
    return true;
}

} // namespace

void WriteWorldPosition(const glm::dvec3 &position, CellReference &reference, Core::BitWriter &writer)
{
    const std::uint32_t bits = Quantization().positionBits;
    const std::array<AxisOnGrid, 3> axes{OnGrid(position.x, bits), OnGrid(position.y, bits),
                                         OnGrid(position.z, bits)};
    const glm::ivec3 cell{axes[0].cell, axes[1].cell, axes[2].cell};

    WriteCellChange(cell, reference.cell, writer);
    for (const AxisOnGrid &axis : axes)
    {
        writer.WriteBits(axis.offset, bits);
    }
    reference.cell = cell;
}

bool ReadWorldPosition(Core::BitReader &reader, CellReference &reference, glm::dvec3 &outPosition)
{
    const std::uint32_t bits = Quantization().positionBits;
    glm::ivec3 cell = reference.cell;
    if (!ReadCellChange(reader, cell))
    {
        reader.Invalidate();
        return false;
    }

    glm::dvec3 position{};
    for (glm::length_t axis = 0; axis < 3; ++axis)
    {
        position[axis] = FromGrid(cell[axis], reader.ReadBits(bits), bits);
    }
    if (!reader.Ok())
    {
        return false;
    }
    reference.cell = cell;
    outPosition = position;
    return true;
}

} // namespace Assisi::NetSync
