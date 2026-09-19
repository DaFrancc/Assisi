/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <cstdint>

using namespace Assisi::Mondrian;

namespace
{

constexpr Rect kVisible{.x = 10.f, .y = 10.f, .width = 40.f, .height = 20.f};
constexpr Rect kElsewhere{.x = 500.f, .y = 500.f, .width = 10.f, .height = 10.f};
constexpr TextureId kTextureA{1};
constexpr TextureId kTextureB{2};

} // namespace

TEST_CASE("DrawList: the builder sets each property on the quad it made, and only that one")
{
    constexpr Color kFill{.r = 0.1f, .g = 0.2f, .b = 0.3f, .a = 0.4f};
    constexpr Color kEdge{.r = 0.5f, .g = 0.6f, .b = 0.7f, .a = 0.8f};
    constexpr Rect kUv{.x = 0.25f, .y = 0.5f, .width = 0.25f, .height = 0.5f};
    constexpr float kBorder = 3.f;
    constexpr float kRadius = 6.f;
    constexpr float kLone   = 2.f;

    DrawList list;
    list.Quad(kVisible);
    list.Quad(kVisible)
        .Fill(kFill)
        .Border(kBorder, kEdge)
        .Corners(kRadius, CornerStyle::Rounded)
        .CornerAt(Corner::BottomLeft, kLone, CornerStyle::Cut)
        .Clip(kVisible)
        .Texture(kTextureA, kUv);
    list.Finalize();

    REQUIRE(list.Instances().size() == 2);
    const QuadInstance &untouched = list.Instances()[0];
    CHECK(untouched.borderWidth == 0.f);
    CHECK(untouched.cornerStyles == 0u);
    CHECK(untouched.kind == static_cast<uint32_t>(QuadKind::Solid));

    const QuadInstance &built = list.Instances()[1];
    CHECK(built.color.b == kFill.b);
    CHECK(built.borderWidth == kBorder);
    CHECK(built.borderColor.g == kEdge.g);
    CHECK(built.cornerRadius[static_cast<std::size_t>(Corner::TopLeft)] == kRadius);
    CHECK(built.cornerRadius[static_cast<std::size_t>(Corner::BottomLeft)] == kLone);
    CHECK(UnpackCornerStyle(built.cornerStyles, Corner::TopRight) == CornerStyle::Rounded);
    CHECK(UnpackCornerStyle(built.cornerStyles, Corner::BottomLeft) == CornerStyle::Cut);
    CHECK(built.clip.width == kVisible.width);
    CHECK(built.uv.y == kUv.y);
    CHECK(built.kind == static_cast<uint32_t>(QuadKind::Image));
}

TEST_CASE("DrawList: every corner keeps its own style when packed")
{
    uint32_t packed = 0;
    packed          = PackCornerStyle(packed, Corner::TopLeft, CornerStyle::Cut);
    packed          = PackCornerStyle(packed, Corner::TopRight, CornerStyle::Rounded);
    packed          = PackCornerStyle(packed, Corner::BottomRight, CornerStyle::Square);
    packed          = PackCornerStyle(packed, Corner::BottomLeft, CornerStyle::Cut);
    // Overwriting one corner leaves the others alone.
    packed = PackCornerStyle(packed, Corner::TopLeft, CornerStyle::Rounded);

    CHECK(UnpackCornerStyle(packed, Corner::TopLeft) == CornerStyle::Rounded);
    CHECK(UnpackCornerStyle(packed, Corner::TopRight) == CornerStyle::Rounded);
    CHECK(UnpackCornerStyle(packed, Corner::BottomRight) == CornerStyle::Square);
    CHECK(UnpackCornerStyle(packed, Corner::BottomLeft) == CornerStyle::Cut);
}

TEST_CASE("DrawList: consecutive quads with one texture share a draw")
{
    DrawList list;
    list.Quad(kVisible).Texture(kTextureA, Rect{});
    list.Quad(kVisible).Texture(kTextureA, Rect{});
    list.Quad(kVisible).Texture(kTextureB, Rect{});
    list.Finalize();

    REQUIRE(list.Entries().size() == 2);
    CHECK(list.Entries()[0].firstInstance == 0u);
    CHECK(list.Entries()[0].instanceCount == 2u);
    CHECK(list.Entries()[0].texture == kTextureA);
    CHECK(list.Entries()[1].firstInstance == 2u);
    CHECK(list.Entries()[1].instanceCount == 1u);
    CHECK(list.Entries()[1].texture == kTextureB);
}

TEST_CASE("DrawList: a texture that returns after another starts a new draw")
{
    DrawList list;
    list.Quad(kVisible).Texture(kTextureA, Rect{});
    list.Quad(kVisible).Texture(kTextureB, Rect{});
    list.Quad(kVisible).Texture(kTextureA, Rect{});
    list.Finalize();

    // Merging the two A quads would draw the B quad out of order.
    REQUIRE(list.Entries().size() == 3);
    CHECK(list.Entries()[2].texture == kTextureA);
    CHECK(list.Entries()[2].firstInstance == 2u);
}

TEST_CASE("DrawList: a material or mask change also starts a new draw")
{
    DrawList list;
    list.Quad(kVisible);
    list.Quad(kVisible).Material(MaterialId{1});
    list.Quad(kVisible).Mask(MaskId{1});
    list.Finalize();

    CHECK(list.Entries().size() == 3);
}

TEST_CASE("DrawList: quads that cannot be seen are dropped and the draws index what is left")
{
    DrawList list;
    list.Quad(kVisible).Texture(kTextureA, Rect{});
    list.Quad(kVisible).Clip(kElsewhere);
    list.Quad(Rect{.x = 0.f, .y = 0.f, .width = 0.f, .height = 5.f});
    list.Quad(kVisible).Texture(kTextureB, Rect{});
    list.Finalize();

    REQUIRE(list.Instances().size() == 2);
    REQUIRE(list.Entries().size() == 2);
    CHECK(list.Entries()[1].firstInstance == 1u);
    CHECK(list.Entries()[1].texture == kTextureB);
    CHECK(list.Instances()[1].uv.width == 0.f);
}

TEST_CASE("DrawList: clearing empties it and takes quads again")
{
    DrawList list;
    list.Quad(kVisible);
    list.Finalize();
    list.Clear();

    CHECK_FALSE(list.IsFinalized());
    CHECK(list.Instances().empty());
    CHECK(list.Entries().empty());
    list.Quad(kVisible);
    list.Finalize();
    CHECK(list.Instances().size() == 1);
    CHECK(list.Entries().size() == 1);
}

#ifndef NDEBUG
TEST_CASE("DrawList: a finalized list refuses changes")
{
    const Assisi::Testing::ThrowOnContractViolation guard;

    DrawList list;
    QuadBuilder quad = list.Quad(kVisible);
    list.Finalize();
    CHECK_THROWS_AS(quad.Fill(Color{}), Assisi::Core::ContractViolation);
    CHECK_THROWS_AS(list.Quad(kVisible), Assisi::Core::ContractViolation);
}
#endif
