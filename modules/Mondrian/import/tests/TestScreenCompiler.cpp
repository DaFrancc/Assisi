/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <Assisi/Mondrian/Pattern.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/ScreenBlob.hpp>
#include <Assisi/Mondrian/ScreenLoader.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using Assisi::Core::EventCatalog;
using Assisi::Core::EventQueue;

namespace
{

struct QuitRequested
{
};

/// The events a file may name here. Built per case rather than linked, so what
/// an `on_click` resolves against is what this file says it is.
EventCatalog OneEvent()
{
    EventCatalog catalog;
    catalog.Register({.name = "Game::QuitRequested", .push = [](EventQueue &events) { events.Push(QuitRequested{}); }});
    return catalog;
}

/// The pause menu as a file: every element and most of the attribute table.
constexpr std::string_view kPauseMenu = R"amdn(<screen name="Pause" input="consume" beneath="hide" pause="true"
        sort="menu" needs="PauseMenu" align="center center"
        background="rgbf(0, 0, 0, 0.55)" blocks_pointer="true">
  <column name="panel" width="fixed 420" padding="32" gap="20"
          background="#1a1c24f0" border_width="2" border_color="#575f7a"
          corner_radius="16" corner_style="rounded" align="center start">
    <text name="title" text_size="48">Paused</text>
    <row name="buttons" gap="20" align="center center">
      <button name="resume" on_click="hide()" focus="true"
              padding="28 10 28 10" text_size="28" background="#e63319"
              corner_radius="10" corner_style="rounded">Resume</button>
      <button name="quit" on_click="Game::QuitRequested"
              padding="28 10 28 10" text_size="28" border_width="2"
              border_color="#ffffff" corner_radius="10"
              corner_style="rounded">Quit</button>
    </row>
  </column>
</screen>
)amdn";

/// Every control the markup has, with every construction attribute written.
/// One file rather than one per element: what this is for is the whole
/// vocabulary holding together, and a screen that mixes them is what an author
/// writes.
constexpr std::string_view kEveryControl = R"amdn(<screen name="Controls">
  <column name="panel" gap="8">
    <toggle name="fullscreen" on="true" />
    <slider name="volume" min="0" max="100" step="5" value="60" />
    <stepped_slider name="quality" min="0" max="3" steps="4" value="2" />
    <scroll name="list" axes="y">
      <button name="one" on_click="hide()">One</button>
      <button name="louder" on_click="step(volume, 2)">+</button>
    </scroll>
    <text_field name="player" lines="single" placeholder="Name" max_length="24" />
    <text_field name="secret" lines="single" mask="dots" />
    <text_field name="notes" lines="multi up-to 3" />
    <text_field name="port" pattern="/[0-9]+/" check="refuse">8080</text_field>
    <text_field name="who" pattern="email" check="on-commit" />
  </column>
</screen>
)amdn";

/// The reason @p result failed, or empty. Its own function because `*` binds
/// tighter than `?:` inside doctest's message macro, so a ternary written at
/// the call would be parsed as part of the stream expression.
template <typename T, typename E> std::string Why(const std::expected<T, E> &result)
{
    return result.has_value() ? std::string{} : std::string{result.error().message};
}

ScreenDocument Compiled(std::string_view text)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));

    std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, OneEvent());
    REQUIRE_MESSAGE(document.has_value(), Why(document));
    return *document;
}

/// The error from compiling @p text, which the case expects to fail.
MarkupError Refused(std::string_view text)
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));

    const std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, OneEvent());
    REQUIRE_FALSE(document.has_value());
    return document.error();
}

/// The controls the shipped Controls.amdn holds, by the names it gives them.
/// Its containers are left out: what they compile to is already covered, and
/// what this case is about is the arguments a control is made with.
constexpr std::array<std::string_view, 11> kComparedControls{
    {"fullscreen", "volume", "quality", "player", "secret", "port", "who", "growing", "upTo", "exactly", "list"}};

/// The look Controls.amdn gives its scrolling list, so the twin is compared
/// against a node styled as the file styles it rather than against a bare one.
Style ListStyle()
{
    Style style;
    style.sizing = {Sizing::Grow(), Sizing::Fixed(150.f)};
    style.direction = Direction::Column;
    style.background = {0.0588235f, 0.0705882f, 0.0901961f, 1.f};
    style.cornerRadius = 10.f;
    style.cornerStyle = CornerStyle::Rounded;
    style.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    style.scrollSmoothing = 0.12f;
    return style;
}

/// Gives @p id the name @p name, which the case expects to be free.
void Name(Screen &screen, NodeId id, std::string_view name)
{
    REQUIRE(screen.Tree().SetName(id, name).has_value());
}

/// Controls.amdn's controls, built through the node API with the arguments the
/// file writes, under the names the file gives them.
void BuildTwin(Screen &screen)
{
    const NodeId row = screen.Add(screen.Root(), Style{});

    Name(screen, screen.AddToggle(row, true).node, "fullscreen");
    Name(screen, screen.AddContinuousSlider(row, SliderRange{.min = 0.f, .max = 100.f, .step = 5.f}, 60.f).node,
         "volume");
    // No step: a stepped slider moves one position per press whatever its ends
    // are, so the file has no way to say one and neither has this.
    Name(screen, screen.AddSteppedSlider(row, SliderRange{.min = 0.f, .max = 3.f}, 4, 1).node, "quality");

    const TextFieldId player = screen.AddTextField(row, TextLines::Single);
    screen.SetPlaceholder(player, "your name");
    screen.SetMaxLength(player, 24);
    screen.SetText(player, "type here");
    Name(screen, player.node, "player");

    const TextFieldId secret = screen.AddTextField(row, TextLines::Single);
    screen.SetPlaceholder(secret, "password");
    screen.SetText(secret, "hunter2");
    screen.SetMask(secret, TextMask::Dots);
    Name(screen, secret.node, "secret");

    const TextFieldId port = screen.AddTextField(row, TextLines::Single);
    screen.SetPlaceholder(port, "port");
    REQUIRE(screen.SetPattern(port, Patterns::kInteger, TextCheck::Refuse).has_value());
    Name(screen, port.node, "port");

    const TextFieldId who = screen.AddTextField(row, TextLines::Single);
    screen.SetPlaceholder(who, "address");
    REQUIRE(screen.SetPattern(who, "[^@ ]+@[^@ ]+", TextCheck::OnChange).has_value());
    Name(screen, who.node, "who");

    const TextFieldId growing = screen.AddTextField(row, TextLines::Multi);
    screen.SetPlaceholder(growing, "grows forever");
    Name(screen, growing.node, "growing");

    const TextFieldId upTo = screen.AddTextField(row, TextLines::Multi);
    screen.SetHeight(upTo, TextHeight::UpTo, 3);
    screen.SetPlaceholder(upTo, "up to 3 lines");
    Name(screen, upTo.node, "upTo");

    const TextFieldId exactly = screen.AddTextField(row, TextLines::Multi);
    screen.SetHeight(exactly, TextHeight::Exactly, 3);
    screen.SetPlaceholder(exactly, "exactly 3 lines");
    Name(screen, exactly.node, "exactly");

    Name(screen, screen.AddScroll(row, ListStyle(), {false, true}), "list");
}

/// Whether the two nodes called @p name agree on everything making one decides.
void CheckSame(const NodeTree &fromFile, const NodeTree &byHand, std::string_view name)
{
    const Node *const one = fromFile.Get(fromFile.Find(name));
    const Node *const other = byHand.Get(byHand.Find(name));
    REQUIRE_MESSAGE(one != nullptr, "the file has no " << name);
    REQUIRE_MESSAGE(other != nullptr, "the twin has no " << name);

    INFO("control: " << name);

    CHECK(one->behaviour == other->behaviour);
    CHECK(one->value == other->value);
    CHECK(one->text == other->text);
    CHECK(one->range.min == other->range.min);
    CHECK(one->range.max == other->range.max);
    CHECK(one->range.step == other->range.step);
    CHECK(one->steps == other->steps);

    CHECK(one->focusable == other->focusable);
    CHECK(one->takesKeyboard == other->takesKeyboard);
    CHECK(one->blocksPointer == other->blocksPointer);

    // The look a control gives itself. A field written in a file with no style
    // of its own would otherwise be an invisible box where one built in code is
    // not, and nothing else here would notice.
    CHECK(one->style.background.a == other->style.background.a);
    CHECK(one->style.borderWidth == other->style.borderWidth);
    CHECK(one->style.cornerRadius == other->style.cornerRadius);
    CHECK(one->style.textSize == other->style.textSize);
    CHECK(one->style.padding.left == other->style.padding.left);
    CHECK(one->style.sizing[static_cast<std::size_t>(Axis::Y)].kind ==
          other->style.sizing[static_cast<std::size_t>(Axis::Y)].kind);
    CHECK(one->style.enabledScrollBars == other->style.enabledScrollBars);

    CHECK(one->edit.editing == other->edit.editing);
    CHECK(one->edit.placeholder == other->edit.placeholder);
    CHECK(one->edit.maxLength == other->edit.maxLength);
    CHECK(one->edit.lines == other->edit.lines);
    CHECK(one->edit.height == other->edit.height);
    CHECK(one->edit.lineLimit == other->edit.lineLimit);
    CHECK(one->edit.mask == other->edit.mask);
    CHECK(one->edit.check == other->edit.check);
    CHECK((one->edit.pattern == nullptr) == (other->edit.pattern == nullptr));

    // Masking a field turns copy and cut off, so these say whether the loader
    // applied a field's settings in an order that kept that true.
    for (std::size_t ability = 0; ability < kTextAbilityCount; ++ability)
    {
        CHECK(one->edit.abilities[ability] == other->edit.abilities[ability]);
    }
}

/// The node called @p name, which the case expects to exist.
const ScreenNode &NodeNamed(const ScreenDocument &document, std::string_view name)
{
    for (const ScreenNode &node : document.nodes)
    {
        if (node.name == name)
        {
            return node;
        }
    }
    REQUIRE_MESSAGE(false, "no node called " << name);
    return document.nodes[0];
}

uint32_t IndexOf(const ScreenDocument &document, std::string_view name)
{
    for (uint32_t index = 0; index < document.nodes.size(); ++index)
    {
        if (document.nodes[index].name == name)
        {
            return index;
        }
    }
    return kNoNode;
}

} // namespace

TEST_CASE("ScreenCompiler: the pause menu file compiles to the tree it describes")
{
    const ScreenDocument document = Compiled(kPauseMenu);

    CHECK(document.name == "Pause");
    CHECK(document.sortKey == kSortMenu);
    CHECK(document.traits.input == ScreenInput::ConsumeInput);
    CHECK(document.traits.beneath == ScreenBeneath::HidesBeneath);
    CHECK(document.traits.pause == ScreenPause::Pause);
    REQUIRE(document.systems.size() == 1);
    CHECK(document.systems[0] == "PauseMenu");

    // Five nodes: the screen, the panel, the title, the row and two buttons.
    REQUIRE(document.nodes.size() == 6);

    const ScreenNode &root = document.nodes[0];
    CHECK(root.parent == kNoNode);
    CHECK(root.blocksPointer);
    CHECK(root.style.childAlign[0] == Alignment::Center);
    CHECK(root.style.background.a == doctest::Approx(0.55f));

    const ScreenNode &panel = NodeNamed(document, "panel");
    CHECK(panel.parent == 0);
    CHECK(panel.style.direction == Direction::Column);
    CHECK(panel.style.sizing[0].kind == SizingKind::Fixed);
    CHECK(panel.style.sizing[0].value == 420.f);
    CHECK(panel.style.padding.left == 32.f);
    CHECK(panel.style.gap == 20.f);
    CHECK(panel.style.cornerStyle == CornerStyle::Rounded);
    CHECK(panel.style.cornerRadius == 16.f);

    const ScreenNode &title = NodeNamed(document, "title");
    CHECK(title.text == "Paused");
    CHECK(title.style.textSize == 48.f);
    CHECK(title.parent == IndexOf(document, "panel"));

    const ScreenNode &buttons = NodeNamed(document, "buttons");
    CHECK(buttons.style.direction == Direction::Row);

    const ScreenNode &resume = NodeNamed(document, "resume");
    CHECK(resume.widget == BuiltinWidget::Button);
    CHECK(resume.text == "Resume");
    CHECK(resume.action == ActionKind::Verb);
    CHECK(resume.verb == ScreenVerb::Hide);
    CHECK(resume.parent == IndexOf(document, "buttons"));
    CHECK(resume.style.padding.left == 28.f);
    CHECK(resume.style.padding.top == 10.f);

    const ScreenNode &quit = NodeNamed(document, "quit");
    CHECK(quit.action == ActionKind::Event);
    CHECK(quit.eventName == "Game::QuitRequested");

    CHECK(document.focus == IndexOf(document, "resume"));
}

TEST_CASE("ScreenCompiler: the table is preorder with every parent before its children")
{
    // What lets the loader build the tree in one pass, and what the blob reader
    // refuses a document for lacking.
    const ScreenDocument document = Compiled(kPauseMenu);

    for (uint32_t index = 1; index < document.nodes.size(); ++index)
    {
        CHECK(document.nodes[index].parent < index);
    }
    CHECK(document.nodes[0].parent == kNoNode);
}

TEST_CASE("ScreenCompiler: a colour is hex, rgb() or rgbf()")
{
    SUBCASE("hex, with and without alpha")
    {
        const ScreenDocument hex = Compiled(R"(<screen background="#ff8000" />)");
        CHECK(hex.nodes[0].style.background.r == doctest::Approx(1.f));
        CHECK(hex.nodes[0].style.background.g == doctest::Approx(0.50196f));
        CHECK(hex.nodes[0].style.background.b == doctest::Approx(0.f));
        CHECK(hex.nodes[0].style.background.a == doctest::Approx(1.f));

        const ScreenDocument withAlpha = Compiled(R"(<screen background="#00000080" />)");
        CHECK(withAlpha.nodes[0].style.background.a == doctest::Approx(0.50196f));
    }

    SUBCASE("rgb, whole numbers from 0 to 255")
    {
        const ScreenDocument opaque = Compiled(R"amdn(<screen background="rgb(255, 128, 0)" />)amdn");
        CHECK(opaque.nodes[0].style.background.r == doctest::Approx(1.f));
        CHECK(opaque.nodes[0].style.background.g == doctest::Approx(0.50196f));
        CHECK(opaque.nodes[0].style.background.b == doctest::Approx(0.f));
        CHECK(opaque.nodes[0].style.background.a == doctest::Approx(1.f));

        // Alpha is on the same scale as the other channels.
        const ScreenDocument clear = Compiled(R"amdn(<screen background="rgb(0, 0, 0, 128)" />)amdn");
        CHECK(clear.nodes[0].style.background.a == doctest::Approx(0.50196f));
    }

    SUBCASE("rgbf, numbers from 0 to 1")
    {
        const ScreenDocument opaque = Compiled(R"amdn(<screen background="rgbf(0.25, 0.5, 0.75)" />)amdn");
        CHECK(opaque.nodes[0].style.background.r == doctest::Approx(0.25f));
        CHECK(opaque.nodes[0].style.background.b == doctest::Approx(0.75f));
        CHECK(opaque.nodes[0].style.background.a == doctest::Approx(1.f));

        const ScreenDocument clear = Compiled(R"amdn(<screen background="rgbf( 0, 0, 0, 0.55 )" />)amdn");
        CHECK(clear.nodes[0].style.background.a == doctest::Approx(0.55f));
    }

    SUBCASE("every colour attribute reads the same forms")
    {
        const ScreenDocument document =
            Compiled(R"amdn(<screen border_color="rgb(0, 255, 0)" text_color="rgbf(0, 0, 1)" />)amdn");
        CHECK(document.nodes[0].style.borderColor.g == doctest::Approx(1.f));
        CHECK(document.nodes[0].style.textColor.b == doctest::Approx(1.f));
    }
}

namespace
{

/// The error from a text on line 3 whose `background` is @p colour, which the
/// case expects to be refused there.
MarkupError ColourRefusedOnLine3(std::string_view colour)
{
    const MarkupError error = Refused("<screen>\n"
                                      "  <row />\n"
                                      "  <text background=\"" +
                                      std::string{colour} +
                                      "\" />\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    return error;
}

} // namespace

TEST_CASE("ScreenCompiler: a colour that could mean two things, or nothing, is refused where it was written")
{
    SUBCASE("bare numbers, which say no scale")
    {
        // `1 1 1` is white on one scale and nearly black on the other.
        const MarkupError error = ColourRefusedOnLine3("1 1 1");
        CHECK(error.message.find("rgb(") != std::string::npos);
        CHECK(error.message.find("rgbf(") != std::string::npos);
    }

    SUBCASE("an rgb channel past 255")
    {
        CHECK(ColourRefusedOnLine3("rgb(256, 0, 0)").message.find("256") != std::string::npos);
    }

    SUBCASE("an rgb channel that is not whole")
    {
        // The likeliest cause is a 0-to-1 value written in the wrong call.
        const MarkupError error = ColourRefusedOnLine3("rgb(0.5, 0, 0)");
        CHECK(error.message.find("0.5") != std::string::npos);
        CHECK(error.message.find("rgbf") != std::string::npos);
    }

    SUBCASE("an rgbf channel past 1")
    {
        // The likeliest cause is a 0-to-255 value written in the wrong call.
        const MarkupError error = ColourRefusedOnLine3("rgbf(255, 0, 0)");
        CHECK(error.message.find("255") != std::string::npos);
        CHECK(error.message.find("rgb(") != std::string::npos);
    }

    SUBCASE("a negative channel")
    {
        CHECK(ColourRefusedOnLine3("rgbf(-0.1, 0, 0)").message.find("-0.1") != std::string::npos);
    }

    SUBCASE("too few channels")
    {
        CHECK(ColourRefusedOnLine3("rgb(0, 0)").message.find("2") != std::string::npos);
    }

    SUBCASE("too many channels")
    {
        CHECK(ColourRefusedOnLine3("rgb(0, 0, 0, 0, 0)").message.find("5") != std::string::npos);
    }

    SUBCASE("a function that is not a colour")
    {
        const MarkupError error = ColourRefusedOnLine3("hsl(0, 0, 0)");
        CHECK(error.message.find("hsl") != std::string::npos);
        CHECK(error.message.find("rgbf") != std::string::npos);
    }

    SUBCASE("hex with the wrong number of digits")
    {
        CHECK(ColourRefusedOnLine3("#fff").message.find("#fff") != std::string::npos);
    }

    SUBCASE("hex with a digit that is not one")
    {
        CHECK(ColourRefusedOnLine3("#ff00zz").message.find("#ff00zz") != std::string::npos);
    }

    SUBCASE("a call never closed")
    {
        CHECK(ColourRefusedOnLine3("rgb(0, 0, 0").message.find(")") != std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: every sizing form reads")
{
    CHECK(Compiled(R"(<screen width="fit" />)").nodes[0].style.sizing[0].kind == SizingKind::Fit);
    CHECK(Compiled(R"(<screen width="grow" />)").nodes[0].style.sizing[0].kind == SizingKind::Grow);

    const Sizing fixed = Compiled(R"(<screen width="fixed 420" />)").nodes[0].style.sizing[0];
    CHECK(fixed.kind == SizingKind::Fixed);
    CHECK(fixed.value == 420.f);

    const Sizing bounded = Compiled(R"(<screen width="percent 0.5 min 100 max 800" />)").nodes[0].style.sizing[0];
    CHECK(bounded.kind == SizingKind::Percent);
    CHECK(bounded.value == 0.5f);
    CHECK(bounded.min == 100.f);
    CHECK(bounded.max == 800.f);
}

TEST_CASE("ScreenCompiler: a sort key is a named layer or a number between them")
{
    CHECK(Compiled(R"(<screen sort="hud" />)").sortKey == kSortHud);
    CHECK(Compiled(R"(<screen sort="overlay" />)").sortKey == kSortOverlay);
    CHECK(Compiled(R"(<screen sort="1500" />)").sortKey == 1500);
}

TEST_CASE("ScreenCompiler: a name nothing declares is refused where it was written")
{
    // Each fault sits on line 3, so a compiler that reported the element's line
    // for everything could not pass by accident.
    SUBCASE("an unknown element")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column>\n"
                                          "    <spinner />\n"
                                          "  </column>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("spinner") != std::string::npos);
        // The message names what there is, so the fix is in front of whoever
        // reads it.
        CHECK(error.message.find("button") != std::string::npos);
    }

    SUBCASE("an unknown attribute")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column\n"
                                          "      colour=\"#ff0000\" />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("colour") != std::string::npos);
    }

    SUBCASE("an unknown enumerator")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column\n"
                                          "      corner_style=\"bevelled\" />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("bevelled") != std::string::npos);
    }

    SUBCASE("a malformed value")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <column\n"
                                          "      gap=\"12px\" />\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("12px") != std::string::npos);
    }

    SUBCASE("an event nothing declares")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <button\n"
                                          "      on_click=\"Game::Misspelt\">Quit</button>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("Game::Misspelt") != std::string::npos);
        // An event in a header nothing reflects is the way this fails while the
        // name looks right, so the message says where to look.
        CHECK(error.message.find("AEVENT") != std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: a file that is not a screen is refused")
{
    CHECK(Refused(R"(<column />)").message.find("<screen>") != std::string::npos);
}

TEST_CASE("ScreenCompiler: text goes where text goes")
{
    SUBCASE("a container holding words")
    {
        // Almost always a mistake, and dropping it silently would lose what
        // somebody wrote.
        CHECK(Refused("<screen>\n  <column>stray</column>\n</screen>\n").message.find("text") != std::string::npos);
    }

    SUBCASE("a leaf holding elements")
    {
        CHECK(Refused("<screen>\n  <text><column /></text>\n</screen>\n").message.find("elements") !=
              std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: focus is claimed once")
{
    const MarkupError error = Refused("<screen>\n"
                                      "  <button focus=\"true\" on_click=\"hide()\">A</button>\n"
                                      "  <button focus=\"true\" on_click=\"hide()\">B</button>\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    CHECK(error.message.find("second") != std::string::npos);
}

TEST_CASE("ScreenCompiler: a name means one node on its screen")
{
    SUBCASE("a second node carrying it is refused where it was written")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <text name=\"title\">One</text>\n"
                                          "  <text name=\"title\">Two</text>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("title") != std::string::npos);
        // Where the first one is, so whoever reads it can choose which to rename.
        CHECK(error.message.find("line 2") != std::string::npos);
    }

    SUBCASE("however far apart they sit")
    {
        CHECK(Refused("<screen>\n"
                      "  <column name=\"panel\">\n"
                      "    <row><text name=\"label\" /></row>\n"
                      "  </column>\n"
                      "  <row name=\"label\" />\n"
                      "</screen>\n")
                  .line == 5);
    }

    SUBCASE("the screen's own name is not a node's")
    {
        // The root's name is what the screen is called, which a system finds it
        // by; a node inside may share it without the two being confused.
        CHECK(Compiled(R"(<screen name="Pause"><text name="Pause" /></screen>)").nodes[1].name == "Pause");
    }

    SUBCASE("unnamed nodes never collide")
    {
        CHECK(Compiled(R"(<screen><text /><text /><row /><row /></screen>)").nodes.size() == 5);
    }
}

TEST_CASE("ScreenCompiler: a verb is a call, and an event is a bare name")
{
    SUBCASE("hide takes nothing and acts on its own screen")
    {
        const ScreenNode resume = Compiled(R"amdn(<screen><button on_click="hide()" /></screen>)amdn").nodes[1];
        CHECK(resume.action == ActionKind::Verb);
        CHECK(resume.verb == ScreenVerb::Hide);
        CHECK(resume.target == kNoNode);
    }

    SUBCASE("step names a slider that may come after it, and how many moves")
    {
        const ScreenDocument document = Compiled(R"amdn(<screen>
  <button name="quieter" on_click="step(volume, -1)" />
  <slider name="volume" min="0" max="100" step="5" />
  <button name="louder" on_click="step( volume ,3 )" />
</screen>)amdn");
        const ScreenNode &quieter = NodeNamed(document, "quieter");
        CHECK(quieter.action == ActionKind::Verb);
        CHECK(quieter.verb == ScreenVerb::Step);
        CHECK(quieter.target == IndexOf(document, "volume"));
        CHECK(quieter.moves == -1);
        // Space around the arguments is the author's, and means nothing.
        CHECK(NodeNamed(document, "louder").moves == 3);
        CHECK(NodeNamed(document, "louder").target == IndexOf(document, "volume"));
    }

    SUBCASE("a stepped slider is a target too")
    {
        const ScreenDocument document = Compiled(R"amdn(<screen>
  <stepped_slider name="quality" steps="4" />
  <button name="better" on_click="step(quality, 1)" />
</screen>)amdn");
        CHECK(NodeNamed(document, "better").target == IndexOf(document, "quality"));
    }

    SUBCASE("an event is unchanged")
    {
        const ScreenNode quit = Compiled(R"(<screen><button on_click="Game::QuitRequested" /></screen>)").nodes[1];
        CHECK(quit.action == ActionKind::Event);
        CHECK(quit.eventName == "Game::QuitRequested");
    }
}

namespace
{

/// The error from a button on line 3 whose `on_click` is @p onClick, which the
/// case expects to be refused there. Line 2 holds a slider called `volume` and a
/// text called `label`, so a target has something to name.
MarkupError RefusedOnLine3(std::string_view onClick)
{
    const std::string text = "<screen>\n"
                             "  <slider name=\"volume\" step=\"1\" /><text name=\"label\" />\n"
                             "  <button on_click=\"" +
                             std::string{onClick} +
                             "\" />\n"
                             "</screen>\n";
    const MarkupError error = Refused(text);
    // Line 3 and not the slider's, so a compiler reporting the wrong element
    // could not pass by accident.
    CHECK(error.line == 3);
    return error;
}

} // namespace

TEST_CASE("ScreenCompiler: a verb that cannot act is refused where it was written")
{
    SUBCASE("a verb written without parens")
    {
        // One spelling per thing: `hide` bare would be a second way to say it.
        CHECK(RefusedOnLine3("hide").message.find("hide()") != std::string::npos);
    }

    SUBCASE("a verb this build does not have")
    {
        const MarkupError error = RefusedOnLine3("frobnicate()");
        CHECK(error.message.find("frobnicate") != std::string::npos);
        // The message names the verbs there are.
        CHECK(error.message.find("step") != std::string::npos);
    }

    SUBCASE("hide given a target")
    {
        CHECK(RefusedOnLine3("hide(volume)").message.find("hide") != std::string::npos);
    }

    SUBCASE("step given nothing")
    {
        CHECK(RefusedOnLine3("step()").message.find("step") != std::string::npos);
    }

    SUBCASE("step given a target and no count")
    {
        CHECK(RefusedOnLine3("step(volume)").message.find("step") != std::string::npos);
    }

    SUBCASE("step given a count that is not a whole number")
    {
        CHECK(RefusedOnLine3("step(volume, 1.5)").message.find("1.5") != std::string::npos);
    }

    SUBCASE("step that moves nothing")
    {
        CHECK(RefusedOnLine3("step(volume, 0)").message.find("0") != std::string::npos);
    }

    SUBCASE("step naming no node")
    {
        CHECK(RefusedOnLine3("step(volum, 1)").message.find("volum") != std::string::npos);
    }

    SUBCASE("step naming something that is not a slider")
    {
        CHECK(RefusedOnLine3("step(label, 1)").message.find("slider") != std::string::npos);
    }

    SUBCASE("a call never closed")
    {
        CHECK(RefusedOnLine3("step(volume, 1").message.find(")") != std::string::npos);
    }

    SUBCASE("something after the call")
    {
        CHECK(RefusedOnLine3("hide() now").message.find("now") != std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: the screen itself cannot be clicked")
{
    // The root is the screen; an action there would never fire.
    CHECK(Refused(R"amdn(<screen on_click="hide()" />)amdn").message.find("on_click") != std::string::npos);
}

TEST_CASE("ScreenCompiler: a named style is carried and checked against nothing")
{
    // There is nowhere for a style name to resolve to yet. A file written today
    // should cook, and start resolving when there is.
    const ScreenDocument document = Compiled(R"(<screen><column style="Panel" gap="4" /></screen>)");
    CHECK(document.nodes[1].styleName == "Panel");
    CHECK(document.nodes[1].style.gap == 4.f);
}

TEST_CASE("ScreenCompiler: compiling text produces bytes the reader reads back")
{
    // The two halves of the format, against each other: what the cooker writes
    // is what a shipped game reads.
    const std::expected<std::vector<std::byte>, MarkupError> bytes = CompileScreenText(kPauseMenu, OneEvent());
    REQUIRE_MESSAGE(bytes.has_value(), Why(bytes));

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(*bytes);
    REQUIRE(read.has_value());
    CHECK(read->name == "Pause");
    CHECK(read->nodes.size() == 6);
    CHECK(read->systems.size() == 1);
    CHECK(NodeNamed(*read, "quit").eventName == "Game::QuitRequested");
}

TEST_CASE("ScreenCompiler: every construction attribute survives the round trip")
{
    // The two halves of the format against each other, over the fields this
    // issue adds: a write and a read that disagree by one field would shift
    // everything after it and still frame correctly.
    const std::expected<std::vector<std::byte>, MarkupError> bytes = CompileScreenText(kEveryControl, OneEvent());
    REQUIRE_MESSAGE(bytes.has_value(), Why(bytes));

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(*bytes);
    REQUIRE(read.has_value());

    CHECK(NodeNamed(*read, "fullscreen").on);

    const ScreenNode &volume = NodeNamed(*read, "volume");
    CHECK(volume.range.min == 0.f);
    CHECK(volume.range.max == 100.f);
    CHECK(volume.range.step == 5.f);
    CHECK(volume.value == 60.f);

    const ScreenNode &quality = NodeNamed(*read, "quality");
    CHECK(quality.steps == 4);
    CHECK(quality.step == 2);

    CHECK(NodeNamed(*read, "list").style.enabledScrollBars[static_cast<std::size_t>(Axis::Y)]);

    const ScreenNode &player = NodeNamed(*read, "player");
    CHECK(player.placeholder == "Name");
    CHECK(player.maxLength == 24);
    CHECK(player.lines == TextLines::Single);

    CHECK(NodeNamed(*read, "secret").mask == TextMask::Dots);

    const ScreenNode &notes = NodeNamed(*read, "notes");
    CHECK(notes.height == TextHeight::UpTo);
    CHECK(notes.lineLimit == 3);

    const ScreenNode &port = NodeNamed(*read, "port");
    CHECK(port.pattern == "[0-9]+");
    CHECK(port.check == TextCheck::Refuse);

    CHECK(NodeNamed(*read, "who").pattern == Patterns::kEmail);

    // A verb with a target: the name the file wrote, resolved to the node the
    // table holds, survives the binary as that node's place.
    const ScreenNode &louder = NodeNamed(*read, "louder");
    CHECK(louder.action == ActionKind::Verb);
    CHECK(louder.verb == ScreenVerb::Step);
    CHECK(louder.target == IndexOf(*read, "volume"));
    CHECK(louder.moves == 2);
}

TEST_CASE("ScreenCompiler: the pause menu the game ships compiles to what it replaced")
{
    // The file itself, not a copy of it. What this catches is a mistake in the
    // shipped screen, which otherwise nothing would notice until a cook — and
    // no test runs one over the real asset tree.
    std::ifstream file{ASSISI_PAUSE_SCREEN_PATH};
    REQUIRE(file.is_open());
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    // The event the shipped file names, which this binary does not link.
    EventCatalog catalog;
    catalog.Register(
        {.name = "Assisi::App::QuitRequested", .push = [](EventQueue &events) { events.Push(QuitRequested{}); }});

    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));
    const std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, catalog);
    REQUIRE_MESSAGE(document.has_value(), Why(document));

    // Everything the C++ builder this file replaced set, so a difference shows
    // up here rather than as a menu that looks nearly right.
    CHECK(document->name == "Pause");
    CHECK(document->sortKey == kSortMenu);
    CHECK(document->traits.input == ScreenInput::ConsumeInput);
    CHECK(document->traits.beneath == ScreenBeneath::HidesBeneath);
    CHECK(document->traits.pause == ScreenPause::Pause);
    REQUIRE(document->systems.size() == 1);
    CHECK(document->systems[0] == "PauseMenu");

    CHECK(document->nodes[0].blocksPointer);
    CHECK(document->nodes[0].style.childAlign[0] == Alignment::Center);
    CHECK(document->nodes[0].style.childAlign[1] == Alignment::Center);
    CHECK(document->nodes[0].style.background.a == doctest::Approx(0.55f));

    const ScreenNode &panel = NodeNamed(*document, "panel");
    CHECK(panel.style.sizing[0].kind == SizingKind::Fixed);
    CHECK(panel.style.sizing[0].value == 420.f);
    CHECK(panel.style.padding.left == 32.f);
    CHECK(panel.style.gap == 20.f);
    CHECK(panel.style.borderWidth == 2.f);
    CHECK(panel.style.cornerRadius == 16.f);
    CHECK(panel.style.cornerStyle == CornerStyle::Rounded);
    CHECK(panel.style.direction == Direction::Column);

    CHECK(NodeNamed(*document, "title").text == "Paused");
    CHECK(NodeNamed(*document, "title").style.textSize == 48.f);

    const ScreenNode &resume = NodeNamed(*document, "resume");
    CHECK(resume.text == "Resume");
    CHECK(resume.action == ActionKind::Verb);
    CHECK(resume.verb == ScreenVerb::Hide);
    CHECK(resume.style.textSize == 28.f);
    CHECK(resume.style.cornerRadius == 10.f);

    const ScreenNode &quit = NodeNamed(*document, "quit");
    CHECK(quit.text == "Quit");
    CHECK(quit.action == ActionKind::Event);
    CHECK(quit.eventName == "Assisi::App::QuitRequested");

    // Focus starts on Resume, so a player reaching for the keyboard is one
    // press from carrying on rather than one press from leaving.
    CHECK(document->focus == IndexOf(*document, "resume"));
}

TEST_CASE("ScreenCompiler: every control compiles to the node that builds it")
{
    const ScreenDocument document = Compiled(kEveryControl);

    const ScreenNode &fullscreen = NodeNamed(document, "fullscreen");
    CHECK(fullscreen.widget == BuiltinWidget::Toggle);
    CHECK(fullscreen.on);

    const ScreenNode &volume = NodeNamed(document, "volume");
    CHECK(volume.widget == BuiltinWidget::ContinuousSlider);
    CHECK(volume.range.min == 0.f);
    CHECK(volume.range.max == 100.f);
    CHECK(volume.range.step == 5.f);
    CHECK(volume.value == 60.f);

    const ScreenNode &quality = NodeNamed(document, "quality");
    CHECK(quality.widget == BuiltinWidget::SteppedSlider);
    CHECK(quality.range.max == 3.f);
    CHECK(quality.steps == 4);
    // `value` is which step it starts on, counted from zero. A stepped slider
    // moves one step per press whatever its range, so `step` would name
    // something it does not have.
    CHECK(quality.step == 2);

    const ScreenNode &list = NodeNamed(document, "list");
    CHECK(list.widget == BuiltinWidget::Scroll);
    CHECK_FALSE(list.style.enabledScrollBars[static_cast<std::size_t>(Axis::X)]);
    CHECK(list.style.enabledScrollBars[static_cast<std::size_t>(Axis::Y)]);
    // A scroll takes the pointer for its own drags, as AddScroll does, so a
    // file gets the same node either way round.
    CHECK(list.blocksPointer);

    const ScreenNode &player = NodeNamed(document, "player");
    CHECK(player.widget == BuiltinWidget::TextField);
    CHECK(player.lines == TextLines::Single);
    CHECK(player.placeholder == "Name");
    CHECK(player.maxLength == 24);
    // A field made with no style of its own is an invisible box, and a player
    // cannot type into what they cannot see: the document carries the look the
    // node API would have given it.
    CHECK(player.style.borderWidth > 0.f);
    CHECK(player.style.background.a > 0.f);
    CHECK(player.takesKeyboard);

    CHECK(NodeNamed(document, "secret").mask == TextMask::Dots);

    const ScreenNode &notes = NodeNamed(document, "notes");
    CHECK(notes.lines == TextLines::Multi);
    CHECK(notes.height == TextHeight::UpTo);
    CHECK(notes.lineLimit == 3);

    const ScreenNode &port = NodeNamed(document, "port");
    // The delimiters mark it as a regex and are not part of it.
    CHECK(port.pattern == "[0-9]+");
    CHECK(port.check == TextCheck::Refuse);
    // A field's text content is what it starts holding.
    CHECK(port.text == "8080");

    // A preset is expanded here, so the loader knows nothing of presets and the
    // shipped game has no table to look one up in.
    CHECK(NodeNamed(document, "who").pattern == Patterns::kEmail);
}

TEST_CASE("ScreenCompiler: a pattern is a name or a marked regex, never guessed between")
{
    SUBCASE("every built-in preset resolves")
    {
        CHECK(Compiled(R"(<screen><text_field pattern="alphabetic" /></screen>)").nodes[1].pattern ==
              Patterns::kAlphabetic);
        CHECK(Compiled(R"(<screen><text_field pattern="alphanumeric" /></screen>)").nodes[1].pattern ==
              Patterns::kAlphanumeric);
        CHECK(Compiled(R"(<screen><text_field pattern="integer" /></screen>)").nodes[1].pattern == Patterns::kInteger);
        CHECK(Compiled(R"(<screen><text_field pattern="real" /></screen>)").nodes[1].pattern == Patterns::kReal);
        CHECK(Compiled(R"(<screen><text_field pattern="email" /></screen>)").nodes[1].pattern == Patterns::kEmail);
    }

    SUBCASE("a name nothing declares")
    {
        // The whole point of marking the regex: a misspelt preset fails here
        // rather than quietly becoming a pattern matching the letters of its
        // own name, which would accept nothing a player could type.
        const MarkupError error = Refused(R"(<screen><text_field pattern="emial" /></screen>)");
        CHECK(error.message.find("emial") != std::string::npos);
        // The message names what there is, so the fix is in front of whoever
        // reads it.
        CHECK(error.message.find("email") != std::string::npos);
    }

    SUBCASE("a regex written without its delimiters")
    {
        // Reads as a name, and is not one. Saying so beats compiling it.
        CHECK(Refused(R"(<screen><text_field pattern="[0-9]+" /></screen>)").message.find("/") != std::string::npos);
    }

    SUBCASE("an empty pattern takes the rule off")
    {
        CHECK(Compiled(R"(<screen><text_field pattern="" /></screen>)").nodes[1].pattern.empty());
    }
}

TEST_CASE("ScreenCompiler: a field's lines say how many and how tall at once")
{
    const ScreenDocument single = Compiled(R"(<screen><text_field lines="single" /></screen>)");
    CHECK(single.nodes[1].lines == TextLines::Single);
    CHECK(single.nodes[1].height == TextHeight::Unbounded);

    const ScreenDocument grows = Compiled(R"(<screen><text_field lines="multi" /></screen>)");
    CHECK(grows.nodes[1].lines == TextLines::Multi);
    CHECK(grows.nodes[1].height == TextHeight::Unbounded);

    const ScreenDocument exactly = Compiled(R"(<screen><text_field lines="multi exactly 4" /></screen>)");
    CHECK(exactly.nodes[1].height == TextHeight::Exactly);
    CHECK(exactly.nodes[1].lineLimit == 4);

    SUBCASE("a bound with no count")
    {
        CHECK(Refused(R"(<screen><text_field lines="multi up-to" /></screen>)").message.find("up-to") !=
              std::string::npos);
    }

    SUBCASE("a bound of no lines")
    {
        // A field held to no lines could never hold anything, which nobody
        // means by it.
        CHECK(Refused(R"(<screen><text_field lines="multi exactly 0" /></screen>)").message.find("exactly 0") !=
              std::string::npos);
    }

    SUBCASE("a height on a single line")
    {
        CHECK(Refused(R"(<screen><text_field lines="single up-to 3" /></screen>)").message.find("single") !=
              std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: a slider says how far a press moves it")
{
    // The default step is a tenth, which is a tenth of a 0..1 range and a
    // thousandth of a 0..100 one. A file that leaves it out is far likelier to
    // have forgotten than to want a thousand presses end to end.
    SUBCASE("a slider that does not say")
    {
        const MarkupError error = Refused(R"(<screen><slider min="0" max="100" /></screen>)");
        CHECK(error.message.find("step") != std::string::npos);
    }

    SUBCASE("a slider that says none")
    {
        // Zero moves nothing, so the control swallows the key and stays put.
        CHECK(Refused(R"(<screen><slider min="0" max="1" step="0" /></screen>)").message.find("step") !=
              std::string::npos);
    }

    SUBCASE("a stepped slider needs none")
    {
        // It moves one step per press whatever its range, so there is nothing
        // for a step to say.
        const ScreenDocument document = Compiled(R"(<screen><stepped_slider min="0" max="3" steps="4" /></screen>)");
        CHECK(document.nodes[1].steps == 4);
    }
}

TEST_CASE("ScreenCompiler: a pattern that does not compile is refused where it was written")
{
    // The only construction attribute with a failure mode of its own. Caught
    // here, the author reads it with the file open; caught at load, a player
    // reads it instead.
    const MarkupError error = Refused("<screen>\n"
                                      "  <text_field\n"
                                      "      pattern=\"/[0-9/\" />\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    CHECK(error.message.find("pattern") != std::string::npos);
}

TEST_CASE("ScreenCompiler: a scroll spells its axes one way")
{
    // Both names write the same field, and two spellings for one thing is how a
    // file comes to say two different things at once.
    const MarkupError error = Refused(R"(<screen><scroll scroll_bars="y" /></screen>)");
    CHECK(error.message.find("axes") != std::string::npos);
}

TEST_CASE("ScreenCompiler: only a button can be clicked")
{
    // A toggle's press is the toggle's; an on_click there would be read by
    // nothing and do nothing.
    CHECK(Refused(R"amdn(<screen><toggle on_click="hide()" /></screen>)amdn").message.find("button") !=
          std::string::npos);
}

TEST_CASE("ScreenCompiler: a control holds what it can hold")
{
    SUBCASE("a toggle holding words")
    {
        CHECK(Refused(R"(<screen><toggle>on</toggle></screen>)").message.find("text") != std::string::npos);
    }

    SUBCASE("a slider holding elements")
    {
        CHECK(Refused(R"(<screen><slider step="1"><text /></slider></screen>)").message.find("elements") !=
              std::string::npos);
    }

    SUBCASE("a scroll holds elements")
    {
        const ScreenDocument document = Compiled(R"(<screen><scroll axes="xy"><text>in</text></scroll></screen>)");
        REQUIRE(document.nodes.size() == 3);
        CHECK(document.nodes[2].parent == 1);
    }
}

TEST_CASE("ScreenCompiler: the controls file builds the screen the node API builds")
{
    // The whole claim markup makes: a loader over the node API that reaches
    // nothing the API lacks. Both screens are built here and compared control
    // for control, so an argument the cooker drops, or a default the compiler
    // fails to seed, shows up as a difference rather than as a screen that
    // looks nearly right.
    std::ifstream file{ASSISI_CONTROLS_SCREEN_PATH};
    REQUIRE(file.is_open());
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));
    const std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, OneEvent());
    REQUIRE_MESSAGE(document.has_value(), Why(document));

    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = OneEvent();

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, *document, catalog);
    REQUIRE(loaded.has_value());

    Screen twin{ui, ScreenTraits{.input = ScreenInput::ConsumeInput}, kSortPopup, "Twin"};
    BuildTwin(twin);

    for (const std::string_view name : kComparedControls)
    {
        CheckSame(loaded->screen->Tree(), twin.Tree(), name);
    }
}

namespace
{

/// A labelled slider with a stepper beside it: a template that names what it
/// holds and wires one of its parts to another.
constexpr std::string_view kLabelledSlider = R"amdn(
  <template name="labelled_slider">
    <row>
      <text name="label" />
      <button name="down" on_click="step(slider, -1)">-</button>
      <slider name="slider" step="5" />
    </row>
  </template>)amdn";

/// A screen holding @p body and, after it, @p templates.
std::string ScreenWith(std::string_view body, std::string_view templates)
{
    return "<screen>\n" + std::string{body} + std::string{templates} + "\n</screen>\n";
}

/// A chain of @p count templates, each holding an instance of the next, and
/// one instance of the first.
std::string TemplateChain(uint32_t count)
{
    std::string text = "<screen>\n  <t0 />\n";
    for (uint32_t index = 0; index < count; ++index)
    {
        const std::string inner = index + 1 < count ? "<t" + std::to_string(index + 1) + " />" : std::string{};
        text += "  <template name=\"t" + std::to_string(index) + "\"><row>" + inner + "</row></template>\n";
    }
    return text + "</screen>\n";
}

} // namespace

TEST_CASE("ScreenCompiler: an instance is its template's root, with the instance's own over it")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <template name="menu_button">
    <button padding="28 10 28 10" text_size="28" background="#000000">Label</button>
  </template>
  <menu_button name="quit" background="#ffffff">Quit</menu_button>
  <menu_button name="plain" />
</screen>)amdn");

    const ScreenNode &quit = NodeNamed(document, "quit");
    CHECK(quit.widget == BuiltinWidget::Button);
    // The template's, where the instance says nothing.
    CHECK(quit.style.padding.left == 28.f);
    CHECK(quit.style.textSize == 28.f);
    // The instance's, where both do.
    CHECK(quit.style.background.r == doctest::Approx(1.f));
    CHECK(quit.text == "Quit");

    const ScreenNode &plain = NodeNamed(document, "plain");
    CHECK(plain.style.background.r == doctest::Approx(0.f));
    CHECK(plain.text == "Label");
}

TEST_CASE("ScreenCompiler: an instance's children follow its template's")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <template name="panel"><column gap="4"><text name="heading" /></column></template>
  <panel name="box"><text name="extra" /></panel>
</screen>)amdn");

    const uint32_t box = IndexOf(document, "box");
    const uint32_t heading = IndexOf(document, "box.heading");
    const uint32_t extra = IndexOf(document, "extra");
    REQUIRE(box != kNoNode);
    REQUIRE(heading != kNoNode);
    REQUIRE(extra != kNoNode);
    CHECK(document.nodes[heading].parent == box);
    CHECK(document.nodes[extra].parent == box);
    CHECK(heading < extra);
    CHECK(document.nodes[box].style.gap == 4.f);
}

TEST_CASE("ScreenCompiler: a file using templates cooks to the bytes its hand-expanded twin does")
{
    // Nothing of a template reaches the binary: the loader and the runtime
    // cannot tell one was used.
    const std::string_view templated = R"amdn(<screen name="Pause">
  <template name="menu_button">
    <button padding="28 10 28 10" text_size="28" corner_radius="10" corner_style="rounded" />
  </template>
  <row name="buttons">
    <menu_button name="resume" on_click="hide()" focus="true" background="#e63319">Resume</menu_button>
    <menu_button name="quit" on_click="Game::QuitRequested" border_width="2">Quit</menu_button>
  </row>
</screen>)amdn";
    const std::string_view expanded = R"amdn(<screen name="Pause">
  <row name="buttons">
    <button name="resume" on_click="hide()" focus="true" background="#e63319"
            padding="28 10 28 10" text_size="28" corner_radius="10" corner_style="rounded">Resume</button>
    <button name="quit" on_click="Game::QuitRequested" border_width="2"
            padding="28 10 28 10" text_size="28" corner_radius="10" corner_style="rounded">Quit</button>
  </row>
</screen>)amdn";

    const std::expected<std::vector<std::byte>, MarkupError> one = CompileScreenText(templated, OneEvent());
    const std::expected<std::vector<std::byte>, MarkupError> other = CompileScreenText(expanded, OneEvent());
    REQUIRE_MESSAGE(one.has_value(), Why(one));
    REQUIRE_MESSAGE(other.has_value(), Why(other));
    CHECK(*one == *other);
}

TEST_CASE("ScreenCompiler: names inside an instance carry the instance's name")
{
    const ScreenDocument document =
        Compiled(ScreenWith("  <labelled_slider name=\"music\" />\n  <labelled_slider name=\"sfx\" />\n"
                            "  <button name=\"louder\" on_click=\"step(music.slider, 1)\" />\n",
                            kLabelledSlider));

    CHECK(IndexOf(document, "music") != kNoNode);
    CHECK(IndexOf(document, "music.label") != kNoNode);
    CHECK(IndexOf(document, "music.slider") != kNoNode);
    CHECK(IndexOf(document, "sfx.slider") != kNoNode);
    CHECK(IndexOf(document, "slider") == kNoNode);

    // A target inside the template means the part of the same instance.
    CHECK(NodeNamed(document, "music.down").target == IndexOf(document, "music.slider"));
    CHECK(NodeNamed(document, "sfx.down").target == IndexOf(document, "sfx.slider"));
    // And markup outside reaches it by the full name.
    CHECK(NodeNamed(document, "louder").target == IndexOf(document, "music.slider"));
}

TEST_CASE("ScreenCompiler: an instance's own attributes belong to the screen, not the template")
{
    // `slider` exists only inside the instance, as `music.slider`. Written on the
    // instance itself, it is read where the instance sits, and names nothing.
    const MarkupError error =
        Refused(ScreenWith("  <labelled_slider name=\"music\" />\n  <stepper on_click=\"step(slider, 1)\" />\n",
                           std::string{kLabelledSlider} + "\n  <template name=\"stepper\"><button /></template>"));
    CHECK(error.line == 3);
    CHECK(error.message.find("names no node") != std::string::npos);
}

TEST_CASE("ScreenCompiler: an unnamed instance keeps its template's names, so a second one is refused")
{
    SUBCASE("one is fine")
    {
        const ScreenDocument document = Compiled(ScreenWith("  <labelled_slider />\n", kLabelledSlider));
        CHECK(IndexOf(document, "slider") != kNoNode);
    }

    SUBCASE("a second is refused at the instance, and says to name it")
    {
        // The templates come after the instances, so this also shows a template
        // may be used above where it is declared.
        const MarkupError error =
            Refused(ScreenWith("  <labelled_slider />\n  <labelled_slider />\n", kLabelledSlider));
        CHECK(error.line == 3);
        CHECK(error.message.find("labelled_slider") != std::string::npos);
        CHECK(error.message.find("Name the instance") != std::string::npos);
    }

    SUBCASE("a template naming nothing may be used unnamed any number of times")
    {
        const ScreenDocument document = Compiled(R"amdn(<screen>
  <template name="spacer"><row /></template>
  <spacer /><spacer /><spacer />
</screen>)amdn");
        CHECK(document.nodes.size() == 4);
    }
}

TEST_CASE("ScreenCompiler: a name may not hold the separator instances qualify with")
{
    // Otherwise `music.slider` written by hand and the one an instance makes
    // could be two nodes with one name.
    const MarkupError error = Refused("<screen>\n"
                                      "  <row>\n"
                                      "    <text name=\"music.slider\" />\n"
                                      "  </row>\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    CHECK(error.message.find(".") != std::string::npos);
}

TEST_CASE("ScreenCompiler: a template that reaches itself is refused, used or not")
{
    SUBCASE("directly")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <template name=\"a\">\n"
                                          "    <row><a /></row>\n"
                                          "  </template>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("'a'") != std::string::npos);
    }

    SUBCASE("through another")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <template name=\"a\"><row><b /></row></template>\n"
                                          "  <template name=\"b\"><row><a /></row></template>\n"
                                          "</screen>\n");
        CHECK(error.message.find("'a'") != std::string::npos);
        CHECK(error.message.find("'b'") != std::string::npos);
    }
}

TEST_CASE("ScreenCompiler: templates nest as deep as the bound and no deeper")
{
    const ScreenDocument document = Compiled(TemplateChain(kMaxTemplateNesting));
    // The root, then one row per template in the chain.
    CHECK(document.nodes.size() == kMaxTemplateNesting + 1);

    CHECK(Refused(TemplateChain(kMaxTemplateNesting + 1)).message.find(std::to_string(kMaxTemplateNesting)) !=
          std::string::npos);
}

TEST_CASE("ScreenCompiler: a template is declared one way")
{
    // Each fault sits on line 3.
    const std::string before = "<screen>\n  <template name=\"fine\"><row /></template>\n";
    const std::string after = "\n</screen>\n";

    SUBCASE("with no name")
    {
        CHECK(Refused(before + "  <template><row /></template>" + after).line == 3);
    }

    SUBCASE("with a name another template has")
    {
        const MarkupError error = Refused(before + "  <template name=\"fine\"><row /></template>" + after);
        CHECK(error.line == 3);
        CHECK(error.message.find("fine") != std::string::npos);
    }

    SUBCASE("with the name of an element the markup has")
    {
        const MarkupError error = Refused(before + "  <template name=\"button\"><row /></template>" + after);
        CHECK(error.line == 3);
        CHECK(error.message.find("button") != std::string::npos);
    }

    SUBCASE("holding nothing")
    {
        CHECK(Refused(before + "  <template name=\"empty\"></template>" + after).line == 3);
    }

    SUBCASE("holding two roots")
    {
        CHECK(Refused(before + "  <template name=\"two\"><row /><row /></template>" + after).line == 3);
    }

    SUBCASE("holding words")
    {
        CHECK(Refused(before + "  <template name=\"wordy\">stray<row /></template>" + after).line == 3);
    }

    SUBCASE("anywhere but directly in the screen")
    {
        CHECK(Refused(before + "  <row><template name=\"inner\"><row /></template></row>" + after).line == 3);
    }

    SUBCASE("rooted in another template")
    {
        // One layer of overrides: a root that is itself an instance would put a
        // template's attributes under two others.
        CHECK(Refused(before + "  <template name=\"wrapped\"><fine /></template>" + after).line == 3);
    }
}

TEST_CASE("ScreenCompiler: an instance holds what its template's root can hold")
{
    SUBCASE("words on a row")
    {
        CHECK(Refused("<screen>\n"
                      "  <template name=\"strip\"><row /></template>\n"
                      "  <strip>words</strip>\n"
                      "</screen>\n")
                  .line == 3);
    }

    SUBCASE("elements in a text")
    {
        CHECK(Refused("<screen>\n"
                      "  <template name=\"caption\"><text /></template>\n"
                      "  <caption><row /></caption>\n"
                      "</screen>\n")
                  .line == 3);
    }
}

TEST_CASE("ScreenCompiler: every template is checked, used or not")
{
    SUBCASE("a mistake in one nobody uses fails the cook where it is written")
    {
        const MarkupError error = Refused("<screen>\n"
                                          "  <template name=\"unused\">\n"
                                          "    <button colour=\"#ff0000\" />\n"
                                          "  </template>\n"
                                          "</screen>\n");
        CHECK(error.line == 3);
        CHECK(error.message.find("colour") != std::string::npos);
    }

    SUBCASE("and one nobody uses makes no nodes")
    {
        CHECK(Compiled(R"amdn(<screen><template name="t"><row><text /></row></template></screen>)amdn").nodes.size() ==
              1);
    }
}

TEST_CASE("ScreenCompiler: an unknown element names the templates beside the elements")
{
    const MarkupError error = Refused("<screen>\n"
                                      "  <template name=\"menu_button\"><button /></template>\n"
                                      "  <menu_buton />\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    CHECK(error.message.find("button") != std::string::npos);
    CHECK(error.message.find("menu_button") != std::string::npos);
}

TEST_CASE("ScreenCompiler: a file that does not parse fails before it compiles")
{
    const std::expected<std::vector<std::byte>, MarkupError> bytes =
        CompileScreenText("<screen>\n  <column>\n</screen>\n", OneEvent());
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().line == 3);
    CHECK(bytes.error().message.find("never closed") != std::string::npos);
}
