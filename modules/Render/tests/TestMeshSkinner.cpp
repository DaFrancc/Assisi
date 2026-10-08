/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMeshSkinner.cpp
/// @brief A frame's skinning dispatches each read their own instance's palette
/// out of one buffer they all share.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshSkinner.hpp>

using Assisi::Render::SkinBatch;
using Assisi::Render::SkinDispatch;

namespace
{

std::vector<glm::mat4> Palette(uint32_t joints, float marker)
{
    return std::vector<glm::mat4>(joints, glm::translate(glm::mat4(1.f), glm::vec3(marker, 0.f, 0.f)));
}

} // namespace

TEST_CASE("Mesh skinner: each instance's palette follows the ones added before it")
{
    SkinBatch batch;
    const std::vector<glm::mat4> first = Palette(2, 1.f);
    const std::vector<glm::mat4> second = Palette(3, 2.f);
    batch.Add(SkinDispatch{.sourceVertexBase = 0, .posedVertexBase = 100, .vertexCount = 8}, first);
    batch.Add(SkinDispatch{.sourceVertexBase = 0, .posedVertexBase = 200, .vertexCount = 8}, second);

    REQUIRE(batch.Dispatches().size() == 2);
    CHECK(batch.Dispatches()[0].paletteBase == 0);
    CHECK(batch.Dispatches()[1].paletteBase == 2);
    REQUIRE(batch.Palettes().size() == 5);
    // The second instance's joints are its own, not the first's.
    CHECK(batch.Palettes()[2][3].x == doctest::Approx(2.f));
    CHECK(batch.Palettes()[1][3].x == doctest::Approx(1.f));
}

TEST_CASE("Mesh skinner: an instance with no vertices dispatches nothing")
{
    SkinBatch batch;
    batch.Add(SkinDispatch{.posedVertexBase = 100, .vertexCount = 0}, Palette(2, 1.f));
    CHECK(batch.Empty());
    CHECK(batch.Palettes().empty());
}

TEST_CASE("Mesh skinner: a new frame starts with no dispatches")
{
    SkinBatch batch;
    batch.Add(SkinDispatch{.posedVertexBase = 100, .vertexCount = 8}, Palette(2, 1.f));
    batch.Reset();
    CHECK(batch.Empty());
    CHECK(batch.Palettes().empty());
}
