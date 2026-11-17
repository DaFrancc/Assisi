/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Sprite.hpp
/// @brief Pictures the UI draws: a sprite sheet's named rectangles of one
/// texture, and a picture as drawing reads it.
///
/// A sheet is a file of its own, cooked to a SpriteSheetDocument, named after
/// its file as a string table is: `ui/icons.amsp` is the sheet `icons`, and a
/// screen names its sprite `heart` as `icons:heart`. The sheet owns each
/// sprite's rectangle and borders, so a frame slices the same way everywhere
/// it is used.
///
/// The writer and the reader of the cooked form live here together, as a
/// theme's do in Theme.hpp, so the layout has one definition.

#include <Assisi/Mondrian/DrawList.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief Where a sprite is in its texture, in pixels from the top-left.
ASTRUCT()
struct SpriteRect
{
    AFIELD() uint32_t x = 0;
    AFIELD() uint32_t y = 0;
    AFIELD() uint32_t width = 0;
    AFIELD() uint32_t height = 0;

    [[nodiscard]] friend bool operator==(const SpriteRect &, const SpriteRect &) = default;
};

/// @brief How far in from each edge of a sprite, in its own pixels, the parts
/// a nine-slice keeps at their size reach. All zero for a sprite that stretches
/// whole.
ASTRUCT()
struct SpriteBorders
{
    AFIELD() uint32_t left = 0;
    AFIELD() uint32_t top = 0;
    AFIELD() uint32_t right = 0;
    AFIELD() uint32_t bottom = 0;

    /// @brief Whether any border is set, which is what makes a sprite draw as
    /// a nine-slice.
    [[nodiscard]] bool Any() const { return left != 0 || top != 0 || right != 0 || bottom != 0; }

    [[nodiscard]] friend bool operator==(const SpriteBorders &, const SpriteBorders &) = default;
};

/// @brief One named picture in a sheet.
ASTRUCT()
struct SheetSprite
{
    AFIELD() Core::InternedString name;
    AFIELD() SpriteRect rect;
    AFIELD() SpriteBorders borders;

    [[nodiscard]] friend bool operator==(const SheetSprite &, const SheetSprite &) = default;
};

/// @brief A whole sheet: its texture, that texture's size, and its sprites.
ASTRUCT()
struct SpriteSheetDocument
{
    /// The image the sprites are cut from.
    AFIELD() Assisi::Core::AssetPath texture;
    AFIELD() std::vector<SheetSprite> sprites;
    /// The texture's size in pixels, which every rectangle lies within.
    AFIELD() uint32_t width = 0;
    AFIELD() uint32_t height = 0;

    /// @brief The sprite called @p name, or null.
    [[nodiscard]] const SheetSprite *Find(std::string_view name) const;

    [[nodiscard]] friend bool operator==(const SpriteSheetDocument &, const SpriteSheetDocument &) = default;
};

/// @brief A sheet as the UI holds it: where it was loaded from, which names
/// it, and what it holds.
struct ListedSheet
{
    std::string vpath;
    SpriteSheetDocument document;
};

/// @brief Every sheet a project lists, in the order it lists them.
struct SpriteSheets
{
    std::vector<ListedSheet> listed;
};

/// @brief The name the sheet at @p vpath is known by: the file's name without
/// its folder or its last extension, as a string table's is.
[[nodiscard]] std::string_view SheetName(std::string_view vpath);

/// @brief How a screen names sprite @p sprite of sheet @p sheet: `sheet:sprite`.
[[nodiscard]] std::string QualifiedSprite(std::string_view sheet, std::string_view sprite);

/// @brief A picture as drawing reads it: which texture, where in it, how
/// large, and how it slices.
struct Sprite
{
    /// Where it is in its texture, as fractions of the texture.
    Rect uv{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};
    SpriteBorders borders;
    /// Its size in its own pixels, each one UI pixel at its natural size; zero
    /// for a picture of no known size, which takes no space of its own.
    uint32_t width = 0;
    uint32_t height = 0;
    TextureId texture = kWhiteTexture;

    [[nodiscard]] friend bool operator==(const Sprite &, const Sprite &) = default;
};

/// @brief @p sprite of @p sheet, drawn from @p texture, which holds the sheet's
/// image.
[[nodiscard]] Sprite SpriteOf(const SpriteSheetDocument &sheet, const SheetSprite &sprite, TextureId texture);

/// @brief All of @p texture, @p width by @p height pixels, as one picture.
[[nodiscard]] Sprite WholeTexture(TextureId texture, uint32_t width, uint32_t height);

/// @brief Version of how the sprite sheet payload is framed around the
/// document's fields, separate from the blob envelope's. The fields themselves
/// are covered by SpriteSheetLayoutHash.
///
/// Part of the sprite sheet cooker's cache key, so bumping it re-cooks every
/// sheet.
inline constexpr uint8_t kSpriteSheetPayloadVersion = 1;

/// @brief Hash of the document's reflected layout, every nested field included.
[[nodiscard]] uint64_t SpriteSheetLayoutHash();

/// @brief Why bytes did not read as a cooked sprite sheet.
enum class CookedSpriteSheetError : uint8_t
{
    NotASpriteSheet,    ///< Not a cooked blob, or a blob of another kind.
    Truncated,          ///< The bytes ran out inside the payload.
    UnsupportedVersion, ///< A payload version or field layout this build does not read.
    Invalid,            ///< Framed correctly and describing no usable sheet.
    Count_
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedSpriteSheetError error) noexcept;

/// @brief Write @p document as a complete cooked blob, envelope included.
void WriteCookedSpriteSheet(Core::BitWriter &writer, const SpriteSheetDocument &document);

/// @brief Read a blob written by WriteCookedSpriteSheet.
///
/// A sheet naming no texture, two sprites sharing a name, a sprite with no
/// area or reaching outside the texture, or borders wider or taller than their
/// sprite is Invalid.
[[nodiscard]] std::expected<SpriteSheetDocument, CookedSpriteSheetError> ReadCookedSpriteSheet(
    std::span<const std::byte> bytes);

} // namespace Assisi::Mondrian
