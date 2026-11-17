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
#include <span>

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
    style.sizing = {Sizing::Fixed(Px(width)), Sizing::Fixed(Px(height))};
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

/// The first quad of @p kind, or null.
const QuadInstance *FirstOfKind(const DrawList &list, QuadKind kind)
{
    const std::span<const QuadInstance> quads = list.Instances();
    const std::span<const QuadInstance>::iterator found = std::ranges::find_if(
        quads, [kind](const QuadInstance &quad) { return quad.kind == static_cast<uint32_t>(kind); });
    return found != quads.end() ? &*found : nullptr;
}

constexpr Assisi::Math::Color4<ColorSpace::Srgb> kTint{0.5f, 0.75f, 1.f, 1.f};

/// A frame with a different border on each side, so a mixed-up side shows.
Sprite Framed()
{
    Sprite frame = WholeTexture(kPicture, 16, 16);
    frame.borders = {.left = 4, .top = 3, .right = 2, .bottom = 1};
    return frame;
}

} // namespace

TEST_CASE("Draw: a box draws its background, border and corners where layout put it")
{
    NodeTree tree;
    Style style = Filled(100.f, 50.f, 0.5f);
    style.borderWidth = Px(2.f);
    style.borderColor = Assisi::Math::Color4<ColorSpace::Srgb>{1.f, 1.f, 1.f, 1.f};
    style.cornerRadius = Px(8.f);
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
    thin.borderWidth = Px(1.f);
    tree.SetStyle(tree.Create(tree.Root()), thin);
    Style thick = Filled(100.f, 100.f, 0.75f);
    thick.borderWidth = Px(3.f);
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
    labelStyle.textSize = Px(kFixtureSize);
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
    Sprite half = WholeTexture(kPicture, 0, 0);
    half.uv.width = 0.5f;
    tree.SetImage(picture, half);

    DrawList drawn = Draw(tree, FixtureFont());
    const QuadInstance *image = FirstOfKind(drawn, QuadKind::Image);
    REQUIRE(image != nullptr);
    CHECK(image->rect.width == 64.f);
    CHECK(image->uv.width == 0.5f);

    drawn.Finalize();
    CHECK(std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPicture; }));
}

TEST_CASE("Draw: a picture sits inside its node's padding, tinted")
{
    NodeTree tree;
    const NodeId picture = tree.Create(tree.Root());
    Style style = Filled(100.f, 60.f, 0.f);
    style.padding = Padding::All(Px(10.f));
    style.tint = kTint;
    tree.SetStyle(picture, style);
    tree.SetImage(picture, WholeTexture(kPicture, 0, 0));

    const DrawList drawn = Draw(tree, FixtureFont());
    const QuadInstance *image = FirstOfKind(drawn, QuadKind::Image);
    REQUIRE(image != nullptr);
    CHECK(image->rect.x == 10.f);
    CHECK(image->rect.width == 80.f);
    CHECK(image->rect.height == 40.f);
    CHECK(image->color == kTint);
}

TEST_CASE("Draw: contain shows all of a picture as large as fits, keeping its shape, in the middle")
{
    NodeTree tree;
    const NodeId picture = tree.Create(tree.Root());
    Style style = Filled(200.f, 200.f, 0.f);
    style.fit = ImageFit::Contain;
    tree.SetStyle(picture, style);
    tree.SetImage(picture, WholeTexture(kPicture, 100, 50));

    const DrawList drawn = Draw(tree, FixtureFont());
    const QuadInstance *image = FirstOfKind(drawn, QuadKind::Image);
    REQUIRE(image != nullptr);
    CHECK(image->rect.width == 200.f);
    CHECK(image->rect.height == 100.f);
    CHECK(image->rect.y == 50.f);
    CHECK(image->uv.width == 1.f);
    CHECK(image->uv.height == 1.f);
}

TEST_CASE("Draw: cover fills the node, keeping the picture's shape, and cuts off what overhangs evenly")
{
    NodeTree tree;
    const NodeId picture = tree.Create(tree.Root());
    Style style = Filled(100.f, 100.f, 0.f);
    style.fit = ImageFit::Cover;
    tree.SetStyle(picture, style);
    tree.SetImage(picture, WholeTexture(kPicture, 100, 50));

    const DrawList drawn = Draw(tree, FixtureFont());
    const QuadInstance *image = FirstOfKind(drawn, QuadKind::Image);
    REQUIRE(image != nullptr);
    CHECK(image->rect.width == 100.f);
    CHECK(image->rect.height == 100.f);
    // Twice as wide as it is tall, scaled up to the node's height: half its
    // width shows, a quarter cut off each side.
    CHECK(image->uv.x == doctest::Approx(0.25f));
    CHECK(image->uv.width == doctest::Approx(0.5f));
    CHECK(image->uv.height == 1.f);
}

TEST_CASE("Draw: a sprite with borders draws as a nine-slice whose corners keep their size times the UI scale")
{
    NodeTree tree;
    const NodeId frame = tree.Create(tree.Root());
    tree.SetStyle(frame, Filled(300.f, 200.f, 0.f));
    tree.SetImage(frame, Framed());

    const DrawList drawn = Draw(tree, FixtureFont(), kReference, 2.f);
    const QuadInstance *sliced = FirstOfKind(drawn, QuadKind::NineSlice);
    REQUIRE(sliced != nullptr);
    CHECK(sliced->rect.width == 600.f);
    // Two device pixels to a UI pixel: the corners on the quad are twice the
    // texels they show.
    CHECK(sliced->slice[0] == PackSliceInsets(8, 6));
    CHECK(sliced->slice[1] == PackSliceInsets(4, 2));
    CHECK(sliced->slice[2] == PackSliceInsets(4, 3));
    CHECK(sliced->slice[3] == PackSliceInsets(2, 1));
}

TEST_CASE("Draw: a nine-slice too small for its corners shrinks them in proportion")
{
    NodeTree tree;
    const NodeId frame = tree.Create(tree.Root());
    tree.SetStyle(frame, Filled(3.f, 2.f, 0.f));
    tree.SetImage(frame, Framed());

    const DrawList drawn = Draw(tree, FixtureFont());
    const QuadInstance *sliced = FirstOfKind(drawn, QuadKind::NineSlice);
    REQUIRE(sliced != nullptr);
    // Across, 4 + 2 shown in 3 pixels; down, 3 + 1 in 2: each pair halves.
    CHECK(sliced->slice[0] == PackSliceInsets(2, 1));
    CHECK(sliced->slice[1] == PackSliceInsets(1, 0));
    CHECK(sliced->slice[2] == PackSliceInsets(4, 3));
}

TEST_CASE("Draw: a background image covers the whole node over its background colour, tinted")
{
    NodeTree tree;
    const NodeId panel = tree.Create(tree.Root());
    Style style = Filled(120.f, 80.f, 0.5f);
    style.padding = Padding::All(Px(10.f));
    style.tint = kTint;
    tree.SetStyle(panel, style);
    Node *node = tree.Editable(panel);
    node->background = Framed();
    node->hasBackgroundImage = true;

    const DrawList drawn = Draw(tree, FixtureFont());
    const std::optional<std::size_t> colour = QuadWithRed(drawn, 0.5f);
    const QuadInstance *sliced = FirstOfKind(drawn, QuadKind::NineSlice);
    REQUIRE(colour.has_value());
    REQUIRE(sliced != nullptr);
    CHECK(sliced > &drawn.Instances()[*colour]);
    CHECK(sliced->rect.width == 120.f);
    CHECK(sliced->rect.height == 80.f);
    CHECK(sliced->color == kTint);
}

TEST_CASE("Draw: what a control paints behind the text comes before the glyphs it marks")
{
    NodeTree tree;
    RegisterBuiltinWidgets(tree.Widgets());

    constexpr float kFieldWidth = 200.f;
    const NodeId field = tree.Create(tree.Root());
    Style style;
    style.sizing = {Sizing::Fixed(Px(kFieldWidth)), Sizing::Fit()};
    tree.SetStyle(field, style);
    tree.SetText(field, "AAA");
    tree.SetBehaviour(field, static_cast<uint32_t>(BuiltinWidget::TextField));
    tree.SetFocusable(field, true);

    TextEdit edit;
    edit.editing = TextEditing::Editable;
    edit.anchor = 0;
    edit.caret = 3; // all of it selected, so there is a highlight to find
    tree.SetTextEdit(field, edit);

    const Font font = FixtureFont();
    LayoutResult layout;
    ComputeLayout(tree, kReference, 1.f, &font, layout);
    DrawList list;
    Interaction interaction;
    interaction.focused = field;
    DrawTree(tree, layout, list, kAtlas, interaction);

    const auto quads = list.Instances();
    const auto glyph = std::ranges::find_if(quads, [](const QuadInstance &quad)
                                            { return quad.kind == static_cast<uint32_t>(QuadKind::Glyph); });
    REQUIRE(glyph != quads.end());

    // The highlight is the quad drawn just before the first glyph: painted
    // after them, it would cover the words it is meant to mark. It covers the
    // three letters rather than the whole box, which is what tells it apart
    // from the field's own background.
    REQUIRE(glyph != quads.begin());
    const QuadInstance &highlight = *(glyph - 1);
    CHECK(highlight.kind != static_cast<uint32_t>(QuadKind::Glyph));
    CHECK(highlight.rect.width > 0.f);
    CHECK(highlight.rect.width < kFieldWidth);
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
