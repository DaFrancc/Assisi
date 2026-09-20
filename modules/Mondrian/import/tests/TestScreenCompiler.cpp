/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Import/ScreenCompiler.hpp>

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <doctest/doctest.h>

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
constexpr std::string_view kPauseMenu = R"(<screen name="Pause" input="consume" beneath="hide" pause="true"
        sort="menu" needs="PauseMenu" align="center center"
        background="0 0 0 0.55" blocks_pointer="true">
  <column name="panel" width="fixed 420" padding="32" gap="20"
          background="#1a1c24f0" border_width="2" border_color="#575f7a"
          corner_radius="16" corner_style="rounded" align="center start">
    <text name="title" text_size="48">Paused</text>
    <row name="buttons" gap="20" align="center center">
      <button name="resume" on_click="hide" focus="true"
              padding="28 10 28 10" text_size="28" background="#e63319"
              corner_radius="10" corner_style="rounded">Resume</button>
      <button name="quit" on_click="Game::QuitRequested"
              padding="28 10 28 10" text_size="28" border_width="2"
              border_color="#ffffff" corner_radius="10"
              corner_style="rounded">Quit</button>
    </row>
  </column>
</screen>
)";

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

TEST_CASE("ScreenCompiler: colours read as hex and as numbers")
{
    const ScreenDocument hex = Compiled(R"(<screen background="#ff8000" />)");
    CHECK(hex.nodes[0].style.background.r == doctest::Approx(1.f));
    CHECK(hex.nodes[0].style.background.g == doctest::Approx(0.50196f));
    CHECK(hex.nodes[0].style.background.b == doctest::Approx(0.f));
    CHECK(hex.nodes[0].style.background.a == doctest::Approx(1.f));

    const ScreenDocument withAlpha = Compiled(R"(<screen background="#00000080" />)");
    CHECK(withAlpha.nodes[0].style.background.a == doctest::Approx(0.50196f));

    const ScreenDocument floats = Compiled(R"(<screen background="0.25 0.5 0.75 1" />)");
    CHECK(floats.nodes[0].style.background.r == doctest::Approx(0.25f));
    CHECK(floats.nodes[0].style.background.b == doctest::Approx(0.75f));
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
                                      "  <button focus=\"true\" on_click=\"hide\">A</button>\n"
                                      "  <button focus=\"true\" on_click=\"hide\">B</button>\n"
                                      "</screen>\n");
    CHECK(error.line == 3);
    CHECK(error.message.find("second") != std::string::npos);
}

TEST_CASE("ScreenCompiler: the screen itself cannot be clicked")
{
    // The root is the screen; an action there would never fire.
    CHECK(Refused(R"(<screen on_click="hide" />)").message.find("on_click") != std::string::npos);
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

TEST_CASE("ScreenCompiler: a file that does not parse fails before it compiles")
{
    const std::expected<std::vector<std::byte>, MarkupError> bytes =
        CompileScreenText("<screen>\n  <column>\n</screen>\n", OneEvent());
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error().line == 3);
    CHECK(bytes.error().message.find("never closed") != std::string::npos);
}
