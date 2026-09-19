/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>

using namespace Assisi::Mondrian;
using Assisi::Mondrian::Testing::FixtureFont;

namespace
{

/// A frame of each shape the UI must handle: a common window, and one narrower
/// than it is tall so a layout that mixes up the axes shows it.
constexpr Extent kLandscape{1280, 720};
constexpr Extent kPortrait{480, 960};

constexpr TextureId kPlaceholder{7};
constexpr TextureId kFontTexture{9};

/// Whether @p rect covers some pixels and all of them lie inside @p viewport.
bool VisibleWithin(const Rect &rect, Extent viewport)
{
    return rect.width > 0.f && rect.height > 0.f && rect.x >= 0.f && rect.y >= 0.f &&
           rect.x + rect.width <= static_cast<float>(viewport.width) &&
           rect.y + rect.height <= static_cast<float>(viewport.height);
}

/// Runs one whole frame and returns what it drew.
const DrawList &Frame(Ui &ui, Extent viewport)
{
    ui.ProcessInput();
    ui.Sync(viewport);
    return ui.GetDrawList();
}

bool IsKind(const QuadInstance &quad, QuadKind kind)
{
    return quad.kind == static_cast<uint32_t>(kind);
}

/// The sample panel's rect after a frame at @p viewport.
Rect PanelAt(Ui &ui, Extent viewport)
{
    Frame(ui, viewport);
    const LayoutNode *panel = ui.GetLayout().Get(ui.Tree().Find("panel"));
    REQUIRE(panel != nullptr);
    return panel->rect;
}

} // namespace

TEST_CASE("Mondrian: the sample screen draws boxes, an image and text inside the viewport")
{
    const Font font = FixtureFont();
    for (const Extent viewport : {kLandscape, kPortrait})
    {
        CAPTURE(viewport.width);
        Ui ui;
        ui.SetFont(&font, kFontTexture);
        ui.SetPlaceholderTexture(kPlaceholder);
        const DrawList &drawn = Frame(ui, viewport);

        REQUIRE(drawn.IsFinalized());
        const auto quads = drawn.Instances();
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Solid); }));
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Image); }));
        CHECK(std::ranges::any_of(quads, [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Glyph); }));
        for (const QuadInstance &quad : quads)
        {
            if (!IsKind(quad, QuadKind::Glyph))
            {
                CHECK(VisibleWithin(quad.rect, viewport));
            }
        }
        CHECK(
            std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kFontTexture; }));
        CHECK(
            std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPlaceholder; }));
    }
}

TEST_CASE("Mondrian: the sample screen reflows when the viewport changes")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    const Rect landscape = PanelAt(ui, kLandscape);
    const Rect portrait = PanelAt(ui, kPortrait);
    CHECK(landscape.width != portrait.width);
    CHECK(VisibleWithin(portrait, kPortrait));
}

TEST_CASE("Mondrian: the player's UI scale enlarges the screen")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    const Rect normal = PanelAt(ui, kLandscape);
    ui.SetUserScale(1.5f);
    CHECK(ui.GetUserScale() == 1.5f);
    const Rect larger = PanelAt(ui, kLandscape);
    CHECK(larger.height > normal.height);
}

TEST_CASE("Mondrian: without a font the screen draws no text")
{
    Ui ui;
    const DrawList &drawn = Frame(ui, kLandscape);
    CHECK_FALSE(
        std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad) { return IsKind(quad, QuadKind::Glyph); }));
    CHECK_FALSE(drawn.Instances().empty());
}

TEST_CASE("Mondrian: a zero-sized viewport draws nothing and does not assert")
{
    const Font font = FixtureFont();
    Ui ui;
    ui.SetFont(&font, kFontTexture);
    CHECK(Frame(ui, Extent{0, 0}).Instances().empty());
}

#ifndef NDEBUG
TEST_CASE("Mondrian: the two frame steps must alternate, input first")
{
    const Assisi::Testing::ThrowOnContractViolation guard;

    SUBCASE("sync before any input asserts")
    {
        Ui ui;
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("input twice without a sync asserts")
    {
        Ui ui;
        ui.ProcessInput();
        CHECK_THROWS_AS(ui.ProcessInput(), Assisi::Core::ContractViolation);
    }

    SUBCASE("sync twice without input asserts")
    {
        Ui ui;
        ui.ProcessInput();
        ui.Sync(kLandscape);
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("alternating frames are accepted")
    {
        Ui ui;
        CHECK_NOTHROW(Frame(ui, kLandscape));
        CHECK_NOTHROW(Frame(ui, kPortrait));
    }
}
#endif
