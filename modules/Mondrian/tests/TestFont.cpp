/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Mondrian/Font.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

constexpr uint32_t kAtlasSide = 16;

/// A small font with every table populated, glyphs of each shape: an ordinary
/// one, one with no image (a space), and one whose index is out of codepoint
/// order, as a real font's are.
Font SampleFont()
{
    Font font;
    font.pixelSize   = 32.f;
    font.ascender    = 29.f;
    font.descender   = -7.f;
    font.lineHeight  = 39.f;
    font.atlasWidth  = kAtlasSide;
    font.atlasHeight = kAtlasSide;
    font.spread      = 4;
    font.kind        = FontKind::Sdf;
    font.atlas.resize(static_cast<std::size_t>(kAtlasSide) * kAtlasSide);
    for (std::size_t i = 0; i < font.atlas.size(); ++i)
    {
        font.atlas[i] = static_cast<uint8_t>(i);
    }

    font.glyphs = {
        Glyph{.advance = 9.f, .index = 3, .x = 0, .y = 0, .width = 0, .height = 0, .bearingX = 0, .bearingY = 0},
        Glyph{.advance = 18.f, .index = 36, .x = 0, .y = 0, .width = 8, .height = 10, .bearingX = -1, .bearingY = 23},
        Glyph{.advance = 17.f, .index = 70, .x = 8, .y = 0, .width = 8, .height = 12, .bearingX = 1, .bearingY = 24},
    };
    font.cmap    = {CmapEntry{.codepoint = 32, .glyph = 3}, CmapEntry{.codepoint = 65, .glyph = 36},
                    CmapEntry{.codepoint = 233, .glyph = 70}};
    font.kerning = {KerningPair{.adjust = -1.5f, .left = 36, .right = 70}};
    return font;
}

std::vector<std::byte> Encode(const Font &font)
{
    Assisi::Core::BitWriter writer;
    WriteCookedFont(writer, font);
    const std::span<const std::byte> bytes = writer.Data();
    return {bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("Font: a cooked font reads back exactly as it was written")
{
    const Font written = SampleFont();
    const std::expected<Font, CookedFontError> read = ReadCookedFont(Encode(written));
    REQUIRE(read.has_value());

    CHECK(read->pixelSize == written.pixelSize);
    CHECK(read->ascender == written.ascender);
    CHECK(read->descender == written.descender);
    CHECK(read->lineHeight == written.lineHeight);
    CHECK(read->atlasWidth == written.atlasWidth);
    CHECK(read->atlasHeight == written.atlasHeight);
    CHECK(read->spread == written.spread);
    CHECK(read->kind == written.kind);
    CHECK(read->atlas == written.atlas);

    REQUIRE(read->glyphs.size() == written.glyphs.size());
    for (std::size_t i = 0; i < written.glyphs.size(); ++i)
    {
        const Glyph &a = read->glyphs[i];
        const Glyph &b = written.glyphs[i];
        CHECK(a.index == b.index);
        CHECK(a.advance == b.advance);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
        CHECK(a.width == b.width);
        CHECK(a.height == b.height);
        CHECK(a.bearingX == b.bearingX);
        CHECK(a.bearingY == b.bearingY);
    }
    REQUIRE(read->cmap.size() == written.cmap.size());
    CHECK(read->cmap[2].codepoint == 233u);
    CHECK(read->cmap[2].glyph == 70u);
    REQUIRE(read->kerning.size() == 1);
    CHECK(read->kerning[0].adjust == -1.5f);
    CHECK(read->kerning[0].left == 36u);
    CHECK(read->kerning[0].right == 70u);
}

TEST_CASE("Font: glyphs are found by glyph index and characters through the cmap")
{
    const Font font = SampleFont();

    REQUIRE(font.FindGlyph(70) != nullptr);
    CHECK(font.FindGlyph(70)->advance == 17.f);
    CHECK(font.FindGlyph(3) != nullptr);
    CHECK(font.FindGlyph(4) == nullptr);

    CHECK(font.GlyphFor(65) == 36u);
    CHECK(font.GlyphFor(233) == 70u);
    CHECK_FALSE(font.GlyphFor(66).has_value());
}

TEST_CASE("Font: kerning is looked up by the ordered pair")
{
    Font font = SampleFont();
    CHECK(font.Kerning(36, 70) == -1.5f);
    CHECK(font.Kerning(70, 36) == 0.f);
    CHECK(font.Kerning(36, 36) == 0.f);

    font.kerning.clear();
    CHECK(font.Kerning(36, 70) == 0.f);
}

TEST_CASE("Font: every prefix of a cooked font is refused as truncated")
{
    const std::vector<std::byte> bytes = Encode(SampleFont());
    // Past the envelope, so each cut lands inside the font's own fields.
    Assisi::Core::BitWriter header;
    Assisi::Core::WriteCookedHeader(header, Assisi::Core::CookedKind::Font);
    const std::size_t headerBytes = header.Data().size();

    for (std::size_t length = headerBytes; length < bytes.size(); ++length)
    {
        const std::expected<Font, CookedFontError> read = ReadCookedFont(std::span{bytes}.first(length));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedFontError::Truncated);
    }
}

TEST_CASE("Font: a blob of another kind is not a font")
{
    Assisi::Core::BitWriter writer;
    Assisi::Core::WriteCookedHeader(writer, Assisi::Core::CookedKind::Texture);
    writer.WriteUInt8(kFontPayloadVersion);
    const std::span<const std::byte> bytes = writer.Data();

    const std::expected<Font, CookedFontError> read = ReadCookedFont(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedFontError::NotAFont);
}

TEST_CASE("Font: a layout version this build does not know is refused")
{
    Assisi::Core::BitWriter writer;
    Assisi::Core::WriteCookedHeader(writer, Assisi::Core::CookedKind::Font);
    writer.WriteUInt8(kFontPayloadVersion + 1);
    const std::span<const std::byte> bytes = writer.Data();

    const std::expected<Font, CookedFontError> read = ReadCookedFont(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedFontError::UnsupportedVersion);
}

TEST_CASE("Font: contents that contradict each other are invalid")
{
    SUBCASE("a glyph whose rect leaves the atlas")
    {
        Font font             = SampleFont();
        font.glyphs[2].x      = kAtlasSide - 2;
        const auto read       = ReadCookedFont(Encode(font));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedFontError::Invalid);
    }

    SUBCASE("an atlas that is not width times height bytes")
    {
        Font font = SampleFont();
        font.atlas.pop_back();
        const auto read = ReadCookedFont(Encode(font));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedFontError::Invalid);
    }

    SUBCASE("glyphs out of index order")
    {
        Font font = SampleFont();
        std::swap(font.glyphs[1], font.glyphs[2]);
        const auto read = ReadCookedFont(Encode(font));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedFontError::Invalid);
    }

    SUBCASE("a cmap entry naming a glyph the font does not have")
    {
        Font font             = SampleFont();
        font.cmap[1].glyph    = 999;
        const auto read       = ReadCookedFont(Encode(font));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedFontError::Invalid);
    }
}
