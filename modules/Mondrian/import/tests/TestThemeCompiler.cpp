/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/Import/ScreenRules.hpp>
#include <Assisi/Mondrian/Import/ThemeCompiler.hpp>

#include <Assisi/Mondrian/Theme.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;

namespace
{

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;

/// A theme with two modes, an accent colour and a spacing in each, and a
/// button style taking its background from the accent.
constexpr std::string_view kDark = R"amth(<theme>
  <palette mode="dark" accent="#e63319" space="8" />
  <palette mode="light" accent="#ffffff" space="12" />
  <style name="button" background="$accent" padding="4" hover:background="#000000" />
  <style name="panel" gap="$space" />
</theme>)amth";

/// Another theme for the same screens, which has the button style and the
/// accent, and no panel.
constexpr std::string_view kContrast = R"amth(<theme>
  <palette mode="contrast" accent="#ffff00" space="16" />
  <style name="button" background="$accent" />
</theme>)amth";

constexpr std::string_view kDarkPath = "ui/Dark.amth";
constexpr std::string_view kContrastPath = "ui/Contrast.amth";

/// A channel written as two hex digits, on the 0 to 1 scale a colour holds.
constexpr float Hex(uint32_t channel)
{
    return static_cast<float>(channel) / 255.f;
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

/// The rules a screen meets with the themes listed: the dark one, and the
/// contrast one too when @p both.
ScreenRules Listing(bool both)
{
    ScreenRules rules;
    rules.themes.push_back(ProjectTheme{.vpath = std::string{kDarkPath}, .compiled = Theme(kDark)});
    if (both)
    {
        rules.themes.push_back(ProjectTheme{.vpath = std::string{kContrastPath}, .compiled = Theme(kContrast)});
    }
    return rules;
}

const ThemeStyle &StyleNamed(const ThemeDocument &theme, std::string_view name)
{
    const ThemeStyle *style = theme.FindStyle(name);
    REQUIRE_MESSAGE(style != nullptr, "no style called " << name);
    return *style;
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

} // namespace

// -----------------------------------------------------------------------------
// A theme
// -----------------------------------------------------------------------------

TEST_CASE("Themes: each palette gives every token a value in its mode, read into every property that can hold it")
{
    const CompiledTheme theme = Theme(kDark);
    REQUIRE(theme.document.modes.size() == 2);
    CHECK(theme.document.modes[0].View() == "dark");
    CHECK(theme.document.modes[1].View() == "light");

    const ThemeToken *accent = theme.document.FindToken("accent");
    REQUIRE(accent != nullptr);
    REQUIRE(accent->values.size() == 2);
    CHECK(accent->values[0].Writes(StyleProperty::Background));
    CHECK(accent->values[0].Writes(StyleProperty::TextColor));
    CHECK_FALSE(accent->values[0].Writes(StyleProperty::Width));
    CHECK(accent->values[0].values.background.r == doctest::Approx(Hex(0xe6)));
    CHECK(accent->values[1].values.background.r == doctest::Approx(1.f));

    const ThemeToken *space = theme.document.FindToken("space");
    REQUIRE(space != nullptr);
    CHECK(space->values[0].Writes(StyleProperty::Gap));
    CHECK(space->values[0].Writes(StyleProperty::Padding));
    CHECK(space->values[0].Writes(StyleProperty::Width));
    CHECK_FALSE(space->values[0].Writes(StyleProperty::Background));
    CHECK(space->values[0].values.gap == Px(8.f));
    CHECK(space->values[1].values.gap == Px(12.f));
}

TEST_CASE("Themes: a style holds a look at rest and one for each state, written out or taken from tokens")
{
    const CompiledTheme theme = Theme(kDark);
    const ThemeStyle &button = StyleNamed(theme.document, "button");

    CHECK(button.resting.patch.written ==
          (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Padding)));
    REQUIRE(button.resting.tokens.size() == 1);
    CHECK(button.resting.tokens[0] ==
          TokenUse{.token = Assisi::Core::InternedString{"accent"}, .property = StyleProperty::Background});
    CHECK(button.resting.patch.values.padding == Padding::All(Px(4.f)));

    const ThemedPatch *hover = LookFor(button.states, StyleState::Hover);
    REQUIRE(hover != nullptr);
    CHECK(hover->patch.written == PropertyBit(StyleProperty::Background));
    CHECK(hover->patch.values.background.r == doctest::Approx(0.f));
}

TEST_CASE("Themes: a style extending another has the base under it, wherever the base is written")
{
    const CompiledTheme theme = Theme(R"amth(<theme>
  <palette mode="dark" accent="#e63319" />
  <style name="primary" extends="button" background="#333333" pressed:background="#444444" />
  <style name="button" extends="base" hover:background="#222222" />
  <style name="base" background="$accent" gap="2" />
</theme>)amth");
    const ThemeStyle &primary = StyleNamed(theme.document, "primary");

    // Its own background wins over the base's token.
    CHECK(primary.resting.tokens.empty());
    CHECK(primary.resting.patch.values.background.r == doctest::Approx(Hex(0x33)));
    CHECK(primary.resting.patch.values.gap == Px(2.f));
    REQUIRE(LookFor(primary.states, StyleState::Hover) != nullptr);
    CHECK(LookFor(primary.states, StyleState::Hover)->patch.values.background.r == doctest::Approx(Hex(0x22)));
    REQUIRE(LookFor(primary.states, StyleState::Pressed) != nullptr);

    // And one that does not write it keeps the base's token.
    const ThemeStyle &button = StyleNamed(theme.document, "button");
    REQUIRE(button.resting.tokens.size() == 1);
    CHECK(button.resting.tokens[0].token.View() == "accent");
}

TEST_CASE("Themes: a theme cooks to bytes the reader reads back")
{
    const std::expected<std::vector<std::byte>, MarkupError> bytes = CompileThemeText(kDark);
    REQUIRE_MESSAGE(bytes.has_value(), Why(bytes));
    const std::expected<ThemeDocument, CookedThemeError> read = ReadCookedTheme(*bytes);
    REQUIRE(read.has_value());
    CHECK(*read == Theme(kDark).document);
}

TEST_CASE("Themes: a theme file that says something a theme cannot is refused where it says it")
{
    struct Case
    {
        std::string_view name;
        std::string_view text;
        std::string_view says;
        uint32_t line;
    };
    const std::vector<Case> cases{
        {"a file starting with something else", "<screen />", "starts with <theme>", 1},
        {"an attribute on the theme", "<theme mode=\"dark\" />", "no attributes", 1},
        {"an element a theme does not have", "<theme>\n  <palette mode=\"a\" />\n  <button />\n</theme>",
         "'button' is not part of a theme", 3},
        {"no palette", "<theme>\n  <style name=\"a\" gap=\"2\" />\n</theme>", "at least one <palette", 1},
        {"a palette with no mode", "<theme>\n  <palette accent=\"#ffffff\" />\n</theme>", "mode=", 2},
        {"a palette holding something", "<theme>\n  <palette mode=\"a\"><style /></palette>\n</theme>",
         "holds nothing", 2},
        {"two palettes for one mode", "<theme>\n  <palette mode=\"a\" />\n  <palette mode=\"a\" />\n</theme>",
         "second palette for mode 'a'", 3},
        {"a token one mode gives and another does not",
         "<theme>\n  <palette mode=\"a\" accent=\"#ffffff\" />\n  <palette mode=\"b\" />\n</theme>",
         "mode 'b' gives no value for 'accent'", 3},
        {"a token no mode before it gave",
         "<theme>\n  <palette mode=\"a\" />\n  <palette mode=\"b\" accent=\"#ffffff\" />\n</theme>",
         "mode 'a' gives no value for 'accent'", 2},
        {"a token's name with a prefix", "<theme>\n  <palette mode=\"a\" hover:accent=\"#ffffff\" />\n</theme>",
         "no token's name", 2},
        {"a token naming another", "<theme>\n  <palette mode=\"a\" ink=\"#ffffff\" accent=\"$ink\" />\n</theme>",
         "one token does not name another", 2},
        {"a style with no name", "<theme>\n  <palette mode=\"a\" />\n  <style gap=\"2\" />\n</theme>", "name=", 3},
        {"two styles with one name",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" />\n  <style name=\"s\" />\n</theme>",
         "second style called 's'", 4},
        {"a style saying what a node does",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" enabled=\"false\" />\n</theme>",
         "only how a node looks", 3},
        {"a style holding one control's own",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" axes=\"y\" />\n</theme>", "every node has", 3},
        {"a style holding no attribute at all",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" colour=\"#ffffff\" />\n</theme>",
         "not an attribute", 3},
        {"a style's value its attribute cannot hold",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" gap=\"wide\" />\n</theme>", "'wide'", 3},
        {"a style's state that is no state",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" held:gap=\"2\" />\n</theme>", "not a state", 3},
        {"a style's token no palette gives",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" background=\"$accent\" />\n</theme>",
         "no token 'accent'", 3},
        {"a style's token a mode gives a value its attribute cannot hold",
         "<theme>\n  <palette mode=\"a\" accent=\"4\" />\n  <palette mode=\"b\" accent=\"#ffffff\" />\n"
         "  <style name=\"s\" width=\"$accent\" />\n</theme>",
         "in mode 'b', which 'width' cannot hold", 4},
        {"a style extending one the theme does not declare",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" extends=\"base\" />\n</theme>",
         "which this theme does not declare", 3},
        {"a style extending itself",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" extends=\"s\" />\n</theme>", "built on itself", 3},
        {"styles extending each other",
         "<theme>\n  <palette mode=\"a\" />\n  <style name=\"s\" extends=\"t\" />\n"
         "  <style name=\"t\" extends=\"s\" />\n</theme>",
         "built on itself", 3},
    };
    for (const Case &refused : cases)
    {
        CAPTURE(refused.name);
        const MarkupError error = ThemeRefused(refused.text);
        CHECK_MESSAGE(error.message.find(refused.says) != std::string::npos, error.message);
        CHECK(error.line == refused.line);
    }
}

// -----------------------------------------------------------------------------
// A screen using the project's themes
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a node naming a style every listed theme has cooks, carrying the name")
{
    const ScreenDocument document =
        Compiled(R"(<screen><button name="go" style="button">Go</button></screen>)", {}, Listing(true));
    CHECK(NodeNamed(document, "go").styleName.View() == "button");
}

TEST_CASE("Themes: a style one listed theme does not have fails the cook, naming the theme and its styles")
{
    const MarkupError error =
        Refused("<screen>\n  <column style=\"panel\" />\n</screen>", {}, Listing(true));
    CHECK(error.line == 2);
    CHECK(error.message.find("ui/Contrast.amth") != std::string::npos);
    CHECK(error.message.find("Its styles are: button.") != std::string::npos);
}

TEST_CASE("Themes: a style or a token named while no theme is listed fails the cook")
{
    CHECK(Refused(R"(<screen><column style="panel" /></screen>)").message.find("lists no theme") !=
          std::string::npos);
    CHECK(Refused(R"(<screen><column background="$accent" /></screen>)").message.find("lists no theme") !=
          std::string::npos);
}

TEST_CASE("Themes: a token a node uses is recorded at rest and in a state, and counts as written")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <button name="go" text_color="$accent" pressed:border_color="$accent">Go</button>
</screen>)amdn",
                                             {}, Listing(true));
    const ScreenNode &go = NodeNamed(document, "go");
    REQUIRE(go.tokens.size() == 1);
    CHECK(go.tokens[0] ==
          TokenUse{.token = Assisi::Core::InternedString{"accent"}, .property = StyleProperty::TextColor});
    CHECK((go.written & PropertyBit(StyleProperty::TextColor)) != 0);
    const ThemedPatch *pressed = LookFor(go.states, StyleState::Pressed);
    REQUIRE(pressed != nullptr);
    REQUIRE(pressed->tokens.size() == 1);
    CHECK(pressed->tokens[0].property == StyleProperty::BorderColor);
    CHECK(pressed->patch.Writes(StyleProperty::BorderColor));
}

TEST_CASE("Themes: a token is checked in every listed theme and every mode")
{
    SUBCASE("one theme does not have it")
    {
        const MarkupError error = Refused("<screen>\n  <column gap=\"$missing\" />\n</screen>", {}, Listing(true));
        CHECK(error.line == 2);
        CHECK(error.message.find("ui/Dark.amth' has no token 'missing'") != std::string::npos);
    }
    SUBCASE("a mode gives a value the attribute cannot hold")
    {
        const MarkupError error = Refused("<screen>\n  <column width=\"$accent\" />\n</screen>", {}, Listing(true));
        CHECK(error.message.find("'#e63319' in mode 'dark', which 'width' cannot hold") != std::string::npos);
    }
    SUBCASE("a name no token can have")
    {
        CHECK(Refused(R"(<screen><column gap="$9" /></screen>)", {}, Listing(true)).message.find("names no token") !=
              std::string::npos);
    }
}

TEST_CASE("Themes: a token gives only how a node looks")
{
    const MarkupError error = Refused(R"(<screen><button enabled="$accent">Go</button></screen>)", {}, Listing(true));
    CHECK(error.message.find("A token gives only how a node looks") != std::string::npos);
}

TEST_CASE("Themes: a node names one style, and a state is written on the style's attributes")
{
    const MarkupError error =
        Refused(R"(<screen><button hover:style="button">Go</button></screen>)", {}, Listing(true));
    CHECK(error.message.find("names one style") != std::string::npos);
}

TEST_CASE("Themes: what a node writes itself is recorded, and a value written over a token replaces it")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
  <template name="swatch">
    <column background="$accent" gap="2" />
  </template>
  <swatch name="plain" />
  <swatch name="painted" background="#ffffff" />
  <row name="line" />
  <scroll name="list" axes="y" />
</screen>)amdn",
                                             {}, Listing(false));

    const ScreenNode &plain = NodeNamed(document, "plain");
    REQUIRE(plain.tokens.size() == 1);
    CHECK(plain.written == (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Gap) |
                            PropertyBit(StyleProperty::Direction)));

    const ScreenNode &painted = NodeNamed(document, "painted");
    CHECK(painted.tokens.empty());
    CHECK((painted.written & PropertyBit(StyleProperty::Background)) != 0);

    // Row and column name their direction; a scroll's is only its default.
    CHECK(NodeNamed(document, "line").written == PropertyBit(StyleProperty::Direction));
    CHECK(NodeNamed(document, "list").written == PropertyBit(StyleProperty::ScrollBars));
    CHECK(document.nodes[0].written == 0);
}

TEST_CASE("Themes: the UI settings' themes are read with the rules, and one that does not compile names itself")
{
    const std::string settings =
        R"({ "version": 1, "type": "UiConfig", "themes": ["ui/Dark.amth", "ui/Contrast.amth"] })";

    const Files files{{"config/ui.json", settings},
                      {"ui/Dark.amth", std::string{kDark}},
                      {"ui/Contrast.amth", std::string{kContrast}}};
    const std::expected<ScreenRules, MarkupError> rules = LoadScreenRules(Serving(files));
    REQUIRE_MESSAGE(rules.has_value(), Why(rules));
    REQUIRE(rules->themes.size() == 2);
    CHECK(rules->themes[0].vpath == kDarkPath);
    CHECK(rules->themes[1].compiled.document.modes[0].View() == "contrast");

    const std::expected<ScreenRules, MarkupError> broken = LoadScreenRules(Serving(
        {{"config/ui.json", settings}, {"ui/Dark.amth", std::string{kDark}}, {"ui/Contrast.amth", "<theme />"}}));
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().file == kContrastPath);

    const std::expected<ScreenRules, MarkupError> missing =
        LoadScreenRules(Serving({{"config/ui.json", settings}, {"ui/Dark.amth", std::string{kDark}}}));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.find("ui/Contrast.amth") != std::string::npos);
}
