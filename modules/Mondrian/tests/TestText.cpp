/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Mondrian/Text.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;

namespace
{

constexpr uint32_t kAtlasSide = 64;
constexpr float kCookedSize = 32.f;
constexpr float kAscender = 29.f;
constexpr float kLineHeight = 39.f;

/// Glyph indices, out of codepoint order as a real font's are.
constexpr uint32_t kNotdef = 0;
constexpr uint32_t kA = 10;
constexpr uint32_t kS = 20;
constexpr uint32_t kI = 30;
constexpr uint32_t kSpace = 40;
constexpr uint32_t kEAcute = 50;

/// Advances, whole pixels at the cooked size so expected positions are exact.
constexpr float kAAdvance = 20.f;
constexpr float kSAdvance = 16.f;
constexpr float kIAdvance = 8.f;
constexpr float kSpaceAdvance = 8.f;

/// A font that maps A, s, i, e acute, space and no-break space, lacks sharp s,
/// and has a drawable missing glyph. No kerning, so layout arithmetic reads
/// straight off the advances.
Font PlainFont()
{
    Font font;
    font.pixelSize = kCookedSize;
    font.ascender = kAscender;
    font.descender = -7.f;
    font.lineHeight = kLineHeight;
    font.atlasWidth = kAtlasSide;
    font.atlasHeight = kAtlasSide;
    font.atlas.assign(static_cast<std::size_t>(kAtlasSide) * kAtlasSide, 0);
    font.glyphs = {
        Glyph{.advance = 24.f,
              .index = kNotdef,
              .x = 0,
              .y = 0,
              .width = 10,
              .height = 12,
              .bearingX = 1,
              .bearingY = 23},
        Glyph{.advance = kAAdvance,
              .index = kA,
              .x = 10,
              .y = 0,
              .width = 8,
              .height = 10,
              .bearingX = 1,
              .bearingY = 23},
        Glyph{
            .advance = kSAdvance, .index = kS, .x = 18, .y = 0, .width = 6, .height = 8, .bearingX = 1, .bearingY = 17},
        Glyph{.advance = kIAdvance,
              .index = kI,
              .x = 24,
              .y = 0,
              .width = 3,
              .height = 10,
              .bearingX = 2,
              .bearingY = 23},
        Glyph{.advance = kSpaceAdvance, .index = kSpace},
        Glyph{.advance = 16.f,
              .index = kEAcute,
              .x = 27,
              .y = 0,
              .width = 6,
              .height = 12,
              .bearingX = 1,
              .bearingY = 24},
    };
    font.cmap = {CmapEntry{.codepoint = ' ', .glyph = kSpace},  CmapEntry{.codepoint = 'A', .glyph = kA},
                 CmapEntry{.codepoint = 'i', .glyph = kI},      CmapEntry{.codepoint = 's', .glyph = kS},
                 CmapEntry{.codepoint = 0xA0, .glyph = kSpace}, CmapEntry{.codepoint = 0xE9, .glyph = kEAcute}};
    return font;
}

constexpr float kAIKerning = -2.f;

/// PlainFont with A pulled towards a following i.
Font KernedFont()
{
    Font font = PlainFont();
    font.kerning = {KerningPair{.adjust = kAIKerning, .left = kA, .right = kI}};
    return font;
}

// Text in the tests, spelled in bytes so the source's encoding cannot change it.
constexpr std::string_view kEAcuteUtf8 = "\xC3\xA9";
constexpr std::string_view kNoBreakSpaceUtf8 = "\xC2\xA0";
constexpr std::string_view kSharpSUtf8 = "\xC3\x9F";

TextLayout Lay(const Font &font, std::string_view text, float size, std::optional<float> wrap = std::nullopt,
               TextAlign align = TextAlign::Left)
{
    return LayoutText(Shape(text, font), font, size, wrap, align);
}

} // namespace

TEST_CASE("Text: shaping yields one glyph a codepoint, not a byte, and keeps where each came from")
{
    const Font font = PlainFont();
    const std::string text = std::string("A") + std::string(kEAcuteUtf8) + "s";
    const ShapedText shaped = Shape(text, font);

    REQUIRE(shaped.glyphs.size() == 3);
    CHECK(shaped.glyphs[0].index == kA);
    CHECK(shaped.glyphs[1].index == kEAcute);
    CHECK(shaped.glyphs[2].index == kS);
    CHECK(shaped.glyphs[0].cluster == 0);
    CHECK(shaped.glyphs[1].cluster == 1);
    CHECK(shaped.glyphs[2].cluster == 3);
}

TEST_CASE("Text: what the font cannot draw becomes its missing glyph")
{
    const Font font = PlainFont();

    const ShapedText unmapped = Shape(kSharpSUtf8, font);
    REQUIRE(unmapped.glyphs.size() == 1);
    CHECK(unmapped.glyphs[0].index == kNotdef);
    CHECK(unmapped.glyphs[0].advance == 24.f);

    const ShapedText invalid = Shape("A\xFF", font);
    REQUIRE(invalid.glyphs.size() == 2);
    CHECK(invalid.glyphs[1].index == kNotdef);
    CHECK(invalid.glyphs[1].cluster == 1);
}

TEST_CASE("Text: kerning moves the pen between a pair, in that order only")
{
    const Font font = KernedFont();

    const ShapedText ai = Shape("Ai", font);
    REQUIRE(ai.glyphs.size() == 2);
    CHECK(ai.glyphs[0].advance == kAAdvance + kAIKerning);

    const ShapedText ia = Shape("iA", font);
    REQUIRE(ia.glyphs.size() == 2);
    CHECK(ia.glyphs[0].advance == kIAdvance);
}

TEST_CASE("Text: spaces may break, a newline ends a line, a no-break space is ink")
{
    const Font font = PlainFont();
    const std::string text = std::string(" \n") + std::string(kNoBreakSpaceUtf8);
    const ShapedText shaped = Shape(text, font);

    REQUIRE(shaped.glyphs.size() == 3);
    CHECK(shaped.glyphs[0].kind == GlyphClass::Whitespace);
    CHECK(shaped.glyphs[1].kind == GlyphClass::Newline);
    CHECK(shaped.glyphs[1].advance == 0.f);
    CHECK(shaped.glyphs[2].kind == GlyphClass::Ink);
    CHECK(shaped.glyphs[2].index == kSpace);
}

TEST_CASE("Text: an unwrapped line is measured by its advances, scaled to the size asked")
{
    const Font font = PlainFont();

    const TextLayout cooked = Lay(font, "Ais", kCookedSize);
    REQUIRE(cooked.lines.size() == 1);
    REQUIRE(cooked.glyphs.size() == 3);
    CHECK(cooked.glyphs[0].x == 0.f);
    CHECK(cooked.glyphs[1].x == kAAdvance);
    CHECK(cooked.glyphs[2].x == kAAdvance + kIAdvance);
    CHECK(cooked.glyphs[0].y == kAscender);
    CHECK(cooked.width == kAAdvance + kIAdvance + kSAdvance);
    CHECK(cooked.height == kLineHeight);

    const TextLayout doubled = Lay(font, "Ais", 2.f * kCookedSize);
    CHECK(doubled.glyphs[2].x == 2.f * cooked.glyphs[2].x);
    CHECK(doubled.glyphs[0].y == 2.f * kAscender);
    CHECK(doubled.width == 2.f * cooked.width);
    CHECK(doubled.height == 2.f * kLineHeight);
}

TEST_CASE("Text: the longest word is the narrowest a text can wrap to")
{
    const Font font = PlainFont();
    const ShapedText shaped = Shape("Ai sAis\nA", font); // words of 28, 60 and 20
    CHECK(MeasureLongestWord(shaped, font, kCookedSize) == 60.f);
    CHECK(MeasureLongestWord(shaped, font, 2.f * kCookedSize) == 120.f);
    CHECK(MeasureLongestWord(shaped, font, 30.f) == 57.f); // 56.25, rounded up
    CHECK(MeasureLongestWord(Shape("", font), font, kCookedSize) == 0.f);
}

TEST_CASE("Text: kerning reaches the layout")
{
    const TextLayout layout = Lay(KernedFont(), "Ai", kCookedSize);
    REQUIRE(layout.glyphs.size() == 2);
    CHECK(layout.glyphs[1].x == kAAdvance + kAIKerning);
}

TEST_CASE("Text: an empty string is one empty line")
{
    const TextLayout layout = Lay(PlainFont(), "", kCookedSize);
    REQUIRE(layout.lines.size() == 1);
    CHECK(layout.lines[0].count == 0);
    CHECK(layout.width == 0.f);
    CHECK(layout.height == kLineHeight);
}

TEST_CASE("Text: wrapping breaks after spaces, and a line's trailing space is placed but not measured")
{
    const Font font = PlainFont();
    constexpr float kWrap = 50.f; // room for "AA " but not "AA A"
    const TextLayout layout = Lay(font, "AA AA AA", kCookedSize, kWrap);

    REQUIRE(layout.lines.size() == 3);
    REQUIRE(layout.glyphs.size() == 8);
    for (const TextLine &line : layout.lines)
    {
        CHECK(line.width == 2.f * kAAdvance);
    }
    CHECK(layout.lines[0].first == 0);
    CHECK(layout.lines[0].count == 3);
    CHECK(layout.lines[1].first == 3);
    CHECK(layout.lines[1].count == 3);
    CHECK(layout.lines[2].first == 6);
    CHECK(layout.lines[2].count == 2);

    CHECK(layout.lines[0].baseline == kAscender);
    CHECK(layout.lines[1].baseline == kAscender + kLineHeight);
    CHECK(layout.lines[2].baseline == kAscender + 2.f * kLineHeight);
    CHECK(layout.height == 3.f * kLineHeight);
    CHECK(layout.width == 2.f * kAAdvance);

    // The space ending the first line sits after its ink; the next word starts the next line.
    CHECK(layout.glyphs[2].x == 2.f * kAAdvance);
    CHECK(layout.glyphs[2].y == kAscender);
    CHECK(layout.glyphs[3].x == 0.f);
    CHECK(layout.glyphs[3].y == kAscender + kLineHeight);
}

TEST_CASE("Text: trailing whitespace does not widen an unwrapped line")
{
    CHECK(Lay(PlainFont(), "AA  ", kCookedSize).width == 2.f * kAAdvance);
}

TEST_CASE("Text: a no-break space never breaks a line")
{
    const std::string text = "A" + std::string(kNoBreakSpaceUtf8) + "AAA";
    constexpr float kWrap = 70.f; // "A" nbsp "AA" is 68
    const TextLayout layout = Lay(PlainFont(), text, kCookedSize, kWrap);

    // Breaking at the no-break space would leave two glyphs on the first line;
    // the word breaks where it overflows instead.
    REQUIRE(layout.lines.size() == 2);
    CHECK(layout.lines[0].count == 4);
    CHECK(layout.lines[1].count == 1);
}

TEST_CASE("Text: a word wider than the wrap breaks where it overflows, never leaving a line empty")
{
    const Font font = PlainFont();

    const TextLayout halves = Lay(font, "AAAA", kCookedSize, 50.f);
    REQUIRE(halves.lines.size() == 2);
    CHECK(halves.lines[0].count == 2);
    CHECK(halves.lines[1].count == 2);

    // Narrower than one glyph: each line still takes one.
    const TextLayout singles = Lay(font, "AAA", kCookedSize, 10.f);
    REQUIRE(singles.lines.size() == 3);
    for (const TextLine &line : singles.lines)
    {
        CHECK(line.count == 1);
    }
}

TEST_CASE("Text: a newline ends its line, and one at the end starts an empty line")
{
    const Font font = PlainFont();

    const TextLayout two = Lay(font, "A\nA", kCookedSize);
    REQUIRE(two.lines.size() == 2);
    CHECK(two.lines[0].count == 2);
    CHECK(two.lines[0].width == kAAdvance);
    CHECK(two.glyphs[1].x == kAAdvance);
    CHECK(two.glyphs[2].y == kAscender + kLineHeight);

    const TextLayout trailing = Lay(font, "A\n", kCookedSize);
    REQUIRE(trailing.lines.size() == 2);
    CHECK(trailing.lines[1].count == 0);
    CHECK(trailing.height == 2.f * kLineHeight);
}

TEST_CASE("Text: lines align within the wrap width, or within the widest line without one")
{
    const Font font = PlainFont();
    constexpr float kWrap = 100.f;

    const TextLayout left = Lay(font, "A\nAA", kCookedSize, kWrap, TextAlign::Left);
    REQUIRE(left.lines.size() == 2);
    CHECK(left.lines[0].x == 0.f);
    CHECK(left.lines[1].x == 0.f);

    const TextLayout center = Lay(font, "A\nAA", kCookedSize, kWrap, TextAlign::Center);
    REQUIRE(center.lines.size() == 2);
    REQUIRE(center.glyphs.size() == 4);
    CHECK(center.lines[0].x == 40.f);
    CHECK(center.lines[1].x == 30.f);
    CHECK(center.glyphs[2].x == 30.f); // the second line's glyphs move with it

    const TextLayout right = Lay(font, "A\nAA", kCookedSize, kWrap, TextAlign::Right);
    REQUIRE(right.lines.size() == 2);
    CHECK(right.lines[0].x == 80.f);
    CHECK(right.lines[1].x == 60.f);

    const TextLayout unwrapped = Lay(font, "A\nAA", kCookedSize, std::nullopt, TextAlign::Center);
    REQUIRE(unwrapped.lines.size() == 2);
    CHECK(unwrapped.lines[0].x == 10.f);
    CHECK(unwrapped.lines[1].x == 0.f);

    // A glyph wider than the wrap overflows to the right, not off the left.
    const TextLayout overflow = Lay(font, "A", kCookedSize, 10.f, TextAlign::Right);
    REQUIRE(overflow.lines.size() == 1);
    CHECK(overflow.lines[0].x == 0.f);
}

TEST_CASE("Text: at a fractional scale, edges and the measurement land on whole pixels")
{
    const Font font = PlainFont();
    constexpr float kSize = 30.f; // scale 0.9375

    const TextLayout layout = Lay(font, "A\nA", kSize);
    CHECK(layout.width == 19.f);  // 18.75 rounded up
    CHECK(layout.height == 74.f); // 73.125 rounded up
    REQUIRE(layout.lines.size() == 2);
    CHECK(layout.lines[0].baseline == 27.f); // 27.1875
    CHECK(layout.lines[1].baseline == 64.f); // 63.75

    const TextLayout centered = Lay(font, "A", kSize, 51.f, TextAlign::Center);
    REQUIRE(centered.lines.size() == 1);
    CHECK(centered.lines[0].x == std::round(centered.lines[0].x));
}

namespace
{

constexpr TextureId kAtlas{5};
constexpr Point kOrigin{.x = 100.f, .y = 200.f};
constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kInk{0.2f, 0.4f, 0.6f, 0.8f};

} // namespace

TEST_CASE("Text: drawing puts each glyph's image at the pen layout placed it")
{
    const Font font = PlainFont();
    const TextLayout layout = Lay(font, "Ai ", kCookedSize);
    DrawList list;
    DrawGlyphs(list, layout, kAtlas, kOrigin, kInk);
    list.Finalize();

    // The space has no image.
    REQUIRE(list.Instances().size() == 2);
    const QuadInstance &a = list.Instances()[0];
    const QuadInstance &i = list.Instances()[1];

    CHECK(a.kind == static_cast<uint32_t>(QuadKind::Glyph));
    CHECK(a.rect.x == kOrigin.x + 1.f);
    CHECK(a.rect.y == kOrigin.y + kAscender - 23.f);
    CHECK(a.rect.width == 8.f);
    CHECK(a.rect.height == 10.f);
    CHECK(a.uv.x == doctest::Approx(10.f / kAtlasSide));
    CHECK(a.uv.width == doctest::Approx(8.f / kAtlasSide));
    CHECK(a.uv.height == doctest::Approx(10.f / kAtlasSide));
    CHECK(a.color.r == kInk.r);
    CHECK(a.color.a == kInk.a);

    CHECK(i.rect.x == kOrigin.x + layout.glyphs[1].x + 2.f);

    REQUIRE(list.Entries().size() == 1);
    CHECK(list.Entries()[0].texture == kAtlas);
    CHECK(list.Entries()[0].instanceCount == 2);
}

TEST_CASE("Text: drawn glyphs scale with the size, from the same texels")
{
    const Font font = PlainFont();
    const TextLayout layout = Lay(font, "A", 2.f * kCookedSize);
    DrawList list;
    DrawGlyphs(list, layout, kAtlas, kOrigin, kInk);

    REQUIRE(list.Instances().size() == 1);
    CHECK(list.Instances()[0].rect.x == kOrigin.x + 2.f);
    CHECK(list.Instances()[0].rect.width == 16.f);
    CHECK(list.Instances()[0].uv.width == doctest::Approx(8.f / kAtlasSide));
}

TEST_CASE("Text: a newline draws nothing, and the missing glyph draws its box")
{
    const Font font = PlainFont();

    DrawList newline;
    DrawGlyphs(newline, Lay(font, "\n", kCookedSize), kAtlas, kOrigin, kInk);
    CHECK(newline.Instances().empty());

    DrawList missing;
    DrawGlyphs(missing, Lay(font, kSharpSUtf8, kCookedSize), kAtlas, kOrigin, kInk);
    CHECK(missing.Instances().size() == 1);
}

TEST_CASE("Text: every drawn glyph's pen lies inside the measured box")
{
    const Font font = PlainFont();
    const std::string text = "As " + std::string(kEAcuteUtf8) + "is\nsAis AAs iiA";
    for (const TextAlign align : {TextAlign::Left, TextAlign::Center, TextAlign::Right})
    {
        const TextLayout layout = Lay(font, text, 45.f, 120.f, align);
        const float box = 120.f;
        REQUIRE(layout.lines.size() > 2); // the text wraps, beyond its one newline
        for (const TextLine &line : layout.lines)
        {
            CHECK(line.x >= 0.f);
            CHECK(line.x + line.width <= box);
            CHECK(line.baseline <= layout.height);
        }
        CHECK(layout.width <= box);
    }
}

TEST_CASE("Text: the width a word is measured at is a width that word fits in")
{
    // Layout gives a node at least this much room for its longest word, then
    // wraps the text at what the node ended up with. The two have to be the
    // same arithmetic. A sum scaled once and a sum of scaled parts are the same
    // number written down and different ones in floating point, and a word
    // measured a hair narrower than it is laid out has nowhere to break but the
    // middle of itself.
    const Font font = PlainFont();
    const ShapedText shaped = Shape("sAisA", font); // one word, nowhere to break

    // Sizes the cooked advances do not divide into whole pixels, which is where
    // the two orders of arithmetic part company.
    constexpr int32_t kSteps = 128;
    for (int32_t step = 1; step <= kSteps; ++step)
    {
        const float size = static_cast<float>(step) * kCookedSize / static_cast<float>(kSteps);
        const float measured = MeasureLongestWord(shaped, font, size);

        const TextLayout laid = LayoutText(shaped, font, size, std::nullopt, TextAlign::Left);
        REQUIRE(laid.lines.size() == 1);

        INFO("size: " << size);
        CHECK(laid.lines[0].width <= measured);
    }
}
