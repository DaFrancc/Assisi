/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldPosition.cpp
/// @brief A world position on the wire as a cell and an offset inside it: on
///        one grid everywhere, exact once on it, smooth across a cell edge,
///        and one bit dearer than the offsets alone while it stays in a cell.

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Math/Float.hpp>
#include <Assisi/Math/WorldCell.hpp>
#include <Assisi/NetSync/BodyState.hpp>
#include <Assisi/NetSync/WorldPosition.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

/// Bits an axis of an offset takes at the default quantization.
std::size_t OffsetBits() { return static_cast<std::size_t>(Quantization().positionBits); }

/// The grid step at the default quantization (m).
double Step() { return Math::kWorldCellSize / static_cast<double>(std::uint64_t{1} << OffsetBits()); }

/// Positions near the origin and out to a few hundred kilometres, on both sides.
constexpr std::array<glm::dvec3, 5> kPlaces{
    glm::dvec3{0.3, -1.7, 12.5},
    glm::dvec3{-255.9, 255.9, -0.0001},
    glm::dvec3{50000.123, 3.2, -49999.9},
    glm::dvec3{300000.4, -12.6, 7.7},
    glm::dvec3{-123456.789, 9876.5, 222222.2},
};

/// Writes @p position alone and reads it back.
glm::dvec3 RoundTrip(const glm::dvec3 &position)
{
    Core::BitWriter writer;
    CellReference written;
    WriteWorldPosition(position, written, writer);
    Core::BitReader reader(writer.Data());
    CellReference read;
    glm::dvec3 decoded{};
    REQUIRE(ReadWorldPosition(reader, read, decoded));
    return decoded;
}

/// Bits a position takes after @p previous in the same run.
std::size_t BitsAfter(const glm::dvec3 &previous, const glm::dvec3 &position)
{
    Core::BitWriter writer;
    CellReference reference;
    WriteWorldPosition(previous, reference, writer);
    const std::size_t before = writer.BitsWritten();
    WriteWorldPosition(position, reference, writer);
    return writer.BitsWritten() - before;
}

/// The edge between cells 97 and 98 on an axis (m), 49.92 km out.
constexpr double kEdge = 97.5 * Math::kWorldCellSize;

/// A body moving across that edge: how far it starts short of it (m), and how
/// far it moves between two packets, less than a grid step (m).
constexpr double kShortOfEdge = 0.05;
constexpr double kStride = 0.0004;
constexpr std::int32_t kStrides = 250;

} // namespace

TEST_CASE("A position comes back within half a grid step, near the origin and hundreds of kilometres out")
{
    // The difference is worked out in doubles, which round it by far less than
    // this allowance.
    constexpr double kArithmetic = 1e-9;
    for (const glm::dvec3 &place : kPlaces)
    {
        CAPTURE(place.x);
        const glm::dvec3 decoded = RoundTrip(place);
        for (glm::length_t axis = 0; axis < 3; ++axis)
        {
            CHECK(std::abs(decoded[axis] - place[axis]) <= Step() / 2.0 + kArithmetic);
        }
    }
}

TEST_CASE("A position read off the wire and written again comes back as the same double")
{
    for (const glm::dvec3 &place : kPlaces)
    {
        const glm::dvec3 once = RoundTrip(place);
        const glm::dvec3 twice = RoundTrip(once);
        for (glm::length_t axis = 0; axis < 3; ++axis)
        {
            CHECK(Math::Identical(once[axis], twice[axis]));
        }
    }
}

TEST_CASE("The cell a position is sent in is the engine's world cell")
{
    for (const glm::dvec3 &place : kPlaces)
    {
        Core::BitWriter writer;
        CellReference written;
        WriteWorldPosition(place, written, writer);
        Core::BitReader reader(writer.Data());
        CellReference read;
        glm::dvec3 decoded{};
        REQUIRE(ReadWorldPosition(reader, read, decoded));
        CHECK(read.cell == Math::WorldCellOf(decoded));
        CHECK(written.cell == read.cell);
    }
}

TEST_CASE("A body crossing a cell edge moves on the wire by no more than a grid step at a time")
{
    // Each position in a packet of its own, as a moving body is sent: a fresh
    // reference each time, so every one says its cell in full.
    std::vector<double> decoded;
    for (std::int32_t i = 0; i <= kStrides; ++i)
    {
        const double x = kEdge - kShortOfEdge + kStride * static_cast<double>(i);
        decoded.push_back(RoundTrip(glm::dvec3{x, 0.0, -x}).x);
    }
    REQUIRE(Math::WorldCellOf(glm::dvec3{decoded.front(), 0.0, 0.0}).x == 97);
    REQUIRE(Math::WorldCellOf(glm::dvec3{decoded.back(), 0.0, 0.0}).x == 98);
    for (std::size_t i = 1; i < decoded.size(); ++i)
    {
        const double moved = decoded[i] - decoded[i - 1];
        CHECK(moved >= 0.0);
        CHECK(moved <= Step());
    }
}

TEST_CASE("A run of positions far apart, as a teleport sends, each comes back where it was")
{
    constexpr double kArithmetic = 1e-9;
    Core::BitWriter writer;
    CellReference written;
    for (const glm::dvec3 &place : kPlaces)
    {
        WriteWorldPosition(place, written, writer);
    }
    Core::BitReader reader(writer.Data());
    CellReference read;
    for (const glm::dvec3 &place : kPlaces)
    {
        glm::dvec3 decoded{};
        REQUIRE(ReadWorldPosition(reader, read, decoded));
        CHECK(glm::length(decoded - place) <= Step() + kArithmetic);
    }
    CHECK(reader.BitsRemaining() < 8u);
}

TEST_CASE("A position in the same cell as the one before costs one bit over its offsets, a neighbour's seven")
{
    const std::size_t offsets = 3 * OffsetBits();
    const glm::dvec3 far{50000.0, 0.0, -50000.0};
    CHECK(BitsAfter(far, far + glm::dvec3{3.0, 1.0, -2.0}) == offsets + 1);
    CHECK(BitsAfter(far, far + glm::dvec3{Math::kWorldCellSize, 0.0, 0.0}) == offsets + 1 + 3 * 2);
    // The first in a run is measured from the origin's cell, which a level
    // built around the origin never leaves.
    Core::BitWriter writer;
    CellReference reference;
    WriteWorldPosition(glm::dvec3{12.0, -3.0, 200.0}, reference, writer);
    CHECK(writer.BitsWritten() == offsets + 1);
}

TEST_CASE("A cell past the edge of the world on the wire is refused")
{
    // The cell changes on x by an escaped varint, and not on y or z.
    constexpr std::uint32_t kEscape = 3;
    constexpr std::uint32_t kSame = 0;
    constexpr std::uint32_t kAxisCodeBits = 2;
    const std::uint64_t past = static_cast<std::uint64_t>(kMaxWorldCell) + 1u;
    Core::BitWriter writer;
    writer.WriteBool(true);
    writer.WriteBits(kEscape, kAxisCodeBits);
    writer.WriteVarUInt64(past << 1u);
    writer.WriteBits(kSame, kAxisCodeBits);
    writer.WriteBits(kSame, kAxisCodeBits);
    for (std::int32_t axis = 0; axis < 3; ++axis)
    {
        writer.WriteBits(0, static_cast<std::uint32_t>(OffsetBits()));
    }

    Core::BitReader reader(writer.Data());
    CellReference reference;
    glm::dvec3 decoded{};
    CHECK_FALSE(ReadWorldPosition(reader, reference, decoded));
    CHECK_FALSE(reader.Ok());
}

TEST_CASE("A position past the edge of the world is sent at the edge, and one that is not a number at the origin")
{
    const double edge = static_cast<double>(kMaxWorldCell) * Math::kWorldCellSize;
    const glm::dvec3 clamped = RoundTrip(glm::dvec3{1e15, -1e15, 3.0});
    CHECK(Math::Identical(clamped.x, edge));
    CHECK(Math::Identical(clamped.y, -edge));

    const glm::dvec3 nothing = RoundTrip(glm::dvec3{std::numeric_limits<double>::quiet_NaN(), 5.0,
                                                    std::numeric_limits<double>::infinity()});
    CHECK(Math::Identical(nothing.x, 0.0));
    CHECK(Math::Identical(nothing.z, 0.0));
    CHECK(std::abs(nothing.y - 5.0) <= Step());
}
