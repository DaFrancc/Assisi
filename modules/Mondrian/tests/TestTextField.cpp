/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Pattern.hpp>
#include <Assisi/Mondrian/TextEdit.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Utf8.hpp>

#include <Assisi/Core/EventQueue.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;
using Assisi::Mondrian::Testing::FixtureFont;

namespace
{

constexpr Extent kScreen{1280, 720};
constexpr TextureId kFontTexture{9};

/// Wide enough that the sample text in these cases fits without scrolling.
constexpr float kFieldTestWidth = 600.f;
/// Narrow enough that the fixture's text wraps to several lines.
constexpr float kLinedTestWidth = 120.f;

/// Text whose characters are not one byte each: "héllo", where the e acute is
/// two bytes, and an emoji, which is four.
constexpr std::string_view kAccented = "h\xC3\xA9llo";
constexpr std::string_view kEmoji = "\xF0\x9F\x98\x80";

/// What a field pushes when it changes, and when it is finished.
struct Typed
{
    std::string text;
};

struct Submitted
{
    std::string text;
};

/// One field on screen, with a clipboard and an event queue of its own.
struct Field
{
    Assisi::Mondrian::Font font = FixtureFont();
    std::string clipboard;
    Assisi::Core::EventQueue events;
    /// After the queue, so the queue outlives it.
    Ui ui{events};
    /// After the Ui, so it is destroyed first: a screen may not outlive the UI
    /// it registered with.
    std::unique_ptr<Screen> held;
    Screen *screen = nullptr;
    TextFieldId field;

    explicit Field(TextLines lines = TextLines::Single)
    {
        ui.SetFont(&font, kFontTexture);
        ui.SetClipboard(Clipboard{.read = [this] { return clipboard; },
                                  .write = [this](std::string_view text) { clipboard = text; }});

        held = std::make_unique<Screen>(ui,
                                        ScreenTraits{.input = ScreenInput::ConsumeInput,
                                                     .beneath = ScreenBeneath::HidesBeneath,
                                                     .pause = ScreenPause::Pause},
                                        kSortMenu, "field-screen");
        screen = held.get();
        ui.Show(*screen);

        field = screen->AddTextField(screen->Root(), lines);
        // Floating and fixed, so it sits at a known place and is the last thing
        // the pointer can hit.
        Style style = screen->Tree().Get(field.node)->style;
        style.floating.enabled = true;
        style.sizing = {Sizing::Fixed(kFieldTestWidth), Sizing::Fit()};
        screen->Tree().SetStyle(field.node, style);
        ui.SetFocus(*screen, field.node);
        Step({});
    }

    /// A field of many lines, narrow enough that its text wraps, held to
    /// @p lines by @p height.
    Field(TextHeight height, uint32_t lines) : Field(TextLines::Multi)
    {
        Style style = screen->Tree().Get(field.node)->style;
        style.sizing = {Sizing::Fixed(kLinedTestWidth), Sizing::Fit()};
        screen->Tree().SetStyle(field.node, style);
        screen->SetHeight(field, height, lines);
        Step({});
    }

    void Step(const UiInput &input)
    {
        ui.ProcessInput(input);
        ui.Sync(kScreen);
    }

    [[nodiscard]] std::string_view Text() const { return screen->GetText(field); }

    [[nodiscard]] const TextEdit &Edit() const { return screen->Tree().Get(field.node)->edit; }

    [[nodiscard]] Rect Box() const
    {
        const LayoutNode *placed = screen->GetLayout().Get(field.node);
        REQUIRE(placed != nullptr);
        return placed->rect;
    }

    /// Types @p text into the field, as the keyboard would deliver it.
    void Type(std::string_view text)
    {
        UiInput input;
        input.grant = InputGrant::Everything;
        input.typed = text;
        Step(input);
    }
};

UiInput Keys()
{
    UiInput input;
    input.grant = InputGrant::Everything;
    return input;
}

/// One press of @p key, with the modifiers @p reach and @p step stand for.
UiInput Press(EditKey key, TextReach reach = TextReach::Moves, TextStep step = TextStep::Character)
{
    UiInput input = Keys();
    input.editPressed[static_cast<std::size_t>(key)] = true;
    input.editDown[static_cast<std::size_t>(key)] = true;
    input.reach = reach;
    input.step = step;
    return input;
}

UiInput Press(UiAction action, TextReach reach = TextReach::Moves, TextStep step = TextStep::Character)
{
    UiInput input = Keys();
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    input.reach = reach;
    input.step = step;
    return input;
}

UiInput PointerAt(Point pointer)
{
    UiInput input = Keys();
    input.pointer = pointer;
    return input;
}

/// The characters of @p text from @p first up to @p last, for saying what a
/// selection holds.
std::string_view Between(std::string_view text, uint32_t first, uint32_t last)
{
    const uint32_t from = CharacterOffset(text, first);
    return text.substr(from, CharacterOffset(text, last) - from);
}

} // namespace

TEST_CASE("TextField: typing puts characters in, whatever they are made of")
{
    Field field;
    field.Type("h");
    field.Type("\xC3\xA9"); // e acute, two bytes
    CHECK(field.Text() == "h\xC3\xA9");
    CHECK(field.Edit().caret == 2);
}

TEST_CASE("TextField: backspace takes a whole character, not a byte of one")
{
    Field field;
    field.Type(kAccented);
    REQUIRE(field.Text() == kAccented);

    field.Step(Press(EditKey::Backspace));
    field.Step(Press(EditKey::Backspace));
    CHECK(field.Text() == "h\xC3\xA9l");

    field.Step(Press(EditKey::Backspace));
    CHECK(field.Text() == "h\xC3\xA9");

    // The accented letter is two bytes and goes as one: what is left is the
    // single byte before it, not half of it.
    field.Step(Press(EditKey::Backspace));
    CHECK(field.Text() == "h");
}

TEST_CASE("TextField: the caret steps over a multi-byte character in one move")
{
    Field field;
    field.Type(kEmoji);
    field.Type("b");
    REQUIRE(field.Edit().caret == 2);

    field.Step(Press(UiAction::Left));
    field.Step(Press(UiAction::Left));
    CHECK(field.Edit().caret == 0);

    field.Step(Press(EditKey::Delete));
    CHECK(field.Text() == "b"); // all four bytes of the emoji, and only those
}

TEST_CASE("TextField: Home and End reach the ends of the text")
{
    Field field;
    field.Type("abc");
    field.Step(Press(EditKey::LineStart));
    CHECK(field.Edit().caret == 0);
    field.Step(Press(EditKey::LineEnd));
    CHECK(field.Edit().caret == 3);
}

TEST_CASE("TextField: Shift with a movement selects, and typing replaces what is selected")
{
    Field field;
    field.Type("abcd");
    field.Step(Press(UiAction::Left, TextReach::Extends));
    field.Step(Press(UiAction::Left, TextReach::Extends));
    CHECK(field.Edit().HasSelection());

    field.Type("X");
    CHECK(field.Text() == "abX");
}

TEST_CASE("TextField: a sideways key with a selection collapses it rather than moving from the caret")
{
    Field field;
    field.Type("abcd");
    field.Step(Press(EditKey::SelectAll));
    field.Step(Press(UiAction::Left));
    CHECK(field.Edit().caret == 0);
    CHECK_FALSE(field.Edit().HasSelection());
}

TEST_CASE("TextField: control with a movement goes a word at a time")
{
    Field field;
    field.Type("foo bar");
    field.Step(Press(UiAction::Left, TextReach::Moves, TextStep::Word));
    CHECK(field.Edit().caret == 4); // the start of "bar"

    field.Type("X");
    CHECK(field.Text() == "foo Xbar");
}

TEST_CASE("TextField: control with backspace takes the word before the caret")
{
    Field field;
    field.Type("foo bar");
    field.Step(Press(EditKey::Backspace, TextReach::Moves, TextStep::Word));
    CHECK(field.Text() == "foo ");
}

TEST_CASE("TextField: select all takes everything, and delete then empties the field")
{
    Field field;
    field.Type("abc");
    field.Step(Press(EditKey::SelectAll));
    CHECK(field.Edit().SelectionFirst() == 0);
    CHECK(field.Edit().SelectionLast() == 3);

    field.Step(Press(EditKey::Delete));
    CHECK(field.Text().empty());
}

TEST_CASE("TextField: copy, cut and paste move text through the clipboard")
{
    Field field;
    field.Type(kAccented);

    field.Step(Press(EditKey::LineEnd));
    field.Step(Press(UiAction::Left, TextReach::Extends));
    field.Step(Press(UiAction::Left, TextReach::Extends));
    field.Step(Press(EditKey::Copy));
    CHECK(field.clipboard == "lo");
    CHECK(field.Text() == kAccented); // copying changes nothing

    field.Step(Press(EditKey::Cut));
    CHECK(field.Text() == "h\xC3\xA9l");

    field.Step(Press(EditKey::LineStart));
    field.Step(Press(EditKey::Paste));
    CHECK(field.Text() == "loh\xC3\xA9l");
}

TEST_CASE("TextField: each ability turned off stops its own feature and nothing else")
{
    SUBCASE("copy")
    {
        Field field;
        field.Type("abc");
        field.screen->SetAbility(field.field, TextAbility::Copy, false);
        field.Step(Press(EditKey::SelectAll));
        field.Step(Press(EditKey::Copy));
        CHECK(field.clipboard.empty());
        CHECK(field.Text() == "abc");
    }
    SUBCASE("cut")
    {
        Field field;
        field.Type("abc");
        field.screen->SetAbility(field.field, TextAbility::Cut, false);
        field.Step(Press(EditKey::SelectAll));
        field.Step(Press(EditKey::Cut));
        CHECK(field.clipboard.empty());
        CHECK(field.Text() == "abc");
    }
    SUBCASE("paste")
    {
        Field field;
        field.clipboard = "xyz";
        field.screen->SetAbility(field.field, TextAbility::Paste, false);
        field.Step(Press(EditKey::Paste));
        CHECK(field.Text().empty());
    }
    SUBCASE("select")
    {
        Field field;
        field.Type("abc");
        field.screen->SetAbility(field.field, TextAbility::Select, false);
        field.Step(Press(EditKey::SelectAll));
        CHECK_FALSE(field.Edit().HasSelection());
    }
}

TEST_CASE("TextField: a masked field shows marks, keeps its text, and stops copying")
{
    Field field;
    field.Type("secret");
    field.screen->SetMask(field.field, TextMask::Dots);
    field.Step({});

    // The text is still the text; only what is shown has changed.
    CHECK(field.Text() == "secret");

    std::string marks;
    CHECK(ShownText(field.Text(), TextMask::Dots, marks) != field.Text());
    CHECK(CharacterCount(marks) == CharacterCount(field.Text()));

    field.Step(Press(EditKey::SelectAll));
    field.Step(Press(EditKey::Copy));
    CHECK(field.clipboard.empty());

    // Turned off rather than forbidden.
    field.screen->SetAbility(field.field, TextAbility::Copy, true);
    field.Step(Press(EditKey::Copy));
    CHECK(field.clipboard == "secret");
}

TEST_CASE("TextField: a length limit counts characters, not the bytes they take")
{
    Field field;
    constexpr uint32_t kLimit = 3;
    field.screen->SetMaxLength(field.field, kLimit);

    field.Type("\xC3\xA9");
    field.Type("\xC3\xA9");
    field.Type("\xC3\xA9");
    CHECK(field.Text() == "\xC3\xA9\xC3\xA9\xC3\xA9"); // three characters, six bytes

    field.Type("\xC3\xA9");
    CHECK(CharacterCount(field.Text()) == kLimit); // and no more
}

TEST_CASE("TextField: a paste past the limit is cut to fit, on a character boundary")
{
    Field field;
    constexpr uint32_t kLimit = 2;
    field.screen->SetMaxLength(field.field, kLimit);
    field.clipboard = "\xC3\xA9\xC3\xA9\xC3\xA9";
    field.Step(Press(EditKey::Paste));
    CHECK(field.Text() == "\xC3\xA9\xC3\xA9");
}

TEST_CASE("TextField: pasted text is cleaned of what a field cannot hold")
{
    SUBCASE("one line turns breaks into spaces and drops control characters")
    {
        Field field;
        field.clipboard = "a\r\nb\x01"
                          "c";
        field.Step(Press(EditKey::Paste));
        CHECK(field.Text() == "a bc");
    }
    SUBCASE("many lines keep the breaks")
    {
        Field field(TextLines::Multi);
        field.clipboard = "a\r\nb\x01"
                          "c";
        field.Step(Press(EditKey::Paste));
        CHECK(field.Text() == "a\nbc");
    }
}

TEST_CASE("TextField: Enter finishes a single line and pushes what it holds")
{
    Field field;
    field.screen->OnSubmit(field.field, [](std::string_view text) { return Submitted{std::string{text}}; });
    field.Type("done");

    field.Step(Press(UiAction::Accept));
    REQUIRE(field.events.Read<Submitted>().size() == 1);
    CHECK(field.events.Read<Submitted>()[0].text == "done");
    CHECK(field.Text() == "done"); // Enter does not put a newline in a single line
}

TEST_CASE("TextField: Enter in a field of many lines puts a line break in")
{
    Field field(TextLines::Multi);
    field.screen->OnSubmit(field.field, [](std::string_view text) { return Submitted{std::string{text}}; });
    field.Type("a");
    field.Step(Press(UiAction::Accept));
    field.Type("b");

    CHECK(field.Text() == "a\nb");
    CHECK(field.events.Read<Submitted>().empty());
}

TEST_CASE("TextField: the caret is as tall as the line it stands on, not as tall as the box")
{
    // One line of text in a box with padding around it: the caret belongs to
    // the text, so it must be shorter than the box that holds it.
    Field field;
    field.Type("AAAA");
    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    REQUIRE(placed->text != LayoutNode::kNoText);

    const TextLayout &text = field.screen->GetLayout().texts[placed->text];
    const Rect line = LineBox(text, 0);
    REQUIRE(line.height > 0.f);

    const Rect caret = field.screen->GetCaretRect(field.field);
    CHECK(caret.height == doctest::Approx(line.height));
    CHECK(caret.height < placed->rect.height); // shorter than the box's padding allows
    CHECK(caret.y >= placed->rect.y);
    CHECK(caret.y + caret.height <= placed->rect.y + placed->rect.height);

    // In a field of many lines it covers the line the caret is on rather than
    // all of them, and it moves down as the caret does.
    Field many(TextLines::Multi);
    Style style = many.screen->Tree().Get(many.field.node)->style;
    style.sizing = {Sizing::Fixed(120.f), Sizing::Fit()};
    many.screen->Tree().SetStyle(many.field.node, style);
    many.Step({});
    many.Type("AAAA AAAA AAAA AAAA");

    const LayoutNode *wrapped = many.screen->GetLayout().Get(many.field.node);
    REQUIRE(wrapped != nullptr);
    REQUIRE(many.screen->GetLayout().texts[wrapped->text].lines.size() > 1);

    const Rect onLast = many.screen->GetCaretRect(many.field);
    CHECK(onLast.height == doctest::Approx(line.height));
    CHECK(onLast.height < wrapped->rect.height);

    many.Step(Press(EditKey::LineStart));
    many.Step(Press(UiAction::Up));
    const Rect onFirst = many.screen->GetCaretRect(many.field);
    CHECK(onFirst.y < onLast.y);
    CHECK(onFirst.height == doctest::Approx(onLast.height));
}

TEST_CASE("TextField: a field of many lines wraps, and the caret moves between the lines")
{
    Field field(TextLines::Multi);
    Style style = field.screen->Tree().Get(field.field.node)->style;
    style.sizing = {Sizing::Fixed(120.f), Sizing::Fit()};
    field.screen->Tree().SetStyle(field.field.node, style);
    field.Step({});
    const float oneLine = field.Box().height;

    field.Type("AAAA AAAA AAAA AAAA");
    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    REQUIRE(placed->text != LayoutNode::kNoText);
    REQUIRE(field.screen->GetLayout().texts[placed->text].lines.size() > 1);

    // Unlike a single line, it grew downwards rather than scrolling sideways.
    CHECK(field.Box().height > oneLine);
    CHECK(placed->textScroll == doctest::Approx(0.f));

    // The caret is on the last line; Up takes it off the end of the text.
    const uint32_t atEnd = field.Edit().caret;
    field.Step(Press(UiAction::Up));
    CHECK(field.Edit().caret < atEnd);
}

namespace
{

/// A field of many lines, narrow enough that "AAAA " fills a line on its own.
/// How many lines the field's text was laid out into.
uint32_t LineCount(const Field &field)
{
    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    REQUIRE(placed->text != LayoutNode::kNoText);
    return static_cast<uint32_t>(field.screen->GetLayout().texts[placed->text].lines.size());
}

} // namespace

TEST_CASE("TextField: a press anywhere but on the field being typed into ends the typing")
{
    Field field;
    field.Type("abc");
    REQUIRE(field.ui.GetInteraction().focused == field.field.node);

    const Rect box = field.Box();
    UiInput press = PointerAt({.x = box.x + box.width + 200.f, .y = box.y + box.height + 200.f});
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    CHECK_FALSE(field.ui.GetInteraction().focused);
    CHECK(field.Text() == "abc"); // what was typed stays typed
}

TEST_CASE("TextField: Back gets out of the field, and only then out of the screen")
{
    Field field;
    REQUIRE(field.ui.GetInteraction().focused == field.field.node);

    field.Step(Press(UiAction::Back));
    CHECK_FALSE(field.ui.GetInteraction().focused);
    // Leaving the box did not also leave the menu it is on.
    CHECK(field.screen->IsShown());

    // With nothing holding the keyboard, Back means what it always meant.
    field.Step(Press(UiAction::Back));
    CHECK_FALSE(field.screen->IsShown());
}

TEST_CASE("TextField: leaving a field settles what it holds")
{
    Field field;
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kEmail, TextCheck::OnCommit).has_value());
    field.Type("jim@");
    REQUIRE(field.screen->GetValidity(field.field) == TextValidity::Unchecked);

    field.Step(Press(UiAction::Back));
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid);
}

TEST_CASE("TextField: a placeholder stands in while the field is empty and goes when it is not")
{
    Field field;
    field.screen->SetPlaceholder(field.field, "your name");
    field.Step({});

    const LayoutNode *empty = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(empty != nullptr);
    CHECK(empty->placeholder);
    CHECK(field.Text().empty()); // shown, but not held

    field.Type("a");
    CHECK(field.Text() == "a");
    CHECK_FALSE(field.screen->GetLayout().Get(field.field.node)->placeholder);

    // And comes back when the field is emptied again.
    field.Step(Press(EditKey::Backspace));
    CHECK(field.screen->GetLayout().Get(field.field.node)->placeholder);
}

TEST_CASE("TextField: a placeholder is drawn fainter than the text it stands in for")
{
    Field field;
    field.screen->SetPlaceholder(field.field, "AAAA");
    field.Step({});

    // The sample screen has text of its own, so take only the glyphs standing
    // inside this field's box.
    const auto glyphAlpha = [&field]
    {
        const Rect box = field.Box();
        for (const QuadInstance &quad : field.ui.GetDrawList().Instances())
        {
            const bool inside = quad.rect.x >= box.x && quad.rect.x <= box.x + box.width && quad.rect.y >= box.y &&
                                quad.rect.y <= box.y + box.height;
            if (quad.kind == static_cast<uint32_t>(QuadKind::Glyph) && inside)
            {
                return quad.color.a;
            }
        }
        return 0.f;
    };
    const float ghost = glyphAlpha();
    REQUIRE(ghost > 0.f);

    field.screen->SetPlaceholder(field.field, {});
    field.Type("AAAA");
    CHECK(glyphAlpha() > ghost);
}

TEST_CASE("TextField: a click on a placeholder leaves the caret where it must be")
{
    Field field;
    field.screen->SetPlaceholder(field.field, "a long prompt to click into");
    field.Step({});

    const Rect box = field.Box();
    UiInput press = PointerAt({.x = box.x + (box.width / 2.f), .y = box.y + (box.height / 2.f)});
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    // There is nowhere else for it to go: the words under the pointer are not
    // the field's to put a caret in.
    CHECK(field.Edit().caret == 0);
    CHECK_FALSE(field.Edit().HasSelection());
}

TEST_CASE("TextField: a masked field shows its placeholder plainly, having nothing to hide yet")
{
    Field field;
    field.screen->SetPlaceholder(field.field, "password");
    field.screen->SetMask(field.field, TextMask::Dots);
    field.Step({});

    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    REQUIRE(placed->placeholder);
    // Eight characters of prompt, not eight marks of nothing.
    CHECK(field.screen->GetLayout().texts[placed->text].glyphs.size() == CharacterCount("password"));
}

TEST_CASE("TextField: a field of many lines grows with its text until it is told not to")
{
    constexpr uint32_t kLines = 2;
    Field field(TextHeight::UpTo, kLines);
    const float empty = field.Box().height;

    field.Type("AAAA ");
    const float one = field.Box().height;
    CHECK(one == doctest::Approx(empty)); // one line still

    field.Type("AAAA ");
    REQUIRE(LineCount(field) == kLines);
    const float two = field.Box().height;
    CHECK(two > one); // grew to the second

    // Full: a third line is refused, and the box does not grow.
    field.Type("AAAA ");
    CHECK(LineCount(field) == kLines);
    CHECK(field.Box().height == doctest::Approx(two));
}

TEST_CASE("TextField: a field of exactly so many lines is that tall while empty")
{
    constexpr uint32_t kLines = 3;
    Field tall(TextHeight::Exactly, kLines);
    Field growing(TextHeight::UpTo, kLines);

    // Nothing typed into either: the fixed one already stands three lines tall.
    CHECK(tall.Box().height > growing.Box().height);

    const float before = tall.Box().height;
    tall.Type("AAAA AAAA ");
    CHECK(tall.Box().height == doctest::Approx(before)); // and does not move
}

TEST_CASE("TextField: a field with no line limit goes on growing")
{
    Field field(TextHeight::Unbounded, 0);
    const float empty = field.Box().height;
    field.Type("AAAA AAAA AAAA AAAA AAAA ");
    CHECK(LineCount(field) > 3);
    CHECK(field.Box().height > empty);
}

TEST_CASE("TextField: what would overflow the lines is refused however it arrives")
{
    constexpr uint32_t kLines = 1;

    SUBCASE("typed")
    {
        Field field(TextHeight::UpTo, kLines);
        field.Type("AAAA ");
        const std::string held{field.Text()};
        field.Type("AAAA ");
        CHECK(field.Text() == held);
    }
    SUBCASE("a line break")
    {
        Field field(TextHeight::UpTo, kLines);
        field.Type("A");
        field.Step(Press(UiAction::Accept));
        CHECK(field.Text() == "A"); // Enter would have made a second line
    }
    SUBCASE("pasted")
    {
        Field field(TextHeight::UpTo, kLines);
        field.clipboard = "AAAA AAAA AAAA";
        field.Step(Press(EditKey::Paste));
        CHECK(field.Text().empty());
    }
}

TEST_CASE("TextField: the line limit and the length limit both hold, whichever comes first")
{
    // Wide text reaches the lines first, and the field is still well inside
    // the character count.
    Field wide(TextHeight::UpTo, 1);
    wide.screen->SetMaxLength(wide.field, 100);
    wide.Type("AAAA ");
    const std::string held{wide.Text()};
    wide.Type("AAAA ");
    CHECK(wide.Text() == held);
    CHECK(CharacterCount(wide.Text()) < 100);

    // A short count reaches its limit long before the lines.
    Field narrow(TextHeight::UpTo, 10);
    narrow.screen->SetMaxLength(narrow.field, 3);
    narrow.Type("AAAAAA");
    CHECK(CharacterCount(narrow.Text()) == 3);
    CHECK(LineCount(narrow) == 1);
}

TEST_CASE("TextField: changing the text pushes what it now holds")
{
    Field field;
    field.screen->OnChange(field.field, [](std::string_view text) { return Typed{std::string{text}}; });
    field.Type("ab");
    REQUIRE(field.events.Read<Typed>().size() == 1);
    CHECK(field.events.Read<Typed>()[0].text == "ab");
}

TEST_CASE("TextField: a key held down goes on acting once the repeat comes round")
{
    Field field;
    field.Type("abcd");

    UiInput held = Press(EditKey::Backspace);
    field.Step(held);
    CHECK(field.Text() == "abc");

    // Still down, but not pressed again, and not yet long enough.
    held.editPressed[static_cast<std::size_t>(EditKey::Backspace)] = false;
    held.time = kNavRepeatDelaySeconds / 2.0;
    field.Step(held);
    CHECK(field.Text() == "abc");

    held.time = kNavRepeatDelaySeconds + kNavRepeatIntervalSeconds;
    field.Step(held);
    CHECK(field.Text() == "ab");
}

TEST_CASE("TextField: a press puts the caret where it landed, and a drag selects")
{
    Field field;
    field.Type("AAAA");
    const Rect box = field.Box();

    // Inside the box, a little way along it: the exact character depends on the
    // font, so what matters is that it is neither the first nor the last.
    const Point middle{.x = box.x + (box.width / 2.f), .y = box.y + (box.height / 2.f)};
    UiInput press = PointerAt(middle);
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);
    CHECK(field.Edit().caret == CharacterCount(field.Text()));
    CHECK_FALSE(field.Edit().HasSelection());

    // Dragging back towards the start selects what it passes over.
    UiInput drag = PointerAt({.x = box.x + 1.f, .y = middle.y});
    drag.primaryDown = true;
    field.Step(drag);
    CHECK(field.Edit().HasSelection());
    CHECK(field.Edit().caret == 0);
}

TEST_CASE("TextField: what is selected can be carried to somewhere else in the text")
{
    Field field;
    field.Type("foo bar");
    const Rect box = field.Box();
    const float middle = box.y + (box.height / 2.f);

    // Select "foo": to the start, then one word to the right with Shift.
    field.Step(Press(EditKey::LineStart));
    field.Step(Press(UiAction::Right, TextReach::Extends, TextStep::Word));
    REQUIRE(field.Edit().SelectionFirst() == 0);
    REQUIRE(Between(field.Text(), field.Edit().SelectionFirst(), field.Edit().SelectionLast()) == "foo ");

    // Press on the selection: it stands rather than collapsing, so there is
    // something left to carry.
    UiInput hold = PointerAt({.x = box.x + 1.f, .y = middle});
    hold.primaryDown = true;
    hold.primaryPressed = true;
    field.Step(hold);
    CHECK(field.Edit().HasSelection());
    CHECK(field.Edit().drag == TextDrag::Held);

    // Carry it to the end and drop it there.
    UiInput carry = PointerAt({.x = box.x + box.width - 1.f, .y = middle});
    carry.primaryDown = true;
    const InputResult carrying = field.ui.ProcessInput(carry);
    field.ui.Sync(kScreen);
    CHECK(field.Edit().drag == TextDrag::Moving);
    // The pointer says what is happening: carrying text, not pointing into it.
    CHECK(carrying.cursor == Assisi::Core::CursorShape::Move);

    UiInput drop = PointerAt({.x = box.x + box.width - 1.f, .y = middle});
    drop.primaryReleased = true;
    field.Step(drop);

    CHECK(field.Edit().drag == TextDrag::None);
    CHECK(field.Text() == "barfoo ");
    // And it is still selected where it landed, ready to be carried again.
    CHECK(field.Edit().HasSelection());
    CHECK(Between(field.Text(), field.Edit().SelectionFirst(), field.Edit().SelectionLast()) == "foo ");
}

TEST_CASE("TextField: a press on a selection that goes nowhere is an ordinary click")
{
    Field field;
    field.Type("foo bar");
    field.Step(Press(EditKey::SelectAll));
    const Rect box = field.Box();
    // On the first character, which is inside the selection. Further right
    // would be past the end of the words and so outside it.
    const Point inside{.x = box.x + 1.f, .y = box.y + (box.height / 2.f)};

    UiInput press = PointerAt(inside);
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);
    CHECK(field.Edit().HasSelection()); // still standing while it is held

    UiInput release = PointerAt(inside);
    release.primaryReleased = true;
    field.Step(release);

    CHECK_FALSE(field.Edit().HasSelection()); // and gone once it is let go
    CHECK(field.Text() == "foo bar");
}

TEST_CASE("TextField: dropping a selection on itself changes nothing")
{
    Field field;
    field.Type("foo bar");
    field.Step(Press(EditKey::SelectAll));
    const Rect box = field.Box();
    const Point inside{.x = box.x + 1.f, .y = box.y + (box.height / 2.f)};

    UiInput press = PointerAt(inside);
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    // Barely moved, so the drop lands back inside what was lifted.
    UiInput carry = PointerAt({.x = inside.x + 4.f, .y = inside.y});
    carry.primaryDown = true;
    field.Step(carry);

    UiInput drop = PointerAt({.x = inside.x + 4.f, .y = inside.y});
    drop.primaryReleased = true;
    field.Step(drop);
    CHECK(field.Text() == "foo bar");
}

TEST_CASE("TextField: with carrying turned off, a press on a selection just moves the caret")
{
    Field field;
    field.Type("foo bar");
    field.screen->SetAbility(field.field, TextAbility::Drag, false);
    field.Step(Press(EditKey::SelectAll));

    const Rect box = field.Box();
    UiInput press = PointerAt({.x = box.x + 1.f, .y = box.y + (box.height / 2.f)});
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    CHECK(field.Edit().drag == TextDrag::None);
    CHECK_FALSE(field.Edit().HasSelection()); // collapsed at once, as a click does
}

TEST_CASE("TextField: a double click takes the word under it")
{
    Field field;
    field.Type("foo bar");
    const Rect box = field.Box();
    const Point start{.x = box.x + 1.f, .y = box.y + (box.height / 2.f)};

    UiInput press = PointerAt(start);
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    UiInput again = press;
    again.time = kNavRepeatIntervalSeconds;
    field.Step(again);

    CHECK(field.Edit().SelectionFirst() == 0);
    CHECK(field.Edit().SelectionLast() == 3); // "foo", without the space
}

TEST_CASE("TextField: a pattern that refuses keeps what it will not accept out")
{
    Field field;
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kInteger, TextCheck::Refuse).has_value());

    field.Type("4");
    field.Type("2");
    CHECK(field.Text() == "42");

    field.Type("x");
    CHECK(field.Text() == "42"); // never enters

    // And a field can always be emptied, whatever its pattern says of nothing.
    field.Step(Press(EditKey::SelectAll));
    field.Step(Press(EditKey::Delete));
    CHECK(field.Text().empty());
}

TEST_CASE("TextField: a pattern that marks as typed says so the moment it stops matching")
{
    Field field;
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kEmail, TextCheck::OnChange).has_value());
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Unchecked);

    field.Type("jim@");
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid);

    field.Type("example.com");
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Valid);
}

TEST_CASE("TextField: a pattern that marks on commit says nothing until Enter")
{
    Field field;
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kEmail, TextCheck::OnCommit).has_value());

    field.Type("jim@");
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Unchecked);

    field.Step(Press(UiAction::Accept));
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid);

    field.Type("example.com");
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid); // not judged again until committed

    field.Step(Press(UiAction::Accept));
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Valid);
}

TEST_CASE("TextField: leaving a field is finishing with it, so it is judged then too")
{
    Field field;
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kEmail, TextCheck::OnCommit).has_value());
    field.Type("jim@");
    REQUIRE(field.screen->GetValidity(field.field) == TextValidity::Unchecked);

    // Somewhere for the tab order to go: with only the field on the screen,
    // Next comes round to it and focus never leaves.
    field.screen->AddButton(field.screen->Root(), "elsewhere");
    field.Step({});

    // Focus moves to whatever is next in the tab order; the field hears about
    // it and settles what it holds.
    field.Step(Press(UiAction::Next));
    REQUIRE(field.ui.GetInteraction().focused != field.field.node);
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid);
}

TEST_CASE("TextField: a pattern that will not compile is refused and leaves the field as it was")
{
    Field field;
    const std::expected<void, PatternError> set = field.screen->SetPattern(field.field, "[unclosed", TextCheck::Refuse);
    REQUIRE_FALSE(set.has_value());
    CHECK_FALSE(set.error().message.empty());

    field.Type("anything");
    CHECK(field.Text() == "anything");
}

TEST_CASE("TextField: text set from code is taken as given, limit and pattern notwithstanding")
{
    Field field;
    field.screen->SetMaxLength(field.field, 2);
    REQUIRE(field.screen->SetPattern(field.field, Patterns::kInteger, TextCheck::Refuse).has_value());

    field.screen->SetText(field.field, "a longer answer");
    CHECK(field.Text() == "a longer answer");
    CHECK(field.Edit().caret == CharacterCount(field.Text()));

    // Taken as given, but not pretended to be acceptable: text put in from
    // code is finished text, so the pattern has its say about it.
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Invalid);
    field.screen->SetText(field.field, "42");
    CHECK(field.screen->GetValidity(field.field) == TextValidity::Valid);
}

TEST_CASE("TextField: a selectable label may be copied but not typed into")
{
    Field field;
    const NodeId label = field.screen->Tree().Create(field.screen->Tree().Root(), "label");
    field.screen->Tree().SetText(label, "10.0.0.1");
    field.screen->SetSelectable(label, true);
    // Laid out before it is focused: focus is checked against the last layout,
    // and a node that has not been placed yet cannot hold it.
    field.Step({});
    field.ui.SetFocus(*field.screen, label);
    field.Step({});

    field.Step(Press(EditKey::SelectAll));
    field.Step(Press(EditKey::Copy));
    CHECK(field.clipboard == "10.0.0.1");

    UiInput typing = Keys();
    typing.typed = "x";
    field.Step(typing);
    CHECK(field.screen->Tree().Get(label)->text == "10.0.0.1");
}

TEST_CASE("TextField: the pointer takes the text shape over a field, and keeps it through a drag")
{
    Field field;
    field.Type("AAAA");
    const Rect box = field.Box();
    const Point inside{.x = box.x + (box.width / 2.f), .y = box.y + (box.height / 2.f)};

    // Away from the field, the UI has nothing to say about the pointer.
    UiInput away = PointerAt({.x = box.x + box.width + 200.f, .y = inside.y});
    CHECK(field.ui.ProcessInput(away).cursor == Assisi::Core::CursorShape::Arrow);
    field.ui.Sync(kScreen);

    UiInput over = PointerAt(inside);
    CHECK(field.ui.ProcessInput(over).cursor == Assisi::Core::CursorShape::Text);
    field.ui.Sync(kScreen);

    // Held, and dragged off the box: what has hold of the pointer decides.
    UiInput press = PointerAt(inside);
    press.primaryDown = true;
    press.primaryPressed = true;
    field.Step(press);

    UiInput drag = PointerAt({.x = box.x + box.width + 200.f, .y = inside.y});
    drag.primaryDown = true;
    CHECK(field.ui.ProcessInput(drag).cursor == Assisi::Core::CursorShape::Text);
    field.ui.Sync(kScreen);
}

TEST_CASE("TextField: a field does not grow with what is typed into it")
{
    Field field;
    // Sized by its parent rather than fixed, which is what a field in a row
    // gets and what made it grow with its text.
    Style style = field.screen->Tree().Get(field.field.node)->style;
    style.sizing = {Sizing{.min = 80.f, .kind = SizingKind::Grow}, Sizing::Fit()};
    field.screen->Tree().SetStyle(field.field.node, style);
    field.Step({});

    const Rect empty = field.Box();
    field.Type("a line far longer than the box it is being typed into, and then some more");
    const Rect filled = field.Box();

    CHECK(filled.width == doctest::Approx(empty.width));
    CHECK(filled.x == doctest::Approx(empty.x));
}

TEST_CASE("TextField: what runs past the ends of a field is clipped to it")
{
    Field field;
    Style style = field.screen->Tree().Get(field.field.node)->style;
    style.sizing = {Sizing::Fixed(80.f), Sizing::Fit()};
    field.screen->Tree().SetStyle(field.field.node, style);
    field.Step({});
    field.Type("AAAAAAAAAAAAAAAAAAAA");

    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    CHECK(placed->clip.x >= placed->rect.x);
    CHECK(placed->clip.x + placed->clip.width <= placed->rect.x + placed->rect.width);
}

TEST_CASE("TextField: a long single line scrolls to keep the caret in sight")
{
    Field field;
    Style style = field.screen->Tree().Get(field.field.node)->style;
    style.sizing = {Sizing::Fixed(80.f), Sizing::Fit()};
    field.screen->Tree().SetStyle(field.field.node, style);
    field.Step({});

    field.Type("AAAAAAAAAAAAAAAA");
    const LayoutNode *placed = field.screen->GetLayout().Get(field.field.node);
    REQUIRE(placed != nullptr);
    CHECK(placed->textScroll > 0.f);

    // And comes back when the caret does.
    field.Step(Press(EditKey::LineStart));
    field.Step({});
    CHECK(field.screen->GetLayout().Get(field.field.node)->textScroll == doctest::Approx(0.f));
}
