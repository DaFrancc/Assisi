/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlendedDraws.cpp
/// @brief See-through surfaces are drawn farthest first, and a double-sided one
///        its back faces before its front faces.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/MeshPass.hpp>

using Assisi::Render::BlendedDraw;
using Assisi::Render::BlendedFaces;
using Assisi::Render::DrawItem;
using Assisi::Render::MakeBlendedSortKey;
using Assisi::Render::Material;
using Assisi::Render::MeshBuffer;
using Assisi::Render::PlanBlendedDraws;

namespace
{
/// The planner compares meshes by address and never reads them, so two empty
/// ones stand for any two meshes.
const MeshBuffer kMeshStorage[2];
const MeshBuffer *const kMeshA = &kMeshStorage[0];
const MeshBuffer *const kMeshB = &kMeshStorage[1];

Material MakeBlended(bool doubleSided)
{
    Assisi::Geometry::MaterialData source;
    source.Alpha = Assisi::Geometry::AlphaMode::Blend;
    source.DoubleSided = doubleSided;
    Material material;
    material.Create(nullptr, 0, source, Assisi::Render::MaterialTextures{});
    return material;
}

DrawItem Item(const MeshBuffer *mesh, uint32_t submesh, const Material &material)
{
    return DrawItem{.mesh = mesh, .submeshIndex = submesh, .material = &material};
}

bool Same(const BlendedDraw &draw, const MeshBuffer *mesh, uint32_t first, uint32_t count, BlendedFaces faces)
{
    return draw.mesh == mesh && draw.firstInstance == first && draw.instanceCount == count && draw.faces == faces;
}
} // namespace

TEST_CASE("The blended sort key puts the farthest surface first")
{
    CHECK(MakeBlendedSortKey(20.f) < MakeBlendedSortKey(10.f));
    CHECK(MakeBlendedSortKey(10.f) < MakeBlendedSortKey(0.f));
    // Panes a few centimetres apart a long way off keep their order: the
    // distance is not quantized.
    CHECK(MakeBlendedSortKey(500.03f) < MakeBlendedSortKey(500.f));
    CHECK(MakeBlendedSortKey(0.10f) < MakeBlendedSortKey(0.09f));
}

TEST_CASE("Sorting stably keeps one object's submeshes in mesh order")
{
    // Every submesh of an object shares its distance, so only a stable sort
    // keeps them from swapping between frames.
    std::vector<DrawItem> items;
    for (uint32_t submesh = 0; submesh < 8u; ++submesh)
    {
        items.push_back(DrawItem{.sortKey = MakeBlendedSortKey(5.f), .submeshIndex = submesh});
    }
    items.push_back(DrawItem{.sortKey = MakeBlendedSortKey(50.f), .submeshIndex = 100u});
    std::ranges::stable_sort(items, {}, &DrawItem::sortKey);

    CHECK(items.front().submeshIndex == 100u);
    for (uint32_t i = 1; i < items.size(); ++i)
    {
        CHECK(items[i].submeshIndex == i - 1u);
    }
}

TEST_CASE("A double-sided see-through surface draws its back faces, then its front faces")
{
    const Material twoSided = MakeBlended(true);
    const std::vector<DrawItem> items{Item(kMeshA, 0, twoSided)};

    std::vector<BlendedDraw> draws;
    PlanBlendedDraws(items, draws);
    REQUIRE(draws.size() == 2);
    CHECK(Same(draws[0], kMeshA, 0, 1, BlendedFaces::Back));
    CHECK(Same(draws[1], kMeshA, 0, 1, BlendedFaces::Front));
}

TEST_CASE("A single-sided see-through surface draws its front faces only")
{
    const Material oneSided = MakeBlended(false);
    const std::vector<DrawItem> items{Item(kMeshA, 0, oneSided)};

    std::vector<BlendedDraw> draws;
    PlanBlendedDraws(items, draws);
    REQUIRE(draws.size() == 1);
    CHECK(Same(draws[0], kMeshA, 0, 1, BlendedFaces::Front));
}

TEST_CASE("Each double-sided surface finishes before the nearer one starts")
{
    // Two glass boxes of one mesh, the far one first. Batching their back faces
    // together would draw the far box's front over the near box's back.
    const Material twoSided = MakeBlended(true);
    const std::vector<DrawItem> items{Item(kMeshA, 0, twoSided), Item(kMeshA, 0, twoSided)};

    std::vector<BlendedDraw> draws;
    PlanBlendedDraws(items, draws);
    REQUIRE(draws.size() == 4);
    CHECK(Same(draws[0], kMeshA, 0, 1, BlendedFaces::Back));
    CHECK(Same(draws[1], kMeshA, 0, 1, BlendedFaces::Front));
    CHECK(Same(draws[2], kMeshA, 1, 1, BlendedFaces::Back));
    CHECK(Same(draws[3], kMeshA, 1, 1, BlendedFaces::Front));
}

TEST_CASE("Neighbouring single-sided draws of one submesh share an instanced draw")
{
    // An instanced draw rasterizes its instances in order, so sharing one keeps
    // the composite order.
    const Material oneSided = MakeBlended(false);
    const std::vector<DrawItem> items{Item(kMeshA, 0, oneSided), Item(kMeshA, 0, oneSided),
                                      Item(kMeshB, 0, oneSided), Item(kMeshA, 0, oneSided)};

    std::vector<BlendedDraw> draws;
    PlanBlendedDraws(items, draws);
    REQUIRE(draws.size() == 3);
    CHECK(Same(draws[0], kMeshA, 0, 2, BlendedFaces::Front));
    CHECK(Same(draws[1], kMeshB, 2, 1, BlendedFaces::Front));
    // Not merged back into the first: the mesh between them is nearer.
    CHECK(Same(draws[2], kMeshA, 3, 1, BlendedFaces::Front));
}

TEST_CASE("Draws of different submeshes, or after a double-sided one, are not merged")
{
    const Material oneSided = MakeBlended(false);
    const Material twoSided = MakeBlended(true);
    const std::vector<DrawItem> items{Item(kMeshA, 0, oneSided), Item(kMeshA, 1, oneSided),
                                      Item(kMeshA, 1, twoSided), Item(kMeshA, 1, oneSided)};

    std::vector<BlendedDraw> draws;
    PlanBlendedDraws(items, draws);
    REQUIRE(draws.size() == 5);
    CHECK(Same(draws[0], kMeshA, 0, 1, BlendedFaces::Front));
    CHECK(draws[0].submeshIndex == 0u);
    CHECK(Same(draws[1], kMeshA, 1, 1, BlendedFaces::Front));
    CHECK(draws[1].submeshIndex == 1u);
    CHECK(Same(draws[2], kMeshA, 2, 1, BlendedFaces::Back));
    CHECK(Same(draws[3], kMeshA, 2, 1, BlendedFaces::Front));
    // The same submesh as the front draw before it, but that draw belongs to a
    // double-sided surface and its record is not this one's neighbour's run.
    CHECK(Same(draws[4], kMeshA, 3, 1, BlendedFaces::Front));
}
