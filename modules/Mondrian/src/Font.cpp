/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Font.hpp>

#include <Assisi/Core/CookedBlob.hpp>

#include <algorithm>

namespace Assisi::Mondrian
{
namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// The fewest bytes one entry of each table takes, which bounds how many a
/// count can claim before anything is allocated.
constexpr std::size_t kGlyphBytes   = 2 * sizeof(uint32_t) + 4 * sizeof(uint16_t) + 2 * sizeof(int16_t);
constexpr std::size_t kCmapBytes    = 2 * sizeof(uint32_t);
constexpr std::size_t kKerningBytes = 2 * sizeof(uint32_t) + sizeof(float);

std::size_t BytesLeft(const Core::BitReader &reader)
{
    return reader.BitsRemaining() / kBitsPerByte;
}

/// Reads a table's entry count, refusing one the remaining bytes cannot hold.
std::optional<uint32_t> ReadCount(Core::BitReader &reader, std::size_t entryBytes)
{
    const uint32_t count = reader.ReadVarUInt32();
    if (reader.Failed() || count > BytesLeft(reader) / entryBytes)
    {
        return std::nullopt;
    }
    return count;
}

bool GlyphInsideAtlas(const Glyph &glyph, const Font &font)
{
    return static_cast<uint32_t>(glyph.x) + glyph.width <= font.atlasWidth &&
           static_cast<uint32_t>(glyph.y) + glyph.height <= font.atlasHeight;
}

/// Whether the tables agree with each other and with the atlas. The reader only
/// frames bytes; this is what says the frame holds a usable font.
bool IsConsistent(const Font &font)
{
    if (font.kind >= FontKind::Count ||
        font.atlas.size() != static_cast<std::size_t>(font.atlasWidth) * font.atlasHeight)
    {
        return false;
    }

    for (std::size_t i = 0; i < font.glyphs.size(); ++i)
    {
        if ((i > 0 && font.glyphs[i - 1].index >= font.glyphs[i].index) || !GlyphInsideAtlas(font.glyphs[i], font))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < font.cmap.size(); ++i)
    {
        if ((i > 0 && font.cmap[i - 1].codepoint >= font.cmap[i].codepoint) ||
            font.FindGlyph(font.cmap[i].glyph) == nullptr)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < font.kerning.size(); ++i)
    {
        const KerningPair &pair = font.kerning[i];
        if (i > 0)
        {
            const KerningPair &previous = font.kerning[i - 1];
            if (previous.left > pair.left || (previous.left == pair.left && previous.right >= pair.right))
            {
                return false;
            }
        }
        if (font.FindGlyph(pair.left) == nullptr || font.FindGlyph(pair.right) == nullptr)
        {
            return false;
        }
    }
    return true;
}

} // namespace

const Glyph *Font::FindGlyph(uint32_t index) const
{
    const auto found =
        std::ranges::lower_bound(glyphs, index, std::ranges::less{}, [](const Glyph &glyph) { return glyph.index; });
    return found != glyphs.end() && found->index == index ? &*found : nullptr;
}

std::optional<uint32_t> Font::GlyphFor(uint32_t codepoint) const
{
    const auto found = std::ranges::lower_bound(cmap, codepoint, std::ranges::less{},
                                                [](const CmapEntry &entry) { return entry.codepoint; });
    if (found == cmap.end() || found->codepoint != codepoint)
    {
        return std::nullopt;
    }
    return found->glyph;
}

std::string_view ToString(CookedFontError error) noexcept
{
    switch (error)
    {
    case CookedFontError::NotAFont:
        return "not a cooked font";
    case CookedFontError::UnsupportedVersion:
        return "a font layout this build does not read";
    case CookedFontError::Truncated:
        return "the font bytes end part-way through";
    case CookedFontError::Invalid:
        return "the font's tables contradict each other";
    case CookedFontError::Count:
        break;
    }
    return "unknown";
}

void WriteCookedFont(Core::BitWriter &writer, const Font &font)
{
    Core::WriteCookedHeader(writer, Core::CookedKind::Font);
    writer.WriteUInt8(kFontPayloadVersion);
    writer.WriteUInt8(static_cast<uint8_t>(font.kind));
    writer.WriteUInt8(font.spread);
    writer.WriteFloat(font.pixelSize);
    writer.WriteFloat(font.ascender);
    writer.WriteFloat(font.descender);
    writer.WriteFloat(font.lineHeight);
    writer.WriteUInt32(font.atlasWidth);
    writer.WriteUInt32(font.atlasHeight);

    writer.WriteVarUInt32(static_cast<uint32_t>(font.atlas.size()));
    writer.WriteBytes(std::as_bytes(std::span{font.atlas}));

    writer.WriteVarUInt32(static_cast<uint32_t>(font.glyphs.size()));
    for (const Glyph &glyph : font.glyphs)
    {
        writer.WriteUInt32(glyph.index);
        writer.WriteFloat(glyph.advance);
        writer.WriteUInt16(glyph.x);
        writer.WriteUInt16(glyph.y);
        writer.WriteUInt16(glyph.width);
        writer.WriteUInt16(glyph.height);
        writer.WriteInt16(glyph.bearingX);
        writer.WriteInt16(glyph.bearingY);
    }

    writer.WriteVarUInt32(static_cast<uint32_t>(font.cmap.size()));
    for (const CmapEntry &entry : font.cmap)
    {
        writer.WriteUInt32(entry.codepoint);
        writer.WriteUInt32(entry.glyph);
    }

    writer.WriteVarUInt32(static_cast<uint32_t>(font.kerning.size()));
    for (const KerningPair &pair : font.kerning)
    {
        writer.WriteUInt32(pair.left);
        writer.WriteUInt32(pair.right);
        writer.WriteFloat(pair.adjust);
    }
}

std::expected<Font, CookedFontError> ReadCookedFont(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::CookedKind, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::CookedKind::Font)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedFontError::Truncated);
        }
        return std::unexpected(CookedFontError::NotAFont);
    }

    const uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    if (version != kFontPayloadVersion)
    {
        return std::unexpected(CookedFontError::UnsupportedVersion);
    }

    Font font;
    const uint8_t kind = reader.ReadUInt8();
    font.spread        = reader.ReadUInt8();
    font.pixelSize     = reader.ReadFloat();
    font.ascender      = reader.ReadFloat();
    font.descender     = reader.ReadFloat();
    font.lineHeight    = reader.ReadFloat();
    font.atlasWidth    = reader.ReadUInt32();
    font.atlasHeight   = reader.ReadUInt32();
    // Held as the raw byte until the tables are read, so a truncated blob is
    // reported as truncated whatever its kind byte says.
    font.kind = static_cast<FontKind>(std::min<uint8_t>(kind, static_cast<uint8_t>(FontKind::Count)));

    const std::optional<uint32_t> atlasBytes = ReadCount(reader, sizeof(uint8_t));
    if (!atlasBytes)
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    font.atlas.resize(*atlasBytes);
    reader.ReadBytes(std::as_writable_bytes(std::span{font.atlas}));

    const std::optional<uint32_t> glyphCount = ReadCount(reader, kGlyphBytes);
    if (!glyphCount)
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    font.glyphs.resize(*glyphCount);
    for (Glyph &glyph : font.glyphs)
    {
        glyph.index    = reader.ReadUInt32();
        glyph.advance  = reader.ReadFloat();
        glyph.x        = reader.ReadUInt16();
        glyph.y        = reader.ReadUInt16();
        glyph.width    = reader.ReadUInt16();
        glyph.height   = reader.ReadUInt16();
        glyph.bearingX = reader.ReadInt16();
        glyph.bearingY = reader.ReadInt16();
    }

    const std::optional<uint32_t> cmapCount = ReadCount(reader, kCmapBytes);
    if (!cmapCount)
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    font.cmap.resize(*cmapCount);
    for (CmapEntry &entry : font.cmap)
    {
        entry.codepoint = reader.ReadUInt32();
        entry.glyph     = reader.ReadUInt32();
    }

    const std::optional<uint32_t> kerningCount = ReadCount(reader, kKerningBytes);
    if (!kerningCount)
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    font.kerning.resize(*kerningCount);
    for (KerningPair &pair : font.kerning)
    {
        pair.left   = reader.ReadUInt32();
        pair.right  = reader.ReadUInt32();
        pair.adjust = reader.ReadFloat();
    }

    if (reader.Failed())
    {
        return std::unexpected(CookedFontError::Truncated);
    }
    if (!IsConsistent(font))
    {
        return std::unexpected(CookedFontError::Invalid);
    }
    return font;
}

} // namespace Assisi::Mondrian
