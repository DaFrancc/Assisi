/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Sprite.hpp>

#include <Assisi/Mondrian/StringTable.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <algorithm>
#include <unordered_set>

ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::SpriteSheetDocument);

namespace Assisi::Mondrian
{
namespace
{

/// Bits in the layout hash that follows the version.
constexpr uint32_t kLayoutHashBits = 64;

const Core::Reflect::StructSpec &DocumentSpec()
{
    const Core::Reflect::StructSpec *spec = Core::Reflect::StructTraits<SpriteSheetDocument>::Spec();
    ASSISI_ASSERT(spec != nullptr, "SpriteSheetDocument's generated table is missing");
    return *spec;
}

/// Whether @p sprite has an area, lies within a texture @p width by @p height,
/// and keeps its borders inside itself. Wide enough sums that a rectangle near
/// the top of the range cannot wrap round and pass.
bool Fits(const SheetSprite &sprite, uint32_t width, uint32_t height)
{
    const SpriteRect &rect = sprite.rect;
    const SpriteBorders &borders = sprite.borders;
    return rect.width != 0 && rect.height != 0 && uint64_t{rect.x} + rect.width <= width &&
           uint64_t{rect.y} + rect.height <= height && uint64_t{borders.left} + borders.right <= rect.width &&
           uint64_t{borders.top} + borders.bottom <= rect.height;
}

/// Whether @p document is one a cook writes, so every sprite in it can be drawn.
bool IsConsistent(const SpriteSheetDocument &document)
{
    if (document.texture.Empty() || document.width == 0 || document.height == 0)
    {
        return false;
    }
    std::unordered_set<Core::InternedString> seen;
    for (const SheetSprite &sprite : document.sprites)
    {
        if (sprite.name.Empty() || !seen.insert(sprite.name).second || !Fits(sprite, document.width, document.height))
        {
            return false;
        }
    }
    return true;
}

} // namespace

const SheetSprite *SpriteSheetDocument::Find(std::string_view name) const
{
    const std::vector<SheetSprite>::const_iterator found =
        std::ranges::find_if(sprites, [name](const SheetSprite &sprite) { return sprite.name.View() == name; });
    return found != sprites.end() ? &*found : nullptr;
}

std::string_view SheetName(std::string_view vpath)
{
    return TableName(vpath);
}

std::string QualifiedSprite(std::string_view sheet, std::string_view sprite)
{
    std::string qualified{sheet};
    qualified += kTableSeparator;
    qualified += sprite;
    return qualified;
}

Sprite SpriteOf(const SpriteSheetDocument &sheet, const SheetSprite &sprite, TextureId texture)
{
    const float width = static_cast<float>(sheet.width);
    const float height = static_cast<float>(sheet.height);
    return Sprite{.uv = {.x = static_cast<float>(sprite.rect.x) / width,
                         .y = static_cast<float>(sprite.rect.y) / height,
                         .width = static_cast<float>(sprite.rect.width) / width,
                         .height = static_cast<float>(sprite.rect.height) / height},
                  .borders = sprite.borders,
                  .width = sprite.rect.width,
                  .height = sprite.rect.height,
                  .texture = texture};
}

Sprite WholeTexture(TextureId texture, uint32_t width, uint32_t height)
{
    return Sprite{.uv = {.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f},
                  .borders = {},
                  .width = width,
                  .height = height,
                  .texture = texture};
}

uint64_t SpriteSheetLayoutHash()
{
    return Core::Reflect::StructLayoutHash(DocumentSpec());
}

std::string_view ToString(CookedSpriteSheetError error) noexcept
{
    switch (error)
    {
    case CookedSpriteSheetError::NotASpriteSheet:
        return "not a cooked sprite sheet";
    case CookedSpriteSheetError::Truncated:
        return "ended part-way through";
    case CookedSpriteSheetError::UnsupportedVersion:
        return "a sprite sheet layout this build does not read";
    case CookedSpriteSheetError::Invalid:
        return "describes no usable sprite sheet";
    case CookedSpriteSheetError::Count_:
        break;
    }
    return "unknown";
}

void WriteCookedSpriteSheet(Core::BitWriter &writer, const SpriteSheetDocument &document)
{
    Core::WriteCookedHeader(writer, Core::kSpriteSheetKind);
    writer.WriteUInt8(kSpriteSheetPayloadVersion);
    writer.WriteBits64(SpriteSheetLayoutHash(), kLayoutHashBits);
    (void)Core::Reflect::WriteStruct(DocumentSpec(), &document, writer);
}

std::expected<SpriteSheetDocument, CookedSpriteSheetError> ReadCookedSpriteSheet(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::AssetKindId, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::kSpriteSheetKind)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedSpriteSheetError::Truncated);
        }
        return std::unexpected(CookedSpriteSheetError::NotASpriteSheet);
    }

    const uint8_t version = reader.ReadUInt8();
    const uint64_t layout = reader.ReadBits64(kLayoutHashBits);
    if (reader.Failed())
    {
        return std::unexpected(CookedSpriteSheetError::Truncated);
    }
    // Either one differing means the fields that follow are not the ones this
    // build would read them into.
    if (version != kSpriteSheetPayloadVersion || layout != SpriteSheetLayoutHash())
    {
        return std::unexpected(CookedSpriteSheetError::UnsupportedVersion);
    }

    SpriteSheetDocument document;
    const bool read = Core::Reflect::ReadStruct(DocumentSpec(), &document, reader);
    // Framing first, as a theme's: a document read out of bytes that ran out
    // holds whatever zeroes the reader handed back.
    if (reader.Failed())
    {
        return std::unexpected(CookedSpriteSheetError::Truncated);
    }
    if (!read || !IsConsistent(document))
    {
        return std::unexpected(CookedSpriteSheetError::Invalid);
    }
    return document;
}

} // namespace Assisi::Mondrian
