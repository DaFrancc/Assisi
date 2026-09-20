/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Clipboard.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>
#include <Assisi/Mondrian/Utf8.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

/// The field's own look, until themes decide it.
constexpr Math::Color4<Math::ColorSpace::Srgb> kCaretColor{0.95f, 0.96f, 0.98f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kSelectionColor{0.22f, 0.38f, 0.72f, 1.f};

/// Whether @p codepoint is a space, a tab or a line break: what separates one
/// word from the next when the caret moves by words.
bool Separates(uint32_t codepoint)
{
    // The two spaces that are not ASCII are the ones that turn up in real text:
    // the one that refuses to break a line, and the wide one CJK writing uses.
    constexpr uint32_t kNoBreakSpace = 0x00A0;
    constexpr uint32_t kIdeographicSpace = 0x3000;
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
           codepoint == kNoBreakSpace || codepoint == kIdeographicSpace;
}

/// The codepoint of the character at @p index, or nothing past the end.
uint32_t CharacterCodepoint(std::string_view text, uint32_t index)
{
    uint32_t offset = CharacterOffset(text, index);
    return offset < text.size() ? DecodeUtf8(text, offset) : 0;
}

/// Whether the character at @p index separates words.
bool SeparatesAt(std::string_view text, uint32_t index)
{
    return Separates(CharacterCodepoint(text, index));
}

/// The character index a word-sized step from @p from reaches, going @p forward.
///
/// Forward: past the rest of this word and the spaces after it. Backward: past
/// the spaces before it and then the word. Either way it lands where a word
/// begins or ends, which is where a reader expects the caret to stop.
uint32_t WordStep(std::string_view text, uint32_t from, bool forward)
{
    const uint32_t characters = CharacterCount(text);
    uint32_t at = from;
    if (forward)
    {
        while (at < characters && !SeparatesAt(text, at))
        {
            ++at;
        }
        while (at < characters && SeparatesAt(text, at))
        {
            ++at;
        }
        return at;
    }
    while (at > 0 && SeparatesAt(text, at - 1))
    {
        --at;
    }
    while (at > 0 && !SeparatesAt(text, at - 1))
    {
        --at;
    }
    return at;
}

/// The word around @p index, for a double click to select.
TextRange WordAround(std::string_view text, uint32_t index)
{
    const uint32_t characters = CharacterCount(text);
    if (characters == 0)
    {
        return {};
    }
    // A click past the last character selects the word that ends there.
    const uint32_t at = std::min(index, characters - 1);
    if (SeparatesAt(text, at))
    {
        return {.first = at, .last = at + 1};
    }
    uint32_t first = at;
    while (first > 0 && !SeparatesAt(text, first - 1))
    {
        --first;
    }
    uint32_t last = at;
    while (last < characters && !SeparatesAt(text, last))
    {
        ++last;
    }
    return {.first = first, .last = last};
}

/// The bytes of @p text that @p range covers.
std::string_view Slice(std::string_view text, TextRange range)
{
    const uint32_t from = CharacterOffset(text, range.first);
    const uint32_t to = CharacterOffset(text, range.last);
    return text.substr(from, to - from);
}

/// What is selected in @p node, empty when nothing is.
std::string_view Selected(const Node &node)
{
    return node.edit.HasSelection()
               ? Slice(node.text, {.first = node.edit.SelectionFirst(), .last = node.edit.SelectionLast()})
               : std::string_view{};
}

/// Puts the caret at @p index, taking the selection with it or dropping it.
void MoveCaret(TextEdit &edit, uint32_t index, TextReach reach)
{
    edit.caret = index;
    if (reach == TextReach::Moves)
    {
        edit.anchor = index;
    }
}

/// Whether the field would accept @p candidate as its text.
///
/// Emptying a field is always allowed: a pattern describes what a finished
/// value looks like, and refusing the empty string would leave a full field
/// with no way back.
bool Refuses(const TextEdit &edit, std::string_view candidate)
{
    if (edit.check != TextCheck::Refuses || edit.pattern == nullptr || candidate.empty())
    {
        return false;
    }
    return MatchPattern(*edit.pattern, candidate) == PatternMatch::No;
}

/// What @p text is worth against @p edit's pattern.
TextValidity Judge(const TextEdit &edit, std::string_view text)
{
    if (edit.pattern == nullptr)
    {
        return TextValidity::Unchecked;
    }
    return MatchPattern(*edit.pattern, text) == PatternMatch::Yes ? TextValidity::Valid : TextValidity::Invalid;
}

/// How many lines @p candidate would take in the field @p view is on, at the
/// width it has now.
///
/// Costs a shaping and a layout of the whole text, which is why it is asked
/// only of a field that is held to a number of lines.
uint32_t LinesOf(const WidgetView &view, std::string_view candidate)
{
    const TextLayout *text = view.text;
    if (text == nullptr || text->font == nullptr || view.layout == nullptr)
    {
        return 0;
    }
    std::string marks;
    const std::string_view shown = ShownText(candidate, view.node->edit.mask, marks);
    const Font &font = *text->font;
    const TextLayout laid =
        LayoutText(Shape(shown, font), font, view.node->style.textSize * view.scale,
                   TextWrapWidth(*view.layout, view.node->style, view.scale), view.node->style.textAlign);
    return static_cast<uint32_t>(laid.lines.size());
}

/// Whether @p candidate has more lines than the field @p view is on will hold.
///
/// A field that is full refuses what would overflow it rather than scrolling:
/// a box that has to hold more than it shows belongs inside a scrolling node.
bool Overflows(const WidgetView &view, std::string_view candidate)
{
    const TextEdit &edit = view.node->edit;
    if (edit.lines != TextLines::Multi || edit.height == TextHeight::Unbounded || edit.lineLimit == 0)
    {
        return false;
    }
    return LinesOf(view, candidate) > edit.lineLimit;
}

/// Replaces what is selected in @p node with @p insert, which may be empty, and
/// leaves the caret after it.
///
/// Refuses the whole edit when the field's pattern would not have it, which is
/// why nothing here changes the node until the result is known.
bool Replace(const WidgetView &view, Node &node, std::string_view insert)
{
    TextEdit &edit = node.edit;
    const TextRange gone{.first = edit.SelectionFirst(), .last = edit.SelectionLast()};
    const uint32_t from = CharacterOffset(node.text, gone.first);
    const uint32_t to = CharacterOffset(node.text, gone.last);

    std::string candidate;
    candidate.reserve(node.text.size() - (to - from) + insert.size());
    candidate.append(node.text, 0, from).append(insert).append(node.text, to);
    if (candidate == node.text || Refuses(edit, candidate) || Overflows(view, candidate))
    {
        return false;
    }

    node.text = std::move(candidate);
    MoveCaret(edit, gone.first + CharacterCount(insert), TextReach::Moves);
    if (edit.check == TextCheck::MarksAsTyped)
    {
        edit.validity = Judge(edit, node.text);
    }
    return true;
}

/// Whether what is selected in @p edit may be picked up and carried elsewhere.
bool CanCarry(const TextEdit &edit)
{
    return edit.editing == TextEditing::Editable && edit.Can(TextAbility::Drag) && edit.HasSelection();
}

/// Takes what is selected out of @p node and puts it back at @p dropAt,
/// leaving it selected where it lands.
///
/// Dropping it on itself changes nothing, which is what a press and a small
/// wobble of the hand amount to.
bool MoveSelection(const WidgetView &view, Node &node, uint32_t dropAt)
{
    TextEdit &edit = node.edit;
    const TextRange taken{.first = edit.SelectionFirst(), .last = edit.SelectionLast()};
    if (dropAt >= taken.first && dropAt <= taken.last)
    {
        return false;
    }

    const std::string carried{Slice(node.text, taken)};
    const uint32_t from = CharacterOffset(node.text, taken.first);
    const uint32_t to = CharacterOffset(node.text, taken.last);

    std::string candidate = node.text;
    candidate.erase(from, to - from);
    // A drop beyond the text that was lifted lands that much further back,
    // now that the gap has closed behind it.
    const uint32_t landing = dropAt > taken.last ? dropAt - (taken.last - taken.first) : dropAt;
    candidate.insert(CharacterOffset(candidate, landing), carried);

    // Rearranging text can change where it wraps, so a move can cross the line
    // limit even though it adds nothing.
    if (candidate == node.text || Refuses(edit, candidate) || Overflows(view, candidate))
    {
        return false;
    }
    node.text = std::move(candidate);
    edit.anchor = landing;
    edit.caret = landing + CharacterCount(carried);
    if (edit.check == TextCheck::MarksAsTyped)
    {
        edit.validity = Judge(edit, node.text);
    }
    return true;
}

/// @p text cleaned up for @p edit: control characters dropped, line breaks kept
/// or turned into spaces, and no more characters than the field has room for.
///
/// Typed text and pasted text go through here alike, so a field cannot be
/// filled past its limit by the clipboard.
std::string Clean(const TextEdit &edit, std::string_view text, uint32_t keeping)
{
    /// The one control character above a space, which a terminal sends for a
    /// backspace and nothing should ever hold.
    constexpr uint32_t kDelete = 0x7F;

    std::string cleaned;
    cleaned.reserve(text.size());
    uint32_t offset = 0;
    uint32_t previous = 0;
    while (offset < text.size())
    {
        const uint32_t codepoint = DecodeUtf8(text, offset);
        // A carriage return and the line feed after it are one break between
        // them, which is how the rest of the world writes one.
        const bool pairedWithReturn = codepoint == '\n' && previous == '\r';
        previous = codepoint;
        if (pairedWithReturn)
        {
            continue;
        }
        if (codepoint == '\n' || codepoint == '\r')
        {
            // One line takes a space where a break was, so pasting a paragraph
            // into a name box does not silently run the words together.
            if (edit.lines == TextLines::Multi)
            {
                cleaned.push_back('\n');
            }
            else if (!cleaned.empty() && cleaned.back() != ' ')
            {
                cleaned.push_back(' ');
            }
            continue;
        }
        // Everything else below a space is a control character: a bell, an
        // escape, the remains of a terminal's idea of formatting.
        if (codepoint < ' ' || codepoint == kDelete)
        {
            continue;
        }
        EncodeUtf8(codepoint, cleaned);
    }

    // Then cut to what the field has room for, on a character boundary, so a
    // long paste is trimmed rather than refused and never split in the middle
    // of a letter.
    if (edit.maxLength != kUnlimitedLength)
    {
        const uint32_t room = edit.maxLength - std::min(edit.maxLength, keeping);
        if (CharacterCount(cleaned) > room)
        {
            cleaned.resize(CharacterOffset(cleaned, room));
        }
    }
    return cleaned;
}

/// How many characters @p node keeps once what is selected in it goes, which is
/// what a length limit leaves room against.
uint32_t Keeping(const Node &node)
{
    const TextEdit &edit = node.edit;
    return CharacterCount(node.text) - (edit.SelectionLast() - edit.SelectionFirst());
}

} // namespace

void CommitText(Node &node)
{
    node.edit.validity = Judge(node.edit, node.text);
}

Rect CaretRect(const Node &node, const LayoutNode *layout, const TextLayout *text, float scale)
{
    if (layout == nullptr || text == nullptr)
    {
        return {};
    }

    std::string marks;
    const std::string_view shown = ShownText(node.text, node.edit.mask, marks);
    // While a selection is being carried, the caret is where it would land
    // rather than where it came from: it is the one thing saying where the
    // text will end up.
    const uint32_t at = node.edit.drag == TextDrag::Moving ? node.edit.dropAt : node.edit.caret;
    const CaretPlace place = PlaceCaret(*text, shown, at);
    const Point origin = TextOrigin(*layout, node.style, scale);

    // As tall as the line it stands on, not as tall as the box: a caret taking
    // the whole height of a field reaches out through its padding, and one in
    // a field of many lines strikes through every line at once.
    const Rect line = LineBox(*text, place.line);
    const bool measured = line.height > 0.f;
    return {.x = std::round(origin.x + place.x),
            .y = measured ? origin.y + line.y : layout->rect.y,
            .width = std::max(1.f, std::round(kCaretWidth * scale)),
            .height = measured ? line.height : layout->rect.height};
}

std::string_view ShownText(std::string_view text, TextMask mask, std::string &marks)
{
    if (mask == TextMask::None)
    {
        return text;
    }
    marks.clear();
    marks.reserve(CharacterCount(text) * kMaskMark.size());
    for (uint32_t offset = 0; offset < text.size(); offset = NextCharacter(text, offset))
    {
        marks.append(kMaskMark);
    }
    return marks;
}

namespace
{

/// Where the text starts inside @p view's node, in device pixels.
Point Origin(const WidgetView &view)
{
    return TextOrigin(*view.layout, view.node->style, view.scale);
}

void DrawSelection(const WidgetView &view, DrawList &list)
{
    const Node &node = *view.node;
    if (view.text == nullptr || !node.edit.HasSelection() || !view.focused)
    {
        return;
    }

    std::string marks;
    const std::string_view shown = ShownText(node.text, node.edit.mask, marks);
    const TextRange range{.first = node.edit.SelectionFirst(), .last = node.edit.SelectionLast()};
    const Point origin = Origin(view);
    for (uint32_t line = 0; line < view.text->lines.size(); ++line)
    {
        const Rect box = LineSelection(*view.text, shown, line, range);
        if (box.width > 0.f)
        {
            list.Quad({.x = origin.x + box.x, .y = origin.y + box.y, .width = box.width, .height = box.height})
                .Fill(kSelectionColor);
        }
    }
}

Core::CursorShape FieldCursor(const WidgetView &view, Point /*point*/)
{
    // Carrying a selection is a different job from pointing into text, and the
    // pointer says which is happening.
    return view.node->edit.drag == TextDrag::Moving ? Core::CursorShape::Move : Core::CursorShape::Text;
}

void DrawCaret(const WidgetView &view, DrawList &list)
{
    const Node &node = *view.node;
    if (view.text == nullptr || !view.focused || node.edit.editing != TextEditing::Editable)
    {
        return;
    }
    list.Quad(CaretRect(node, view.layout, view.text, view.scale)).Fill(kCaretColor);
}

/// Where Home and End go: the ends of the line the caret is on, or of the whole
/// text when @p whole. A single line has only the one, so both are the same.
uint32_t LineEnds(const WidgetView &view, const Node &node, bool end, bool whole)
{
    if (whole || view.text == nullptr || view.text->lines.empty())
    {
        return end ? CharacterCount(node.text) : 0;
    }
    std::string marks;
    const std::string_view shown = ShownText(node.text, node.edit.mask, marks);
    const CaretPlace place = PlaceCaret(*view.text, shown, node.edit.caret);
    const TextLine &row = view.text->lines[place.line];
    return CharacterAt(*view.text, shown, {.x = end ? row.x + row.width : row.x, .y = row.baseline});
}

/// Which character of @p node's text the pointer is over.
uint32_t CharacterUnder(const WidgetView &view, Point pointer)
{
    if (view.text == nullptr)
    {
        return 0;
    }
    std::string marks;
    const std::string_view shown = ShownText(view.node->text, view.node->edit.mask, marks);
    const Point origin = Origin(view);
    return CharacterAt(*view.text, shown, {.x = pointer.x - origin.x, .y = pointer.y - origin.y});
}

/// Takes what is selected to the clipboard, and cuts it out when asked.
WidgetResponse Copy(const WidgetView &view, Node &node, TextAbility ability)
{
    const TextEdit &edit = node.edit;
    if (!edit.Can(ability) || !edit.HasSelection() || view.clipboard == nullptr)
    {
        return WidgetResponse::Handled;
    }
    view.clipboard->Write(Selected(node));
    if (ability == TextAbility::Cut && edit.editing == TextEditing::Editable)
    {
        return Replace(view, node, {}) ? WidgetResponse::Changed : WidgetResponse::Handled;
    }
    return WidgetResponse::Handled;
}

WidgetResponse Paste(const WidgetView &view, Node &node)
{
    if (!node.edit.Can(TextAbility::Paste) || node.edit.editing != TextEditing::Editable || view.clipboard == nullptr)
    {
        return WidgetResponse::Handled;
    }
    const std::string pasted = view.clipboard->Read();
    return Replace(view, node, Clean(node.edit, pasted, Keeping(node))) ? WidgetResponse::Changed
                                                                        : WidgetResponse::Handled;
}

/// Erases in @p direction, or the selection when there is one.
WidgetResponse Erase(const WidgetView &view, Node &node, const WidgetEvent &event, bool forward)
{
    TextEdit &edit = node.edit;
    if (edit.editing != TextEditing::Editable)
    {
        return WidgetResponse::Ignored;
    }
    if (!edit.HasSelection())
    {
        // Erasing without a selection takes the character, or the word, on the
        // side the key names: the selection machinery then removes it.
        const uint32_t characters = CharacterCount(node.text);
        const uint32_t to = event.step == TextStep::Word ? WordStep(node.text, edit.caret, forward)
                                                         : (forward ? std::min(edit.caret + 1, characters)
                                                                    : (edit.caret > 0 ? edit.caret - 1 : 0));
        edit.anchor = to;
    }
    return Replace(view, node, {}) ? WidgetResponse::Changed : WidgetResponse::Handled;
}

/// Moves the caret sideways by a character or a word.
WidgetResponse Sideways(Node &node, const WidgetEvent &event, bool forward)
{
    TextEdit &edit = node.edit;
    const uint32_t characters = CharacterCount(node.text);

    // A sideways key with something selected and no Shift collapses the
    // selection to its near end rather than moving from the caret.
    if (edit.HasSelection() && event.reach == TextReach::Moves && event.step == TextStep::Character)
    {
        MoveCaret(edit, forward ? edit.SelectionLast() : edit.SelectionFirst(), TextReach::Moves);
        return WidgetResponse::Handled;
    }

    const uint32_t to = event.step == TextStep::Word
                            ? WordStep(node.text, edit.caret, forward)
                            : (forward ? std::min(edit.caret + 1, characters) : (edit.caret > 0 ? edit.caret - 1 : 0));
    MoveCaret(edit, to, event.reach);
    return WidgetResponse::Handled;
}

/// Moves the caret a line up or down, keeping roughly where it was across.
WidgetResponse Vertically(const WidgetView &view, Node &node, const WidgetEvent &event, bool down)
{
    TextEdit &edit = node.edit;
    if (edit.lines != TextLines::Multi || view.text == nullptr)
    {
        return WidgetResponse::Ignored;
    }

    std::string marks;
    const std::string_view shown = ShownText(node.text, edit.mask, marks);
    const CaretPlace place = PlaceCaret(*view.text, shown, edit.caret);
    if ((down && place.line + 1 >= view.text->lines.size()) || (!down && place.line == 0))
    {
        // Off the end of the text: let the focus move to the next control, the
        // way a single-line field never holds an up or a down at all.
        return WidgetResponse::Ignored;
    }

    const TextLine &row = view.text->lines[down ? place.line + 1 : place.line - 1];
    MoveCaret(edit, CharacterAt(*view.text, shown, {.x = place.x, .y = row.baseline}), event.reach);
    return WidgetResponse::Handled;
}

WidgetResponse EditInput(const WidgetView &view, Node &node, const WidgetEvent &event);

WidgetResponse ActionInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    TextEdit &edit = node.edit;
    switch (event.action)
    {
    case UiAction::Left:
        return Sideways(node, event, false);
    case UiAction::Right:
        return Sideways(node, event, true);
    case UiAction::Up:
        return Vertically(view, node, event, false);
    case UiAction::Down:
        return Vertically(view, node, event, true);
    case UiAction::Accept:
        if (edit.editing != TextEditing::Editable)
        {
            return WidgetResponse::Ignored;
        }
        if (edit.lines == TextLines::Multi)
        {
            return Replace(view, node, "\n") ? WidgetResponse::Changed : WidgetResponse::Handled;
        }
        // A single line is finished by Enter, which is also when a field that
        // waits until the end judges what it holds.
        CommitText(node);
        return WidgetResponse::Submitted;
    case UiAction::Back:
    case UiAction::Next:
    case UiAction::Previous:
    case UiAction::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

WidgetResponse FieldInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    TextEdit &edit = node.edit;
    if (edit.editing == TextEditing::None)
    {
        return WidgetResponse::Ignored;
    }

    // Layout is what worked out where the line had to be for the caret to show;
    // taking that back means the next frame starts from where this one ended
    // rather than sliding back and forth.
    edit.scrolled = view.scale > 0.f ? view.layout->textScroll / view.scale : 0.f;

    switch (event.gesture)
    {
    case WidgetGesture::Press:
    {
        const uint32_t at = CharacterUnder(view, event.pointer);
        constexpr uint32_t kDoubleClick = 2;
        edit.drag = TextDrag::None;
        if (event.clicks == kDoubleClick && edit.Can(TextAbility::Select))
        {
            const TextRange word = WordAround(node.text, at);
            edit.anchor = word.first;
            edit.caret = word.last;
            return WidgetResponse::Handled;
        }
        // A press on what is already selected may be the start of carrying it
        // somewhere, so the selection stands until the pointer says which.
        if (CanCarry(edit) && at >= edit.SelectionFirst() && at < edit.SelectionLast())
        {
            edit.drag = TextDrag::Held;
            edit.dropAt = at;
            return WidgetResponse::Handled;
        }
        MoveCaret(edit, at, TextReach::Moves);
        return WidgetResponse::Handled;
    }
    case WidgetGesture::Drag:
        if (edit.drag != TextDrag::None)
        {
            edit.drag = TextDrag::Moving;
            edit.dropAt = CharacterUnder(view, event.pointer);
            return WidgetResponse::Handled;
        }
        if (edit.Can(TextAbility::Select))
        {
            MoveCaret(edit, CharacterUnder(view, event.pointer), TextReach::Extends);
        }
        return WidgetResponse::Handled;
    case WidgetGesture::Release:
    {
        const TextDrag was = edit.drag;
        edit.drag = TextDrag::None;
        if (was == TextDrag::Moving)
        {
            return MoveSelection(view, node, edit.dropAt) ? WidgetResponse::Changed : WidgetResponse::Handled;
        }
        if (was == TextDrag::Held)
        {
            // Pressed on the selection and let go without going anywhere,
            // which is the ordinary click it turned out to be.
            MoveCaret(edit, edit.dropAt, TextReach::Moves);
        }
        return WidgetResponse::Handled;
    }
    case WidgetGesture::Activate:
        // The click that focused the field has already placed the caret; there
        // is nothing for an activation to add.
        return WidgetResponse::Handled;
    case WidgetGesture::Action:
        return ActionInput(view, node, event);
    case WidgetGesture::Edit:
        return EditInput(view, node, event);
    case WidgetGesture::Type:
    {
        if (edit.editing != TextEditing::Editable)
        {
            return WidgetResponse::Ignored;
        }
        const std::string typed = Clean(edit, event.typed, Keeping(node));
        return !typed.empty() && Replace(view, node, typed) ? WidgetResponse::Changed : WidgetResponse::Handled;
    }
    case WidgetGesture::Wheel:
    case WidgetGesture::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

WidgetResponse EditInput(const WidgetView &view, Node &node, const WidgetEvent &event)
{
    TextEdit &edit = node.edit;
    switch (event.key)
    {
    case EditKey::LineStart:
    case EditKey::LineEnd:
    {
        // Control takes it to the ends of the whole text rather than the line,
        // which for a single line is the same place.
        const bool whole = event.step == TextStep::Word || edit.lines == TextLines::Single;
        const uint32_t to = LineEnds(view, node, event.key == EditKey::LineEnd, whole);
        MoveCaret(edit, to, event.reach);
        return WidgetResponse::Handled;
    }
    case EditKey::Backspace:
        return Erase(view, node, event, false);
    case EditKey::Delete:
        return Erase(view, node, event, true);
    case EditKey::SelectAll:
        if (!edit.Can(TextAbility::Select))
        {
            return WidgetResponse::Handled;
        }
        edit.anchor = 0;
        edit.caret = CharacterCount(node.text);
        return WidgetResponse::Handled;
    case EditKey::Copy:
        return Copy(view, node, TextAbility::Copy);
    case EditKey::Cut:
        return Copy(view, node, TextAbility::Cut);
    case EditKey::Paste:
        return Paste(view, node);
    case EditKey::Count:
        break;
    }
    return WidgetResponse::Ignored;
}

} // namespace

WidgetType TextFieldWidget()
{
    // No measure: a field's size is its style's, so that one grows across a row
    // and a selectable label stays the size of the words it holds.
    return WidgetType{.underlay = &DrawSelection, .draw = &DrawCaret, .input = &FieldInput, .cursor = &FieldCursor};
}

} // namespace Assisi::Mondrian
