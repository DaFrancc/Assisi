/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Draw.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

using namespace Assisi::Mondrian;
using Assisi::Mondrian::Testing::FixtureFont;
using Assisi::Mondrian::Testing::kFixtureSize;

namespace
{

constexpr Extent kReference{1920, 1080};
constexpr TextureId kAtlas{9};
constexpr TextureId kPicture{11};

using Assisi::Math::ColorSpace;

Style Filled(float width, float height, float red)
{
    Style style;
    style.sizing = {Sizing::Fixed(width), Sizing::Fixed(height)};
    style.background = Assisi::Math::Color4<ColorSpace::Srgb>{red, 0.f, 0.f, 1.f};
    return style;
}

/// Lays @p tree out and draws it, unfinalized so every quad is still there.
DrawList Draw(const NodeTree &tree, const Font &font, Extent viewport = kReference, float scale = 1.f)
{
    LayoutResult layout;
    ComputeLayout(tree, viewport, scale, &font, layout);
    DrawList list;
    DrawTree(tree, layout, list, kAtlas, Interaction{});
    return list;
}

/// The index of the first quad filled with @p red, if any.
std::optional<std::size_t> QuadWithRed(const DrawList &list, float red)
{
    const auto quads = list.Instances();
    const auto found = std::ranges::find_if(quads, [red](const QuadInstance &quad)
                                            { return quad.color.r == red && quad.color.a > 0.f; });
    if (found == quads.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - quads.begin());
}

} // namespace

TEST_CASE("Draw: a box draws its background, border and corners where layout put it")
{
    NodeTree tree;
    Style style = Filled(100.f, 50.f, 0.5f);
    style.borderWidth = 2.f;
    style.borderColor = Assisi::Math::Color4<ColorSpace::Srgb>{1.f, 1.f, 1.f, 1.f};
    style.cornerRadius = 8.f;
    style.cornerStyle = CornerStyle::Rounded;
    tree.SetStyle(tree.Create(tree.Root()), style);

    const DrawList list = Draw(tree, FixtureFont());
    const std::optional<std::size_t> box = QuadWithRed(list, 0.5f);
    REQUIRE(box.has_value());
    const QuadInstance &quad = list.Instances()[*box];
    CHECK(quad.rect.width == 100.f);
    CHECK(quad.rect.height == 50.f);
    CHECK(quad.borderWidth == 2.f);
    CHECK(quad.cornerRadius[0] == 8.f);
    CHECK(UnpackCornerStyle(quad.cornerStyles, Corner::TopLeft) == CornerStyle::Rounded);
}

TEST_CASE("Draw: a border keeps at least one device pixel when the UI is scaled down")
{
    NodeTree tree;
    Style thin = Filled(100.f, 100.f, 0.25f);
    thin.borderWidth = 1.f;
    tree.SetStyle(tree.Create(tree.Root()), thin);
    Style thick = Filled(100.f, 100.f, 0.75f);
    thick.borderWidth = 3.f;
    tree.SetStyle(tree.Create(tree.Root()), thick);

    const DrawList list = Draw(tree, FixtureFont(), {960, 540}, 0.5f);
    const std::optional<std::size_t> thinQuad = QuadWithRed(list, 0.25f);
    const std::optional<std::size_t> thickQuad = QuadWithRed(list, 0.75f);
    REQUIRE(thinQuad.has_value());
    REQUIRE(thickQuad.has_value());
    CHECK(list.Instances()[*thinQuad].borderWidth == 1.f);
    CHECK(list.Instances()[*thickQuad].borderWidth == 2.f); // 1.5 device pixels, rounded
}

TEST_CASE("Draw: a floating child draws over its siblings, whatever order they were made in")
{
    NodeTree tree;
    const NodeId panel = tree.Create(tree.Root());
    tree.SetStyle(panel, Filled(200.f, 100.f, 0.1f));
    Style badge = Filled(20.f, 20.f, 0.2f);
    badge.floating.enabled = true;
    tree.SetStyle(tree.Create(panel), badge);
    tree.SetStyle(tree.Create(panel), Filled(50.f, 50.f, 0.3f));

    const DrawList list = Draw(tree, FixtureFont());
    const std::optional<std::size_t> parent = QuadWithRed(list, 0.1f);
    const std::optional<std::size_t> floated = QuadWithRed(list, 0.2f);
    const std::optional<std::size_t> sibling = QuadWithRed(list, 0.3f);
    REQUIRE(parent.has_value());
    REQUIRE(floated.has_value());
    REQUIRE(sibling.has_value());
    CHECK(*parent < *sibling);
    CHECK(*sibling < *floated);
}

TEST_CASE("Draw: text draws from the font atlas inside its node, clipped with it")
{
    NodeTree tree;
    Style listStyle = Filled(100.f, 30.f, 0.4f);
    listStyle.enabledScrollBars = {false, true};
    listStyle.direction = Direction::Column;
    const NodeId list = tree.Create(tree.Root());
    tree.SetStyle(list, listStyle);
    Style labelStyle;
    labelStyle.textSize = kFixtureSize;
    const NodeId label = tree.Create(list);
    tree.SetStyle(label, labelStyle);
    tree.SetText(label, "AA");

    DrawList drawn = Draw(tree, FixtureFont());
    std::size_t glyphs = 0;
    for (const QuadInstance &quad : drawn.Instances())
    {
        if (quad.kind == static_cast<uint32_t>(QuadKind::Glyph))
        {
            ++glyphs;
            CHECK(quad.clip.height == 30.f); // the scrolling list's rect, not unclipped
        }
    }
    CHECK(glyphs == 2);

    drawn.Finalize();
    CHECK(std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kAtlas; }));
}

TEST_CASE("Draw: an image fills its node with the texture it was given")
{
    NodeTree tree;
    const NodeId picture = tree.Create(tree.Root());
    tree.SetStyle(picture, Filled(64.f, 32.f, 0.f));
    const Rect half{.x = 0.f, .y = 0.f, .width = 0.5f, .height = 1.f};
    tree.SetImage(picture, kPicture, half);

    DrawList drawn = Draw(tree, FixtureFont());
    const auto quads = drawn.Instances();
    const auto image = std::ranges::find_if(quads, [](const QuadInstance &quad)
                                            { return quad.kind == static_cast<uint32_t>(QuadKind::Image); });
    REQUIRE(image != quads.end());
    CHECK(image->rect.width == 64.f);
    CHECK(image->uv.width == 0.5f);

    drawn.Finalize();
    CHECK(std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPicture; }));
}

TEST_CASE("Draw: a hidden node and everything under it draws nothing")
{
    NodeTree tree;
    const NodeId hidden = tree.Create(tree.Root());
    tree.SetStyle(hidden, Filled(10.f, 10.f, 0.6f));
    tree.SetStyle(tree.Create(hidden), Filled(5.f, 5.f, 0.7f));
    tree.SetVisible(hidden, false);

    const DrawList list = Draw(tree, FixtureFont());
    CHECK_FALSE(QuadWithRed(list, 0.6f).has_value());
    CHECK_FALSE(QuadWithRed(list, 0.7f).has_value());
}
