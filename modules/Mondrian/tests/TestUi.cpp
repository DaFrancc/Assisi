/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>

using namespace Assisi::Mondrian;

namespace
{

/// A frame of each shape the UI must handle: a common window, and one narrower
/// than it is tall so a layout that mixes up the axes lands out of view.
constexpr Extent kLandscape{1280, 720};
constexpr Extent kPortrait{480, 960};

constexpr TextureId kPlaceholder{7};

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

/// Whether some quad in @p list has a corner of @p style.
bool HasCornerStyle(const DrawList &list, CornerStyle style)
{
    return std::ranges::any_of(list.Instances(), [style](const QuadInstance &quad)
                               { return UnpackCornerStyle(quad.cornerStyles, Corner::TopLeft) == style; });
}

} // namespace

TEST_CASE("Mondrian: the placeholder strip shows every quad the pass draws, inside the viewport")
{
    Ui ui;
    ui.SetPlaceholderTexture(kPlaceholder);
    const DrawList &drawn = Frame(ui, kLandscape);

    REQUIRE(drawn.IsFinalized());
    REQUIRE_FALSE(drawn.Instances().empty());
    for (const QuadInstance &quad : drawn.Instances())
    {
        CHECK(VisibleWithin(quad.rect, kLandscape));
    }

    CHECK(HasCornerStyle(drawn, CornerStyle::Square));
    CHECK(HasCornerStyle(drawn, CornerStyle::Rounded));
    CHECK(HasCornerStyle(drawn, CornerStyle::Cut));
    CHECK(std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad) { return quad.borderWidth > 0.f; }));

    // Clipped: the clip cuts into the quad rather than containing it.
    CHECK(std::ranges::any_of(drawn.Instances(),
                              [](const QuadInstance &quad)
                              { return quad.clip.width < quad.rect.width && quad.clip.width > 0.f; }));

    // Textured with whatever the engine registered.
    CHECK(std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPlaceholder; }));
    CHECK(std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad)
                              { return quad.kind == static_cast<uint32_t>(QuadKind::Image); }));
}

TEST_CASE("Mondrian: every sync rebuilds the strip against the viewport it is given")
{
    Ui ui;
    const std::size_t landscapeCount = Frame(ui, kLandscape).Instances().size();
    const Rect landscapeFirst        = Frame(ui, kLandscape).Instances()[0].rect;
    const DrawList &portrait         = Frame(ui, kPortrait);

    // A second sync rebuilds rather than appends.
    REQUIRE(portrait.Instances().size() == landscapeCount);
    for (const QuadInstance &quad : portrait.Instances())
    {
        CHECK(VisibleWithin(quad.rect, kPortrait));
    }
    CHECK(portrait.Instances()[0].rect.width != landscapeFirst.width);
}

namespace
{

constexpr TextureId kFontTexture{9};
constexpr uint32_t kFontAtlasSide = 32;
constexpr float kFontPixelSize    = 32.f;

/// A few letters, a space and a missing glyph: whatever the placeholder writes,
/// every character draws as something.
Font SampleFont()
{
    Font font;
    font.pixelSize   = kFontPixelSize;
    font.ascender    = 29.f;
    font.descender   = -7.f;
    font.lineHeight  = 39.f;
    font.atlasWidth  = kFontAtlasSide;
    font.atlasHeight = kFontAtlasSide;
    font.atlas.assign(static_cast<std::size_t>(kFontAtlasSide) * kFontAtlasSide, 0);
    font.glyphs = {
        Glyph{.advance = 18.f, .index = 0, .x = 17, .y = 0, .width = 8, .height = 10, .bearingX = 1, .bearingY = 23},
        Glyph{.advance = 20.f, .index = 10, .x = 0, .y = 0, .width = 8, .height = 10, .bearingX = 1, .bearingY = 23},
        Glyph{.advance = 16.f, .index = 20, .x = 8, .y = 0, .width = 6, .height = 8, .bearingX = 1, .bearingY = 17},
        Glyph{.advance = 8.f, .index = 30, .x = 14, .y = 0, .width = 3, .height = 10, .bearingX = 2, .bearingY = 23},
        Glyph{.advance = 8.f, .index = 40},
    };
    font.cmap = {CmapEntry{.codepoint = ' ', .glyph = 40}, CmapEntry{.codepoint = 'A', .glyph = 10},
                 CmapEntry{.codepoint = 'i', .glyph = 30}, CmapEntry{.codepoint = 's', .glyph = 20}};
    return font;
}

/// Every glyph quad in @p list, in draw order.
std::vector<QuadInstance> GlyphQuads(const DrawList &list)
{
    std::vector<QuadInstance> quads;
    for (const QuadInstance &quad : list.Instances())
    {
        if (quad.kind == static_cast<uint32_t>(QuadKind::Glyph))
        {
            quads.push_back(quad);
        }
    }
    return quads;
}

/// The bottom of the placeholder's tile row: its margin plus a tile, as
/// fractions of the viewport's shorter side.
constexpr float kTileRowBottomFraction = 0.15f;

} // namespace

TEST_CASE("Mondrian: with a font, the placeholder writes text below the tiles, inside the viewport, from its atlas")
{
    const Font font = SampleFont();
    for (const Extent viewport : {kLandscape, kPortrait})
    {
        CAPTURE(viewport.width);
        Ui ui;
        ui.SetPlaceholderFont(&font, kFontTexture);
        const DrawList &drawn = Frame(ui, viewport);

        const std::vector<QuadInstance> glyphs = GlyphQuads(drawn);
        REQUIRE_FALSE(glyphs.empty());
        const float tileRowBottom =
            static_cast<float>(std::min(viewport.width, viewport.height)) * kTileRowBottomFraction;
        for (const QuadInstance &quad : glyphs)
        {
            CHECK(VisibleWithin(quad.rect, viewport));
            CHECK(quad.rect.y > tileRowBottom);
        }
        CHECK(std::ranges::all_of(drawn.Entries(),
                                  [&drawn](const DrawEntry &entry)
                                  {
                                      const QuadInstance &first = drawn.Instances()[entry.firstInstance];
                                      return first.kind != static_cast<uint32_t>(QuadKind::Glyph) ||
                                             entry.texture == kFontTexture;
                                  }));
    }
}

TEST_CASE("Mondrian: without a font, the placeholder writes no text")
{
    Ui ui;
    CHECK(GlyphQuads(Frame(ui, kLandscape)).empty());
}

TEST_CASE("Mondrian: a zero-sized viewport draws nothing and does not assert")
{
    const Font font = SampleFont();
    Ui ui;
    ui.SetPlaceholderFont(&font, kFontTexture);
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
