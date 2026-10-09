/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlendSpaceFile.cpp
/// @brief A `.ablnd` file reads into its points, and one a player could not
/// play is refused: no points, a point with no clip, or two at one position.

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>

using Assisi::Core::AssetId;
using Assisi::Geometry::BlendSpace;
using Assisi::Geometry::BlendSpaceError;
using Assisi::Geometry::ReadBlendSpace;

namespace
{

constexpr std::string_view kIdle = "1c7fa8f9-bd8e-4f5d-b402-edadb14dc57d";
constexpr std::string_view kWalk = "2d8eb9fa-ce9f-4e6e-a513-fe0ec25ed68e";

std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

BlendSpaceError Refusal(std::string_view text)
{
    const std::expected<BlendSpace, BlendSpaceError> space = ReadBlendSpace(Bytes(text));
    REQUIRE_FALSE(space.has_value());
    return space.error();
}

} // namespace

TEST_CASE("Blend space file: reads each point's clip and position")
{
    constexpr std::string_view kText = R"({
        "version": 1, "type": "BlendSpace",
        "Points": [
            { "Clip": { "guid": "1c7fa8f9-bd8e-4f5d-b402-edadb14dc57d", "path": "idle.glb" }, "Position": [0, 0] },
            { "Clip": "2d8eb9fa-ce9f-4e6e-a513-fe0ec25ed68e", "Position": [2.5, -1] }
        ]
    })";
    const std::expected<BlendSpace, BlendSpaceError> space = ReadBlendSpace(Bytes(kText));
    REQUIRE(space.has_value());
    REQUIRE(space->Points.size() == 2);
    CHECK(space->Points[0].Clip == AssetId::Parse(kIdle));
    CHECK(space->Points[1].Clip == AssetId::Parse(kWalk));
    CHECK(space->Points[1].Position.x == doctest::Approx(2.5f));
    CHECK(space->Points[1].Position.y == doctest::Approx(-1.f));
}

TEST_CASE("Blend space file: a space a player could not play is refused")
{
    CHECK(Refusal("not json") == BlendSpaceError::NotADocument);
    CHECK(Refusal(R"({ "version": 1, "type": "MaterialData" })") == BlendSpaceError::NotADocument);
    CHECK(Refusal(R"({ "version": 1, "type": "BlendSpace", "Points": [] })") == BlendSpaceError::NoPoints);
    CHECK(Refusal(R"({ "version": 1, "type": "BlendSpace", "Points": [ { "Position": [0, 0] } ] })") ==
          BlendSpaceError::PointHasNoClip);
    CHECK(Refusal(R"({ "version": 1, "type": "BlendSpace", "Points": [
              { "Clip": "1c7fa8f9-bd8e-4f5d-b402-edadb14dc57d", "Position": [1, 1] },
              { "Clip": "2d8eb9fa-ce9f-4e6e-a513-fe0ec25ed68e", "Position": [1, 1] } ] })") ==
          BlendSpaceError::SharedPosition);
}
