/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/Import/SpriteSheetCompiler.hpp>

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"

#include <Assisi/Mondrian/StringTable.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Image/Decode.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

namespace Assisi::Mondrian::Import
{
namespace
{

constexpr std::string_view kSheetElement = "sheet";
constexpr std::string_view kSpriteElement = "sprite";
constexpr std::string_view kTextureAttribute = "texture";
constexpr std::string_view kNameAttribute = "name";
constexpr std::string_view kRectAttribute = "rect";
constexpr std::string_view kBordersAttribute = "borders";

/// Numbers in a rectangle: x, y, width and height.
constexpr std::size_t kRectNumbers = 4;

/// Numbers in borders written per side, and written once for every side.
constexpr std::size_t kBordersPerSide = 4;
constexpr std::size_t kBordersAllSides = 1;

/// @p text as @p count whole numbers of pixels, or nullopt.
template <std::size_t count> std::optional<std::array<uint32_t, count>> ReadPixels(std::string_view text)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.size() != count)
    {
        return std::nullopt;
    }
    std::array<uint32_t, count> numbers{};
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::optional<uint32_t> number = ParseUInt(words[index]);
        if (!number)
        {
            return std::nullopt;
        }
        numbers[index] = *number;
    }
    return numbers;
}

std::expected<SpriteRect, MarkupError> ReadRect(const MarkupAttribute &attribute, const SpriteSheetDocument &sheet)
{
    const std::optional<std::array<uint32_t, kRectNumbers>> numbers = ReadPixels<kRectNumbers>(attribute.value);
    if (!numbers)
    {
        return std::unexpected(At(attribute, "a rect is four whole numbers of pixels, x y width height, such as "
                                             "\"0 0 32 32\"; '" +
                                                 attribute.value + "' is not."));
    }
    const SpriteRect rect{.x = (*numbers)[0], .y = (*numbers)[1], .width = (*numbers)[2], .height = (*numbers)[3]};
    if (rect.width == 0 || rect.height == 0)
    {
        return std::unexpected(At(attribute, "the rect '" + attribute.value + "' holds no pixels."));
    }
    // Wide enough sums that a rectangle near the top of the range cannot wrap
    // round and pass.
    if (uint64_t{rect.x} + rect.width > sheet.width || uint64_t{rect.y} + rect.height > sheet.height)
    {
        return std::unexpected(At(attribute, "the rect '" + attribute.value + "' reaches past the texture '" +
                                                 std::string{sheet.texture.View()} + "', which is " +
                                                 std::to_string(sheet.width) + " by " + std::to_string(sheet.height) +
                                                 " pixels."));
    }
    return rect;
}

std::expected<SpriteBorders, MarkupError> ReadBorders(const MarkupAttribute &attribute, const SpriteRect &rect)
{
    SpriteBorders borders;
    if (const std::optional<std::array<uint32_t, kBordersAllSides>> all =
            ReadPixels<kBordersAllSides>(attribute.value))
    {
        borders = {.left = (*all)[0], .top = (*all)[0], .right = (*all)[0], .bottom = (*all)[0]};
    }
    else if (const std::optional<std::array<uint32_t, kBordersPerSide>> sides =
                 ReadPixels<kBordersPerSide>(attribute.value))
    {
        borders = {.left = (*sides)[0], .top = (*sides)[1], .right = (*sides)[2], .bottom = (*sides)[3]};
    }
    else
    {
        return std::unexpected(At(attribute, "borders are one whole number of pixels for every side, or four for "
                                             "left top right bottom, such as \"16\" or \"16 8 16 8\"; '" +
                                                 attribute.value + "' is neither."));
    }
    if (uint64_t{borders.left} + borders.right > rect.width || uint64_t{borders.top} + borders.bottom > rect.height)
    {
        return std::unexpected(At(attribute, "the borders '" + attribute.value +
                                                 "' do not fit inside their sprite, which is " +
                                                 std::to_string(rect.width) + " by " + std::to_string(rect.height) +
                                                 " pixels: each pair of opposite borders must fit across it."));
    }
    return borders;
}

/// Reads one `<sprite>` into @p sheet, refusing a name another took.
std::expected<void, MarkupError> ReadSprite(const MarkupElement &element, SpriteSheetDocument &sheet)
{
    if (!element.text.empty() || !element.children.empty())
    {
        return std::unexpected(At(element, "a sprite is one element holding nothing; it says what it is in its "
                                           "attributes."));
    }
    SheetSprite sprite;
    sprite.rect = {.x = 0, .y = 0, .width = sheet.width, .height = sheet.height};
    const MarkupAttribute *borders = nullptr;
    for (const MarkupAttribute &attribute : element.attributes)
    {
        if (attribute.name == kNameAttribute)
        {
            if (attribute.value.empty() || attribute.value.find(kTableSeparator) != std::string::npos)
            {
                return std::unexpected(At(attribute, "a sprite's name is what follows the sheet's name and a '" +
                                                         std::string{kTableSeparator} +
                                                         "', so it is not empty and holds no '" +
                                                         std::string{kTableSeparator} + "'."));
            }
            sprite.name = Core::InternedString{attribute.value};
        }
        else if (attribute.name == kRectAttribute)
        {
            const std::expected<SpriteRect, MarkupError> rect = ReadRect(attribute, sheet);
            if (!rect)
            {
                return std::unexpected(rect.error());
            }
            sprite.rect = *rect;
        }
        else if (attribute.name == kBordersAttribute)
        {
            // Read once the rect is known, wherever it is written.
            borders = &attribute;
        }
        else
        {
            return std::unexpected(At(attribute, "'" + attribute.name + "' is not part of a sprite, which takes " +
                                                     std::string{kNameAttribute} + ", " + std::string{kRectAttribute} +
                                                     " and " + std::string{kBordersAttribute} + "."));
        }
    }
    if (sprite.name.Empty())
    {
        return std::unexpected(At(element, "a sprite needs a name, which is what a screen calls it by."));
    }
    if (sheet.Find(sprite.name.View()) != nullptr)
    {
        return std::unexpected(At(element, "the sheet already has a sprite '" + std::string{sprite.name.View()} +
                                               "'; a name finds one sprite."));
    }
    if (borders != nullptr)
    {
        const std::expected<SpriteBorders, MarkupError> read = ReadBorders(*borders, sprite.rect);
        if (!read)
        {
            return std::unexpected(read.error());
        }
        sprite.borders = *read;
    }
    sheet.sprites.push_back(sprite);
    return {};
}

/// Reads the sheet's own attribute, its texture, and that texture's size.
std::expected<void, MarkupError> ReadTexture(const MarkupElement &root, const SourceReader &read,
                                             SpriteSheetDocument &sheet)
{
    const MarkupAttribute *texture = nullptr;
    for (const MarkupAttribute &attribute : root.attributes)
    {
        if (attribute.name != kTextureAttribute)
        {
            return std::unexpected(At(attribute, "'" + attribute.name + "' is not part of a <" +
                                                     std::string{kSheetElement} + ">, which takes only " +
                                                     std::string{kTextureAttribute} + "."));
        }
        texture = &attribute;
    }
    if (texture == nullptr || texture->value.empty())
    {
        return std::unexpected(
            At(root, "a sheet names the image its sprites are cut from, such as texture=\"ui/icons.png\"."));
    }
    const std::expected<std::string, std::string> bytes = read(texture->value);
    if (!bytes)
    {
        return std::unexpected(
            At(*texture, "the texture '" + texture->value + "' could not be read (" + bytes.error() + ")."));
    }
    const std::expected<Extent, std::string> size = ImageSize(*bytes);
    if (!size)
    {
        return std::unexpected(At(*texture, "'" + texture->value + "' " + size.error()));
    }
    sheet.texture = Core::AssetPath{texture->value};
    sheet.width = size->width;
    sheet.height = size->height;
    return {};
}

} // namespace

std::expected<SpriteSheetDocument, MarkupError> CompileSpriteSheet(const MarkupElement &root, const SourceReader &read)
{
    if (root.name != kSheetElement)
    {
        return std::unexpected(At(root, "a sprite sheet file starts with <" + std::string{kSheetElement} +
                                            ">, not <" + root.name + ">."));
    }
    if (!root.text.empty())
    {
        return std::unexpected(At(root, "a sheet holds sprites, and no text."));
    }
    SpriteSheetDocument sheet;
    if (const std::expected<void, MarkupError> texture = ReadTexture(root, read, sheet); !texture)
    {
        return std::unexpected(texture.error());
    }
    // A sheet the cook allows must be one the codec reads.
    if (root.children.size() > Core::Reflect::kMaxVectorElements)
    {
        return std::unexpected(
            At(root, "a sheet holds at most " + std::to_string(Core::Reflect::kMaxVectorElements) + " sprites."));
    }
    for (const MarkupElement &child : root.children)
    {
        if (child.name != kSpriteElement)
        {
            return std::unexpected(At(child, "'" + child.name + "' is not part of a sheet, which holds <" +
                                                 std::string{kSpriteElement} + ">."));
        }
        if (const std::expected<void, MarkupError> sprite = ReadSprite(child, sheet); !sprite)
        {
            return std::unexpected(sprite.error());
        }
    }
    return sheet;
}

std::expected<SpriteSheetDocument, MarkupError> CompileSpriteSheetSource(std::string_view text,
                                                                         const SourceReader &read)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return std::unexpected(parsed.error());
    }
    return CompileSpriteSheet(*parsed, read);
}

std::expected<std::vector<std::byte>, MarkupError> CompileSpriteSheetText(std::string_view text,
                                                                          const SourceReader &read)
{
    const std::expected<SpriteSheetDocument, MarkupError> sheet = CompileSpriteSheetSource(text, read);
    if (!sheet)
    {
        return std::unexpected(sheet.error());
    }
    Core::BitWriter writer;
    WriteCookedSpriteSheet(writer, *sheet);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

std::string SpriteSheetTexture(std::string_view text)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    if (!parsed)
    {
        return {};
    }
    const MarkupAttribute *texture = parsed->Find(kTextureAttribute);
    return texture != nullptr ? texture->value : std::string{};
}

std::expected<Extent, std::string> ImageSize(std::string_view bytes)
{
    const std::expected<Image::ImageExtent, Core::AssetError> extent =
        Image::ReadImageExtent(std::as_bytes(std::span{bytes.data(), bytes.size()}));
    if (!extent)
    {
        return std::unexpected(std::string{"is not an image this build reads."});
    }
    return Extent{.width = extent->width, .height = extent->height};
}

} // namespace Assisi::Mondrian::Import
