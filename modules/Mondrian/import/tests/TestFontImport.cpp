/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Mondrian/Import/FontImport.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;

namespace
{

/// Printable ASCII, the range every Latin font covers.
constexpr uint32_t kFirstPrintable = 0x20;
constexpr uint32_t kLastPrintable  = 0x7E;
constexpr uint32_t kSpace          = 0x20;

/// Small enough to rasterise quickly; the spread is the shipped description's.
constexpr float kTestPixelSize = 32.f;
constexpr uint32_t kTestSpread = 8;

/// A distance field stores the outline at this value and higher inside it.
constexpr uint8_t kSdfOutline = 128;

std::vector<std::byte> ReadFile(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

FontDescription AsciiDescription()
{
    FontDescription description;
    description.ranges    = {kFirstPrintable, kLastPrintable};
    description.source.Assign("fonts/Inter-Regular.ttf");
    description.pixelSize = kTestPixelSize;
    description.spread    = kTestSpread;
    return description;
}

bool Overlap(const Glyph &a, const Glyph &b)
{
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

} // namespace

TEST_CASE("FontImport: a description document reads into its fields")
{
    const char *text = R"({ "version": 1, "type": "FontDescription", "source": "fonts/Inter-Regular.ttf",
                            "ranges": [32, 126, 160, 255], "pixelSize": 40, "spread": 6 })";
    const std::expected<FontDescription, FontImportError> parsed = ParseFontDescription(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->source.View() == "fonts/Inter-Regular.ttf");
    CHECK(parsed->ranges == std::vector<uint32_t>{32, 126, 160, 255});
    CHECK(parsed->pixelSize == 40.f);
    CHECK(parsed->spread == 6u);
}

TEST_CASE("FontImport: a description that cannot be rasterised is refused with its reason")
{
    const auto parse = [](const char *fields)
    {
        const std::string text =
            std::string(R"({ "version": 1, "type": "FontDescription", )") + fields + " }";
        return ParseFontDescription(text);
    };

    CHECK(parse(R"("source": "a.ttf", "ranges": [32])").error() == FontImportError::BadRanges);
    CHECK(parse(R"("source": "a.ttf", "ranges": [])").error() == FontImportError::BadRanges);
    CHECK(parse(R"("source": "a.ttf", "ranges": [126, 32])").error() == FontImportError::BadRanges);
    CHECK(parse(R"("source": "a.ttf", "ranges": [32, 126], "spread": 0)").error() == FontImportError::BadSpread);
    CHECK(parse(R"("source": "a.ttf", "ranges": [32, 126], "spread": 64)").error() == FontImportError::BadSpread);
    CHECK(parse(R"("source": "a.ttf", "ranges": [32, 126], "pixelSize": 0)").error() == FontImportError::BadSize);
    CHECK(parse(R"("ranges": [32, 126])").error() == FontImportError::MissingSource);
    CHECK(ParseFontDescription(R"({ "version": 1, "type": "AppConfig" })").error() ==
          FontImportError::InvalidDocument);
}

TEST_CASE("FontImport: every requested character gets a glyph, packed without overlap")
{
    const std::expected<Font, FontImportError> font =
        RasterizeFont(AsciiDescription(), ReadFile(ASSISI_TEST_FONT_PATH));
    REQUIRE(font.has_value());

    CHECK(font->kind == FontKind::Sdf);
    CHECK(font->pixelSize == kTestPixelSize);
    CHECK(font->spread == kTestSpread);
    CHECK(font->lineHeight > 0.f);
    CHECK(font->ascender > 0.f);
    CHECK(font->descender < 0.f);

    for (uint32_t codepoint = kFirstPrintable; codepoint <= kLastPrintable; ++codepoint)
    {
        const std::optional<uint32_t> glyph = font->GlyphFor(codepoint);
        REQUIRE(glyph.has_value());
        CHECK(font->FindGlyph(*glyph) != nullptr);
    }
    // The font's own missing-glyph glyph, which text falls back to.
    CHECK(font->FindGlyph(0) != nullptr);

    // A space advances the pen and has no image.
    const Glyph *space = font->FindGlyph(*font->GlyphFor(kSpace));
    REQUIRE(space != nullptr);
    CHECK(space->advance > 0.f);
    CHECK(space->width == 0);

    for (std::size_t i = 0; i < font->glyphs.size(); ++i)
    {
        const Glyph &a = font->glyphs[i];
        CHECK(static_cast<uint32_t>(a.x) + a.width <= font->atlasWidth);
        CHECK(static_cast<uint32_t>(a.y) + a.height <= font->atlasHeight);
        for (std::size_t j = i + 1; j < font->glyphs.size(); ++j)
        {
            CHECK_FALSE(Overlap(a, font->glyphs[j]));
        }
    }

    // Ink landed in the atlas: a distance field with nothing inside an outline
    // is the wrong render mode or an empty copy.
    CHECK(std::ranges::any_of(font->atlas, [](uint8_t texel) { return texel > kSdfOutline; }));
}

TEST_CASE("FontImport: a rasterised font is one the runtime reader accepts")
{
    const std::expected<Font, FontImportError> font =
        RasterizeFont(AsciiDescription(), ReadFile(ASSISI_TEST_FONT_PATH));
    REQUIRE(font.has_value());

    Assisi::Core::BitWriter writer;
    WriteCookedFont(writer, *font);
    const std::expected<Font, CookedFontError> read = ReadCookedFont(writer.Data());
    REQUIRE(read.has_value());
    CHECK(read->glyphs.size() == font->glyphs.size());
}

TEST_CASE("FontImport: rasterising the same font twice gives the same bytes")
{
    const std::vector<std::byte> file = ReadFile(ASSISI_TEST_FONT_PATH);
    const std::expected<Font, FontImportError> first  = RasterizeFont(AsciiDescription(), file);
    const std::expected<Font, FontImportError> second = RasterizeFont(AsciiDescription(), file);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    Assisi::Core::BitWriter a;
    Assisi::Core::BitWriter b;
    WriteCookedFont(a, *first);
    WriteCookedFont(b, *second);
    CHECK(std::ranges::equal(a.Data(), b.Data()));
}

TEST_CASE("FontImport: bytes that are not a font are refused")
{
    const std::vector<std::byte> garbage(64, std::byte{0x5A});
    const std::expected<Font, FontImportError> font = RasterizeFont(AsciiDescription(), garbage);
    REQUIRE_FALSE(font.has_value());
    CHECK(font.error() == FontImportError::UnreadableFont);
}
