/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Import/FontImport.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace Assisi::Mondrian::Import
{
namespace
{

/// The spreads FreeType's distance-field renderer accepts.
constexpr uint32_t kMinSpread = 1;
constexpr uint32_t kMaxSpread = 32;

/// Above this a glyph's field would not fit the atlas's 16-bit coordinates at
/// any useful count, and nothing in a UI is drawn this large from one atlas.
constexpr float kMaxPixelSize = 256.f;

/// Codepoints one description may ask for. Past this a pre-baked atlas is the
/// wrong tool; a script that large needs glyphs rasterised as text uses them.
constexpr uint64_t kMaxCodepoints = 0x10000;

/// The highest Unicode codepoint.
constexpr uint32_t kMaxCodepoint = 0x10FFFF;

/// FreeType's fixed-point metrics carry six fractional bits.
constexpr float kFixed26Dot6 = 64.f;

/// Atlas sides tried, doubling from the first until every glyph fits.
constexpr uint32_t kMinAtlasExtent = 256;
constexpr uint32_t kMaxAtlasExtent = 4096;
static_assert(kMaxAtlasExtent <= std::numeric_limits<uint16_t>::max(), "glyph rects are 16-bit");

/// Empty texels between neighbouring glyphs, so linear filtering at one glyph's
/// edge never reads its neighbour.
constexpr uint32_t kGlyphPadding = 1;

/// The glyph every font has at index zero: what a missing character draws as.
constexpr uint32_t kMissingGlyph = 0;

struct LibraryDeleter
{
    void operator()(FT_Library library) const { FT_Done_FreeType(library); }
};
struct FaceDeleter
{
    void operator()(FT_Face face) const { FT_Done_Face(face); }
};
using LibraryHandle = std::unique_ptr<std::remove_pointer_t<FT_Library>, LibraryDeleter>;
using FaceHandle    = std::unique_ptr<std::remove_pointer_t<FT_Face>, FaceDeleter>;

/// One rendered glyph before it has a place in the atlas.
struct Rendered
{
    std::vector<uint8_t> texels; ///< width * height, row by row
    Glyph glyph;
};

bool RangesAreValid(const std::vector<uint32_t> &ranges)
{
    if (ranges.empty() || ranges.size() % 2 != 0)
    {
        return false;
    }
    uint64_t total = 0;
    for (std::size_t i = 0; i < ranges.size(); i += 2)
    {
        if (ranges[i] > ranges[i + 1] || ranges[i + 1] > kMaxCodepoint)
        {
            return false;
        }
        total += static_cast<uint64_t>(ranges[i + 1]) - ranges[i] + 1;
    }
    return total <= kMaxCodepoints;
}

/// Renders glyph @p index as a distance field. A glyph with no outline — a
/// space — has no image and only its advance.
std::expected<Rendered, FontImportError> RenderGlyph(FT_Face face, uint32_t index)
{
    if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING) != 0)
    {
        return std::unexpected(FontImportError::RasterizeFailed);
    }

    Rendered rendered;
    rendered.glyph.index   = index;
    rendered.glyph.advance = static_cast<float>(face->glyph->advance.x) / kFixed26Dot6;
    if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points == 0)
    {
        return rendered;
    }

    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != 0)
    {
        return std::unexpected(FontImportError::RasterizeFailed);
    }

    const FT_Bitmap &bitmap = face->glyph->bitmap;
    if (bitmap.width > kMaxAtlasExtent || bitmap.rows > kMaxAtlasExtent)
    {
        return std::unexpected(FontImportError::AtlasFull);
    }
    rendered.glyph.width    = static_cast<uint16_t>(bitmap.width);
    rendered.glyph.height   = static_cast<uint16_t>(bitmap.rows);
    rendered.glyph.bearingX = static_cast<int16_t>(face->glyph->bitmap_left);
    rendered.glyph.bearingY = static_cast<int16_t>(face->glyph->bitmap_top);

    rendered.texels.resize(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
    const std::size_t rowBytes = bitmap.width;
    for (uint32_t row = 0; row < bitmap.rows; ++row)
    {
        // A negative pitch stores the rows bottom-up.
        const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
        const unsigned char *source = bitmap.pitch >= 0 ? bitmap.buffer + offset
                                                        : bitmap.buffer + offset - bitmap.pitch *
                                                              static_cast<std::ptrdiff_t>(bitmap.rows - 1);
        std::memcpy(rendered.texels.data() + static_cast<std::size_t>(row) * rowBytes, source, rowBytes);
    }
    return rendered;
}

/// Places every glyph on shelves in an @p extent square, tallest first. False
/// when they do not all fit.
bool Pack(std::vector<Rendered> &glyphs, uint32_t extent)
{
    std::vector<Rendered *> order;
    order.reserve(glyphs.size());
    for (Rendered &glyph : glyphs)
    {
        order.push_back(&glyph);
    }
    // Index breaks ties, so the layout never depends on the sort's stability.
    std::ranges::sort(order,
                      [](const Rendered *a, const Rendered *b)
                      {
                          if (a->glyph.height != b->glyph.height)
                          {
                              return a->glyph.height > b->glyph.height;
                          }
                          return a->glyph.index < b->glyph.index;
                      });

    uint32_t x           = 0;
    uint32_t y           = 0;
    uint32_t shelfHeight = 0;
    for (Rendered *rendered : order)
    {
        Glyph &glyph = rendered->glyph;
        if (glyph.width == 0 || glyph.height == 0)
        {
            glyph.x = 0;
            glyph.y = 0;
            continue;
        }
        if (x + glyph.width > extent)
        {
            x = 0;
            y += shelfHeight + kGlyphPadding;
            shelfHeight = 0;
        }
        if (glyph.width > extent || y + glyph.height > extent)
        {
            return false;
        }
        glyph.x = static_cast<uint16_t>(x);
        glyph.y = static_cast<uint16_t>(y);
        x += glyph.width + kGlyphPadding;
        shelfHeight = std::max<uint32_t>(shelfHeight, glyph.height);
    }
    return true;
}

} // namespace

std::string_view ToString(FontImportError error) noexcept
{
    switch (error)
    {
    case FontImportError::InvalidDocument:
        return "is not a FontDescription document";
    case FontImportError::BadRanges:
        return "has character ranges that are empty, odd in number, backwards or too many";
    case FontImportError::BadSize:
        return "has a pixel size that is not positive or too large";
    case FontImportError::BadSpread:
        return "has a spread outside what the rasteriser accepts";
    case FontImportError::MissingSource:
        return "names no font file";
    case FontImportError::UnreadableFont:
        return "names a font file that is not a font";
    case FontImportError::RasterizeFailed:
        return "has a glyph the rasteriser refused";
    case FontImportError::AtlasFull:
        return "has more glyphs than the largest atlas holds";
    case FontImportError::Count:
        break;
    }
    return "unknown";
}

uint32_t RasterizerVersion()
{
    constexpr uint32_t kComponentBits = 8;
    return (static_cast<uint32_t>(FREETYPE_MAJOR) << (2 * kComponentBits)) |
           (static_cast<uint32_t>(FREETYPE_MINOR) << kComponentBits) | static_cast<uint32_t>(FREETYPE_PATCH);
}

std::expected<FontDescription, FontImportError> ParseFontDescription(std::string_view text)
{
    FontDescription description;
    if (!Core::Reflect::ApplyAssetDocument(text, description))
    {
        return std::unexpected(FontImportError::InvalidDocument);
    }
    if (description.source.View().empty())
    {
        return std::unexpected(FontImportError::MissingSource);
    }
    if (!RangesAreValid(description.ranges))
    {
        return std::unexpected(FontImportError::BadRanges);
    }
    if (!(description.pixelSize > 0.f) || description.pixelSize > kMaxPixelSize)
    {
        return std::unexpected(FontImportError::BadSize);
    }
    if (description.spread < kMinSpread || description.spread > kMaxSpread)
    {
        return std::unexpected(FontImportError::BadSpread);
    }
    return description;
}

std::expected<Font, FontImportError> RasterizeFont(const FontDescription &description,
                                                   std::span<const std::byte> fontFile)
{
    FT_Library rawLibrary = nullptr;
    if (FT_Init_FreeType(&rawLibrary) != 0)
    {
        return std::unexpected(FontImportError::RasterizeFailed);
    }
    const LibraryHandle library(rawLibrary);

    const FT_Int spread = static_cast<FT_Int>(description.spread);
    if (FT_Property_Set(library.get(), "sdf", "spread", &spread) != 0)
    {
        return std::unexpected(FontImportError::BadSpread);
    }

    FT_Face rawFace = nullptr;
    if (FT_New_Memory_Face(library.get(), reinterpret_cast<const FT_Byte *>(fontFile.data()),
                           static_cast<FT_Long>(fontFile.size()), 0, &rawFace) != 0)
    {
        return std::unexpected(FontImportError::UnreadableFont);
    }
    const FaceHandle face(rawFace);
    if (FT_Set_Pixel_Sizes(face.get(), 0, static_cast<FT_UInt>(std::lround(description.pixelSize))) != 0)
    {
        return std::unexpected(FontImportError::BadSize);
    }

    // The cmap, and the set of glyphs it reaches plus the missing-glyph glyph.
    // Ordered containers, so the output is the same on every run.
    std::map<uint32_t, uint32_t> cmap;
    std::set<uint32_t> indices{kMissingGlyph};
    for (std::size_t i = 0; i < description.ranges.size(); i += 2)
    {
        for (uint32_t codepoint = description.ranges[i]; codepoint <= description.ranges[i + 1]; ++codepoint)
        {
            const uint32_t index = FT_Get_Char_Index(face.get(), codepoint);
            if (index != kMissingGlyph)
            {
                cmap.emplace(codepoint, index);
                indices.insert(index);
            }
        }
    }

    std::vector<Rendered> rendered;
    rendered.reserve(indices.size());
    for (const uint32_t index : indices)
    {
        std::expected<Rendered, FontImportError> glyph = RenderGlyph(face.get(), index);
        if (!glyph)
        {
            return std::unexpected(glyph.error());
        }
        rendered.push_back(std::move(*glyph));
    }

    uint32_t extent = kMinAtlasExtent;
    while (!Pack(rendered, extent))
    {
        if (extent == kMaxAtlasExtent)
        {
            return std::unexpected(FontImportError::AtlasFull);
        }
        extent *= 2;
    }

    Font font;
    font.kind        = FontKind::Sdf;
    font.spread      = static_cast<uint8_t>(description.spread);
    font.pixelSize   = description.pixelSize;
    font.ascender    = static_cast<float>(face->size->metrics.ascender) / kFixed26Dot6;
    font.descender   = static_cast<float>(face->size->metrics.descender) / kFixed26Dot6;
    font.lineHeight  = static_cast<float>(face->size->metrics.height) / kFixed26Dot6;
    font.atlasWidth  = extent;
    font.atlasHeight = extent;
    font.atlas.assign(static_cast<std::size_t>(extent) * extent, 0);

    // `rendered` is still in index order, which is the order Font keeps.
    font.glyphs.reserve(rendered.size());
    for (const Rendered &glyph : rendered)
    {
        for (uint32_t row = 0; row < glyph.glyph.height; ++row)
        {
            const std::size_t to   = (static_cast<std::size_t>(glyph.glyph.y) + row) * extent + glyph.glyph.x;
            const std::size_t from = static_cast<std::size_t>(row) * glyph.glyph.width;
            std::memcpy(font.atlas.data() + to, glyph.texels.data() + from, glyph.glyph.width);
        }
        font.glyphs.push_back(glyph.glyph);
    }

    font.cmap.reserve(cmap.size());
    for (const auto &[codepoint, index] : cmap)
    {
        font.cmap.push_back(CmapEntry{.codepoint = codepoint, .glyph = index});
    }

    if (FT_HAS_KERNING(face.get()))
    {
        for (const uint32_t left : indices)
        {
            for (const uint32_t right : indices)
            {
                FT_Vector delta{};
                if (FT_Get_Kerning(face.get(), left, right, FT_KERNING_DEFAULT, &delta) == 0 && delta.x != 0)
                {
                    font.kerning.push_back(KerningPair{
                        .adjust = static_cast<float>(delta.x) / kFixed26Dot6, .left = left, .right = right});
                }
            }
        }
    }
    return font;
}

std::expected<Font, FontLoadError> ReadSourceFont(std::string_view vpath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(vpath);
    if (!text)
    {
        return std::unexpected(FontLoadError::Missing);
    }
    const std::expected<FontDescription, FontImportError> description = ParseFontDescription(*text);
    if (!description)
    {
        Core::Log::Error("Mondrian: '{}' {}.", vpath, ToString(description.error()));
        return std::unexpected(FontLoadError::Invalid);
    }
    const std::expected<std::vector<std::byte>, Core::AssetError> fontFile =
        Core::AssetSystem::ReadBinary(description->source.View());
    if (!fontFile)
    {
        Core::Log::Error("Mondrian: '{}' names '{}', which could not be read.", vpath, description->source.View());
        return std::unexpected(FontLoadError::Unreadable);
    }
    std::expected<Font, FontImportError> font = RasterizeFont(*description, *fontFile);
    if (!font)
    {
        Core::Log::Error("Mondrian: '{}' {}.", vpath, ToString(font.error()));
        return std::unexpected(FontLoadError::Invalid);
    }
    return std::move(*font);
}

} // namespace Assisi::Mondrian::Import
