/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TextEdit.hpp
/// @brief What a node holding editable text carries beyond the text itself.
///
/// The text is the node's own, growable and UTF-8; this is everything around
/// it. The caret and the selection are counted in characters as a reader sees
/// them rather than in bytes, so no movement can land inside one and no
/// deletion can cut one in half. Which switches are on decides what a player
/// may do to the text, and the pattern decides whether what they have written
/// is acceptable — two separate questions, because refusing a keystroke and
/// marking a field wrong are different kindnesses.

#include <Assisi/Mondrian/Pattern.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace Assisi::Mondrian
{

struct Node;

/// What a masked field shows in place of each character it holds: a bullet,
/// U+2022. A font a masked field is set in has to carry it, or every character
/// shows as the missing-glyph box instead.
inline constexpr std::string_view kMaskMark = "\xE2\x80\xA2";

/// How wide the caret draws, in logical pixels. Layout keeps this much room
/// for it at the end of a line, so a caret at the end of a full field is not
/// the one thing pushed out of sight.
inline constexpr float kCaretWidth = 2.f;

/// @brief What a player may do with a node's text.
enum class TextEditing : uint8_t
{
    None,       ///< text to read: the node is not a field at all
    Selectable, ///< text to read and copy, such as an error code or an address
    Editable,   ///< a field
    Count
};

/// @brief Whether a field holds one line or many.
enum class TextLines : uint8_t
{
    Single, ///< never wraps, scrolls sideways, and submits on Enter
    Multi,  ///< wraps to its width, grows downwards, and takes a newline on Enter
    Count
};

/// @brief How tall a field of many lines is, counted in lines of its own text.
///
/// A field never scrolls its own content: a box that has to hold more than it
/// shows goes inside a scrolling node, which is where bars, the wheel and
/// dragging already live. So a field either grows, or is full.
enum class TextHeight : uint8_t
{
    Unbounded, ///< grows with what is in it, for as long as that goes on
    UpTo,      ///< grows with what is in it until it reaches its lines, then is full
    Exactly,   ///< always its lines tall, and full at that many
    Count
};

/// @brief Whether a field shows what it holds.
enum class TextMask : uint8_t
{
    None,
    Dots, ///< one mark per character, for a password
    Count
};

/// @brief The things a field can be asked not to allow. Every one is on until
/// it is turned off.
enum class TextAbility : uint8_t
{
    Select,
    Copy,
    Cut,
    Paste,
    Drag, ///< taking hold of what is selected and dropping it somewhere else
    Count
};

inline constexpr std::size_t kTextAbilityCount = static_cast<std::size_t>(TextAbility::Count);

/// @brief When a field's pattern is consulted, and what a mismatch costs.
enum class TextCheck : uint8_t
{
    /// An edit whose result the pattern could never accept does not happen.
    /// Suits a pattern that describes each character — digits, say — and not
    /// one describing a whole finished value, which is never true partway
    /// through being typed.
    Refuses,
    /// Every edit sets the field's validity, so it goes wrong the moment it
    /// stops matching and right again as soon as it matches.
    MarksAsTyped,
    /// Enter, or focus leaving the field, sets its validity. An address can be
    /// typed in peace and judged once it is finished.
    MarksOnCommit,
    Count
};

/// @brief How far a press that landed on what is already selected has got
/// towards carrying it somewhere else.
enum class TextDrag : uint8_t
{
    None,
    /// The press landed inside the selection and is still down. It may yet
    /// become a drag, so the selection is left alone until it is let go: a
    /// press that never moves is an ordinary click that puts the caret there.
    Held,
    Moving, ///< the pointer has moved, and the caret shows where the text would land
    Count
};

/// @brief Whether a field's text is acceptable, as of the last time anything
/// looked.
enum class TextValidity : uint8_t
{
    Unchecked, ///< no pattern, or nothing has been committed yet
    Valid,
    Invalid,
    Count
};

/// A field with no length limit, which is every field until one is set.
inline constexpr uint32_t kUnlimitedLength = std::numeric_limits<uint32_t>::max();

/// @brief A node's text editing state.
///
/// @p caret and @p anchor are character indices into the node's text: where the
/// next character goes, and where the selection it may be part of started. They
/// are equal when nothing is selected, and either may be the lesser — dragging
/// leftwards puts the caret before the anchor.
struct TextEdit
{
    /// What the field shows while it holds nothing: what to type, or an
    /// example of it. Never part of the text, so it is not returned, not
    /// selected, and not carried to the clipboard.
    std::string placeholder;
    /// What the text must look like to be acceptable; null for no pattern,
    /// which is a field that accepts anything. Shared, because a pattern is
    /// read-only once compiled and several fields may want the same one.
    std::shared_ptr<const Pattern> pattern;
    /// How far a single line has scrolled sideways to keep the caret in view,
    /// in logical pixels.
    float scrolled = 0.f;
    uint32_t caret = 0;
    uint32_t anchor = 0;
    /// The most characters the player may put in, counted as they see them.
    /// What is set from code is not held to it.
    uint32_t maxLength = kUnlimitedLength;
    /// Where what is being dragged would land, while it is being dragged.
    uint32_t dropAt = 0;
    /// How many lines UpTo and Exactly mean; unused while Unbounded.
    uint32_t lineLimit = 0;
    std::array<bool, kTextAbilityCount> abilities{true, true, true, true, true};
    TextEditing editing = TextEditing::None;
    TextLines lines = TextLines::Single;
    TextMask mask = TextMask::None;
    TextCheck check = TextCheck::MarksOnCommit;
    TextValidity validity = TextValidity::Unchecked;
    TextDrag drag = TextDrag::None;
    TextHeight height = TextHeight::Unbounded;

    [[nodiscard]] bool Can(TextAbility ability) const { return abilities[static_cast<std::size_t>(ability)]; }

    /// @brief Whether any text is selected.
    [[nodiscard]] bool HasSelection() const { return caret != anchor; }

    /// @brief The selected range, in characters, lower bound first.
    [[nodiscard]] uint32_t SelectionFirst() const { return caret < anchor ? caret : anchor; }
    [[nodiscard]] uint32_t SelectionLast() const { return caret < anchor ? anchor : caret; }
};

/// @brief What @p text looks like on screen under @p mask: itself, or one mark
/// per character.
///
/// The masked string is built into @p marks, which must outlive the view
/// returned; unmasked text is returned as a view of itself and costs nothing.
/// Everything that maps between the screen and the text — shaping, the caret,
/// hit testing — works on what is shown, so a masked field's marks line up with
/// the characters they stand for one to one.
[[nodiscard]] std::string_view ShownText(std::string_view text, TextMask mask, std::string &marks);

/// @brief The control that a node's TextEdit state belongs to: the caret, the
/// selection, the editing keys and the clipboard.
[[nodiscard]] WidgetType TextFieldWidget();

/// @brief Where the caret stands on @p node, in device pixels: as tall as the
/// line it is on rather than as tall as the box, and placed against the text
/// as @p layout and @p text put it. Empty without both of those.
///
/// What the caret is drawn from, and what anything that has to follow it is
/// placed against — a suggestion list, or the window a writing system puts
/// part-formed characters in.
[[nodiscard]] Rect CaretRect(const Node &node, const LayoutNode *layout, const TextLayout *text, float scale);

/// @brief Judges what @p node holds against its pattern, settling its validity
/// now whatever its mode says about when. A node with no pattern goes back to
/// Unchecked.
///
/// Called where the text is finished rather than merely changed: a field
/// submitted, focus leaving one, and text put in from code.
void CommitText(Node &node);

} // namespace Assisi::Mondrian
