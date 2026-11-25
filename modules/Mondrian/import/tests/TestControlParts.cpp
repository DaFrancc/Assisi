/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/Import/ScreenRules.hpp>
#include <Assisi/Mondrian/Import/ThemeCompiler.hpp>

#include <Assisi/Mondrian/ScreenDocument.hpp>
#include <Assisi/Mondrian/Theme.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;

namespace
{

/// A channel written as two hex digits, on the 0 to 1 scale a colour holds.
constexpr float Hex(uint32_t channel)
{
    return static_cast<float>(channel) / 255.f;
}

/// The node standing for the part called @p part of the node at @p control,
/// which the case expects to exist.
const ScreenNode &PartNode(const ScreenDocument &document, uint32_t control, std::string_view part)
{
    for (const ScreenNode &node : document.nodes)
    {
        if (node.parent == control && node.part.View() == part)
        {
            return node;
        }
    }
    REQUIRE_MESSAGE(false, "no part called " << part);
    return document.nodes[0];
}

/// How many nodes stand for the part called @p part of the node at @p control.
uint32_t PartNodes(const ScreenDocument &document, uint32_t control, std::string_view part)
{
    uint32_t count = 0;
    for (const ScreenNode &node : document.nodes)
    {
        count += node.parent == control && node.part.View() == part ? 1U : 0U;
    }
    return count;
}

const ThemedPatch *LookFor(const std::vector<ThemedPatch> &looks, StyleState state)
{
    for (const ThemedPatch &look : looks)
    {
        if (look.patch.state == state)
        {
            return &look;
        }
    }
    return nullptr;
}

CompiledTheme Theme(std::string_view text)
{
    std::expected<CompiledTheme, MarkupError> theme = CompileThemeSource(text);
    REQUIRE_MESSAGE(theme.has_value(), Why(theme));
    return std::move(*theme);
}

MarkupError ThemeRefused(std::string_view text)
{
    const std::expected<CompiledTheme, MarkupError> theme = CompileThemeSource(text);
    REQUIRE_FALSE(theme.has_value());
    return theme.error();
}

const ThemePartStyle *PartStyle(const ThemeStyle &style, std::string_view part)
{
    for (const ThemePartStyle &held : style.parts)
    {
        if (held.name.View() == part)
        {
            return &held;
        }
    }
    return nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
// A screen
// -----------------------------------------------------------------------------

TEST_CASE("ControlParts: a part's attributes, prefixed with its name, are its own look at rest and in each state")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <slider name="volume" step="5" thumb:background="#336699" thumb:hover:background="#ffffff"
          track:visible="false" background="#111111" />
</screen>)amdn");
    const uint32_t slider = IndexOf(document, "volume");

    const ScreenNode &thumb = PartNode(document, slider, "thumb");
    CHECK(thumb.written == PropertyBit(StyleProperty::Background));
    CHECK(thumb.style.background.b == doctest::Approx(Hex(0x99)));
    const ThemedPatch *hover = LookFor(thumb.states, StyleState::Hover);
    REQUIRE(hover != nullptr);
    CHECK(hover->patch.values.background.r == doctest::Approx(1.f));
    CHECK_FALSE(PartNode(document, slider, "track").visible);
    // The slider's own look stays its own.
    CHECK(document.nodes[slider].written == PropertyBit(StyleProperty::Background));
    CHECK(PartNodes(document, slider, "fill") == 0);
}

TEST_CASE("ControlParts: a part's tag holds its look and what it shows, and its attributes join the prefixed ones")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <slider name="volume" step="5" thumb:background="#336699">
    <thumb on:border_width="2" padding="2">
      <text name="value">60</text>
    </thumb>
  </slider>
</screen>)amdn");
    const uint32_t slider = IndexOf(document, "volume");

    REQUIRE(PartNodes(document, slider, "thumb") == 1);
    const ScreenNode &thumb = PartNode(document, slider, "thumb");
    CHECK(thumb.written == (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Padding)));
    CHECK(LookFor(thumb.states, StyleState::On) != nullptr);
    const uint32_t value = IndexOf(document, "value");
    REQUIRE(value != kNoNode);
    CHECK(&document.nodes[document.nodes[value].parent] == &thumb);
}

TEST_CASE("ControlParts: a template's parts are the instance's, and what the instance writes wins")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <template name="gold_slider">
    <slider step="5" thumb:background="#ffcc00" thumb:padding="1" />
  </template>
  <gold_slider name="volume" thumb:background="#000000" />
</screen>)amdn");
    const uint32_t slider = IndexOf(document, "volume");

    REQUIRE(PartNodes(document, slider, "thumb") == 1);
    const ScreenNode &thumb = PartNode(document, slider, "thumb");
    CHECK(thumb.style.background.r == doctest::Approx(0.f));
    CHECK(thumb.written == (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Padding)));
}

TEST_CASE("ControlParts: a part that is not there, or a prefix where it cannot go, is refused naming what is")
{
    struct Case
    {
        std::string_view name;
        std::string_view text;
        std::string_view says;
    };
    const std::vector<Case> cases{
        {"a part the control does not have", R"(<screen><slider step="5" knob:background="#ffffff" /></screen>)",
         "'knob' is not a state or a part of slider. The states are: on, focused, hover, pressed, disabled; "
         "slider's parts are: track, fill, thumb"},
        {"a part's tag the control does not have", R"(<screen><toggle><thumb /></toggle></screen>)",
         "'thumb' is not a part of toggle. Its parts are: track, knob"},
        {"a prefix on a node with no parts", R"(<screen><row thumb:background="#ffffff" /></screen>)",
         "'thumb' is not a state. The states are"},
        {"two states", R"(<screen><slider step="5" hover:pressed:background="#ffffff" /></screen>)",
         "a part, then a state"},
        {"a state before a part", R"(<screen><slider step="5" hover:thumb:background="#ffffff" /></screen>)",
         "a part, then a state"},
        {"a part saying what it does", R"(<screen><slider step="5" thumb:on_change="Game::QuitRequested" /></screen>)",
         "a part takes only how it looks"},
        {"a part naming a style", R"(<screen><slider step="5"><thumb style="big" /></slider></screen>)",
         "a part takes only how it looks"},
        {"a control inside a part", R"(<screen><slider step="5"><thumb><button>Go</button></thumb></slider></screen>)",
         "passes the pointer to its slider"},
        {"a part's tag inside a scroll", R"(<screen><scroll axes="y"><thumb /></scroll></screen>)",
         "holds what it scrolls"},
    };
    for (const Case &refused : cases)
    {
        CAPTURE(refused.name);
        const MarkupError error = Refused(refused.text);
        CHECK_MESSAGE(error.message.find(refused.says) != std::string::npos, error.message);
    }
}

TEST_CASE("ControlParts: a scroll's parts are styled by prefix, one look for both its bars")
{
    const ScreenDocument document =
        Compiled(R"(<screen><scroll name="list" axes="xy" thumb:background="#ffffff" /></screen>)");
    CHECK(PartNodes(document, IndexOf(document, "list"), "thumb") == 1);
}

// -----------------------------------------------------------------------------
// A theme
// -----------------------------------------------------------------------------

TEST_CASE("ControlParts: a style holds each part's look at rest and in each state, and a style extending it keeps them")
{
    const CompiledTheme theme = Theme(R"amth(<theme>
  <palette mode="dark" accent="#e63319" />
  <style name="slider" thumb:background="$accent" thumb:hover:background="#ffffff" track:height="4" />
  <style name="big_slider" extends="slider" thumb:width="30" />
</theme>)amth");

    const ThemeStyle *slider = theme.document.FindStyle("slider");
    REQUIRE(slider != nullptr);
    CHECK(slider->resting.patch.written == 0);
    const ThemePartStyle *thumb = PartStyle(*slider, "thumb");
    REQUIRE(thumb != nullptr);
    REQUIRE(thumb->resting.tokens.size() == 1);
    CHECK(thumb->resting.tokens[0].token.View() == "accent");
    REQUIRE(LookFor(thumb->states, StyleState::Hover) != nullptr);
    REQUIRE(PartStyle(*slider, "track") != nullptr);

    const ThemeStyle *big = theme.document.FindStyle("big_slider");
    REQUIRE(big != nullptr);
    const ThemePartStyle *bigThumb = PartStyle(*big, "thumb");
    REQUIRE(bigThumb != nullptr);
    CHECK(bigThumb->resting.patch.written ==
          (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Width)));
    CHECK(LookFor(bigThumb->states, StyleState::Hover) != nullptr);
    CHECK(PartStyle(*big, "track") != nullptr);
}

TEST_CASE("ControlParts: a theme refuses a part no control has, and one a style named for a control lacks")
{
    const MarkupError anyStyle = ThemeRefused(R"amth(<theme>
  <palette mode="dark" />
  <style name="fancy" wheel:background="#ffffff" />
</theme>)amth");
    CHECK_MESSAGE(anyStyle.message.find("'wheel' is not a state or a part of any control") != std::string::npos,
                  anyStyle.message);
    CHECK(anyStyle.line == 3);

    const MarkupError controlStyle = ThemeRefused(R"amth(<theme>
  <palette mode="dark" />
  <style name="toggle" thumb:background="#ffffff" />
</theme>)amth");
    CHECK_MESSAGE(controlStyle.message.find("toggle's parts are: track, knob") != std::string::npos,
                  controlStyle.message);
}

// -----------------------------------------------------------------------------
// The book
// -----------------------------------------------------------------------------

TEST_CASE("ControlParts: the book's table of parts lists each control's parts as the controls have them")
{
    std::ifstream file{ASSISI_BOOK_CONTROLS_PATH};
    REQUIRE(file.is_open());
    std::stringstream read;
    read << file.rdbuf();
    const std::string book = read.str();

    for (const BuiltinWidget widget : {BuiltinWidget::Toggle, BuiltinWidget::ContinuousSlider,
                                       BuiltinWidget::SteppedSlider, BuiltinWidget::Scroll})
    {
        std::string row = "| `" + std::string{BuiltinWidgetName(widget)} + "` |";
        std::string listed;
        for (const std::string_view part : BuiltinPartNames(widget))
        {
            listed += listed.empty() ? " " : ", ";
            listed += "`" + std::string{part} + "`";
        }
        row += listed + " |";
        CHECK_MESSAGE(book.find(row) != std::string::npos, "the book has no row " << row);
    }
}
