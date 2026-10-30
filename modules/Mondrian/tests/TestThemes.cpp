/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Theme.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/ScreenLoader.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using Assisi::Core::InternedString;

namespace
{

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;

constexpr Extent kViewport{1280, 720};

/// One colour per place a value can come from, none of them a default, so what
/// a node wears says where it came from.
constexpr Color kStyleResting{0.1f, 0.f, 0.f, 1.f};
constexpr Color kOwnResting{0.f, 0.1f, 0.f, 1.f};
constexpr Color kStyleHover{0.2f, 0.f, 0.f, 1.f};
constexpr Color kOwnHover{0.f, 0.2f, 0.f, 1.f};
constexpr Color kStyleDisabled{0.3f, 0.3f, 0.3f, 1.f};
constexpr Color kDarkAccent{0.9f, 0.2f, 0.1f, 1.f};
constexpr Color kLightAccent{0.1f, 0.2f, 0.9f, 1.f};
constexpr Color kContrastAccent{1.f, 1.f, 0.f, 1.f};
constexpr Color kContrastButton{0.f, 0.f, 0.f, 1.f};
/// The text colour a style starts with, which a node writing none shows.
constexpr Color kDefaultText{1.f, 1.f, 1.f, 1.f};

/// The gap the button style gives, in UI pixels.
constexpr float kStyleGap = 12.f;

/// Sizes in UI pixels. These tests set no font, so every node is given a size
/// of its own to have something to point at.
constexpr float kRowWidth = 400.f;
constexpr float kRowHeight = 80.f;
constexpr float kLabelWidth = 200.f;
constexpr float kButtonWidth = 150.f;

/// Where these themes are listed: what game code names them by.
constexpr std::string_view kDarkPath = "ui/Dark.amth";
constexpr std::string_view kContrastPath = "ui/Contrast.amth";

/// A look writing only the background, @p background, for @p state.
StylePatch Background(const Color &background, StyleState state = StyleState::Focused)
{
    Style style;
    style.background = background;
    StylePatch patch;
    patch.state = state;
    patch.Take(StyleProperty::Background, style);
    return patch;
}

ThemedPatch Written(const StylePatch &patch)
{
    return ThemedPatch{.patch = patch, .tokens = {}};
}

/// A look taking @p property from @p token, for @p state.
ThemedPatch Token(std::string_view token, StyleProperty property, StyleState state = StyleState::Focused)
{
    ThemedPatch patch;
    patch.patch.state = state;
    patch.Bind(TokenUse{.token = InternedString{token}, .property = property});
    return patch;
}

/// A token holding one colour per mode, each read into every colour property,
/// as the cook reads a colour.
ThemeToken ColourToken(std::string_view name, std::initializer_list<Color> perMode)
{
    ThemeToken token{.name = InternedString{name}, .values = {}};
    for (const Color &colour : perMode)
    {
        Style style;
        style.background = colour;
        style.borderColor = colour;
        style.textColor = colour;
        StylePatch values;
        values.Take(StyleProperty::Background, style);
        values.Take(StyleProperty::BorderColor, style);
        values.Take(StyleProperty::TextColor, style);
        token.values.push_back(values);
    }
    return token;
}

/// Two modes, dark and light, and an accent in each. A button style with a
/// resting background and gap, a hover look and a disabled look, and a style
/// taking its text colour from the accent.
ThemeDocument Dark()
{
    ThemeDocument theme;
    theme.modes = {InternedString{"dark"}, InternedString{"light"}};
    theme.tokens = {ColourToken("accent", {kDarkAccent, kLightAccent})};

    ThemeStyle button{.name = InternedString{"button"}, .resting = {}, .states = {}};
    Style resting;
    resting.background = kStyleResting;
    resting.gap = Px(kStyleGap);
    button.resting.patch.Take(StyleProperty::Background, resting);
    button.resting.patch.Take(StyleProperty::Gap, resting);
    button.states = {Written(Background(kStyleHover, StyleState::Hover)),
                     Written(Background(kStyleDisabled, StyleState::Disabled))};

    const ThemeStyle accented{
        .name = InternedString{"accented"}, .resting = Token("accent", StyleProperty::TextColor), .states = {}};
    theme.styles = {button, accented};
    return theme;
}

/// Another look for the same names: a contrast mode first, and a dark mode
/// second, so a switch from Dark keeps dark and one from its light mode cannot.
ThemeDocument Contrast()
{
    ThemeDocument theme;
    theme.modes = {InternedString{"contrast"}, InternedString{"dark"}};
    theme.tokens = {ColourToken("accent", {kContrastAccent, kDarkAccent})};
    const ThemeStyle button{
        .name = InternedString{"button"}, .resting = Written(Background(kContrastButton)), .states = {}};
    const ThemeStyle accented{
        .name = InternedString{"accented"}, .resting = Token("accent", StyleProperty::TextColor), .states = {}};
    theme.styles = {button, accented};
    return theme;
}

Themes BothThemes()
{
    Themes themes;
    themes.listed = {ListedTheme{.vpath = std::string{kDarkPath}, .document = Dark()},
                     ListedTheme{.vpath = std::string{kContrastPath}, .document = Contrast()}};
    return themes;
}

/// A node naming the button style, writing its own resting background and a
/// hover look of its own.
NodeLook OwnButton()
{
    NodeLook look;
    look.style.background = kOwnResting;
    look.written = PropertyBit(StyleProperty::Background);
    look.states = {Written(Background(kOwnHover, StyleState::Hover))};
    look.styleName = InternedString{"button"};
    return look;
}

const StylePatch *LookFor(const ResolvedLook &resolved, StyleState state)
{
    for (const StylePatch &look : resolved.states)
    {
        if (look.state == state)
        {
            return &look;
        }
    }
    return nullptr;
}

/// Which states hold, in the order StyleState lists them.
StateSet Holds(bool focused, bool hover, bool pressed, bool disabled)
{
    return StateSet{focused, hover, pressed, disabled};
}

std::vector<std::byte> Cook(const ThemeDocument &theme)
{
    Assisi::Core::BitWriter writer;
    WriteCookedTheme(writer, theme);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

CookedThemeError RefusalOf(const ThemeDocument &theme)
{
    const std::expected<ThemeDocument, CookedThemeError> read = ReadCookedTheme(Cook(theme));
    REQUIRE_FALSE(read.has_value());
    return read.error();
}

UiInput PointerAt(Point pointer)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = InputGrant::Everything;
    return input;
}

/// A node of @p width by @p height under @p parent, which writes its size and
/// nothing else.
ScreenNode Boxed(uint32_t parent, float width, float height)
{
    ScreenNode node;
    node.parent = parent;
    node.style.sizing = {Sizing::Fixed(Px(width)), Sizing::Fixed(Px(height))};
    node.written = PropertyBit(StyleProperty::Width) | PropertyBit(StyleProperty::Height);
    return node;
}

/// A screen file's document: a row whose text turns to the accent under the
/// pointer, holding a label that writes no colour and one that writes its own,
/// and a button naming the button style, whose hover look is the accent.
ScreenDocument Shop()
{
    ScreenDocument document;
    document.traits = {.input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Run};
    ScreenNode root;
    root.style.direction = Direction::Column;
    document.nodes.push_back(root);

    ScreenNode row = Boxed(0, kRowWidth, kRowHeight);
    row.name = InternedString{"row"};
    row.states = {Token("accent", StyleProperty::TextColor, StyleState::Hover)};
    document.nodes.push_back(row);

    ScreenNode label = Boxed(1, kLabelWidth, kRowHeight);
    label.name = InternedString{"label"};
    label.text = document.AddText("Iron sword");
    document.nodes.push_back(label);

    ScreenNode price = Boxed(1, kLabelWidth, kRowHeight);
    price.name = InternedString{"price"};
    price.text = document.AddText("12");
    price.style.textColor = kOwnResting;
    price.written |= PropertyBit(StyleProperty::TextColor);
    document.nodes.push_back(price);

    ScreenNode buy = Boxed(0, kButtonWidth, kRowHeight);
    buy.name = InternedString{"buy"};
    buy.widget = BuiltinWidget::Button;
    buy.text = document.AddText("Buy");
    buy.styleName = InternedString{"button"};
    buy.states = {Token("accent", StyleProperty::Background, StyleState::Hover)};
    document.nodes.push_back(buy);
    return document;
}

/// A UI with both themes, and the shop loaded on it and shown.
struct Stage
{
    Assisi::Core::EventQueue events;
    TestWorld world{events};
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    Themes themes = BothThemes();
    Ui ui{universal};
    /// After the Ui, so it is destroyed first: a screen may not outlive the UI
    /// it registered with.
    std::unique_ptr<Screen> screen;

    Stage()
    {
        ui.SetThemes(&themes);
        std::expected<LoadedScreen, ScreenLoadError> loaded =
            InstantiateScreen(ui, world, "ui/Shop.amdn", Shop(), Assisi::Core::EventCatalog{});
        REQUIRE(loaded.has_value());
        screen = std::move(loaded->screen);
        ui.Show(*screen);
        Step({});
    }

    void Step(const UiInput &input)
    {
        (void)ui.ProcessInput(input);
        ui.Sync(kViewport);
    }

    [[nodiscard]] const Style &Worn(std::string_view name) const
    {
        const Node *node = screen->Tree().Get(screen->Find(name));
        REQUIRE(node != nullptr);
        return node->style;
    }

    [[nodiscard]] Point CentreOf(std::string_view name) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(screen->Find(name));
        REQUIRE(placed != nullptr);
        return {.x = placed->rect.x + (placed->rect.width / 2.f), .y = placed->rect.y + (placed->rect.height / 2.f)};
    }
};

} // namespace

// -----------------------------------------------------------------------------
// Looks as a file writes them
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a look written out replaces a token for the property, and a token a value")
{
    Style written;
    written.background = kOwnResting;

    ThemedPatch look = Token("accent", StyleProperty::Background);
    look.Take(StyleProperty::Background, written);
    CHECK(look.tokens.empty());
    CHECK(look.patch.values.background == kOwnResting);

    look.Bind(TokenUse{.token = InternedString{"accent"}, .property = StyleProperty::Background});
    look.Bind(TokenUse{.token = InternedString{"surface"}, .property = StyleProperty::Background});
    REQUIRE(look.tokens.size() == 1);
    CHECK(look.tokens[0].token.View() == "surface");
    CHECK(look.patch.written == PropertyBit(StyleProperty::Background));
}

TEST_CASE("Themes: a look put over another replaces each property it writes, value or token, and keeps the rest")
{
    ThemedPatch under = Token("accent", StyleProperty::Background);
    under.Bind(TokenUse{.token = InternedString{"ink"}, .property = StyleProperty::TextColor});
    Style gap;
    gap.gap = Px(kStyleGap);
    under.Take(StyleProperty::Gap, gap);

    ThemedPatch over = Written(Background(kOwnResting));
    over.Bind(TokenUse{.token = InternedString{"paper"}, .property = StyleProperty::TextColor});
    under.Add(over);

    CHECK(under.patch.values.background == kOwnResting);
    CHECK(under.patch.values.gap == Px(kStyleGap));
    REQUIRE(under.tokens.size() == 1);
    CHECK(under.tokens[0] == TokenUse{.token = InternedString{"paper"}, .property = StyleProperty::TextColor});
    CHECK(under.patch.written == (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::TextColor) |
                                  PropertyBit(StyleProperty::Gap)));
}

// -----------------------------------------------------------------------------
// The cooked form
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a theme survives the round trip field for field")
{
    const ThemeDocument written = Dark();
    const std::expected<ThemeDocument, CookedThemeError> read = ReadCookedTheme(Cook(written));
    REQUIRE(read.has_value());
    CHECK(*read == written);
}

TEST_CASE("Themes: a cooked theme that disagrees with itself is refused")
{
    ThemeDocument theme = Dark();

    SUBCASE("no mode")
    {
        // A theme is put on in its first mode, and there would be none.
        theme.modes.clear();
        theme.tokens.clear();
    }
    SUBCASE("a token without a value for every mode")
    {
        theme.tokens[0].values.pop_back();
    }
    SUBCASE("two modes with one name")
    {
        theme.modes[1] = theme.modes[0];
    }
    SUBCASE("two tokens with one name")
    {
        theme.tokens.push_back(theme.tokens[0]);
    }
    SUBCASE("two styles with one name")
    {
        theme.styles[1].name = theme.styles[0].name;
    }
    SUBCASE("a style with two looks for one state")
    {
        theme.styles[0].states.push_back(theme.styles[0].states[0]);
    }
    SUBCASE("a token's value writing a property this build does not have")
    {
        theme.tokens[0].values[0].written |= PropertyBit(StyleProperty::Count_);
    }
    SUBCASE("a style writing a property this build does not have")
    {
        theme.styles[0].resting.patch.written |= PropertyBit(StyleProperty::Count_);
    }
    SUBCASE("a token used for a property this build does not have")
    {
        theme.styles[1].resting.tokens[0].property = static_cast<StyleProperty>(200);
    }
    CHECK(RefusalOf(theme) == CookedThemeError::Invalid);
}

TEST_CASE("Themes: a blob of another kind, another layout or cut short is refused")
{
    const std::vector<std::byte> whole = Cook(Dark());

    SUBCASE("another kind")
    {
        Assisi::Core::BitWriter writer;
        Assisi::Core::WriteCookedHeader(writer, Assisi::Core::kScreenKind);
        writer.WriteUInt8(kThemePayloadVersion);
        const std::span<const std::byte> bytes = writer.Data();
        const std::expected<ThemeDocument, CookedThemeError> read =
            ReadCookedTheme(std::vector<std::byte>{bytes.begin(), bytes.end()});
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedThemeError::NotATheme);
    }

    SUBCASE("another layout")
    {
        std::vector<std::byte> bytes = whole;
        Assisi::Core::BitReader reader{bytes};
        REQUIRE(Assisi::Core::ReadCookedHeader(reader).has_value());
        bytes[(reader.BitsRead() / 8) + sizeof(kThemePayloadVersion)] ^= std::byte{1};
        const std::expected<ThemeDocument, CookedThemeError> read = ReadCookedTheme(bytes);
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedThemeError::UnsupportedVersion);
    }

    SUBCASE("cut short anywhere")
    {
        for (std::size_t length = 0; length < whole.size(); ++length)
        {
            REQUIRE_FALSE(ReadCookedTheme(std::span<const std::byte>{whole.data(), length}).has_value());
        }
        CHECK(ReadCookedTheme(whole).has_value());
    }
}

TEST_CASE("Themes: a style, a token, a mode and a listed theme are each found by name")
{
    const ThemeDocument theme = Dark();
    REQUIRE(theme.FindStyle("accented") != nullptr);
    CHECK(theme.FindStyle("accented")->name.View() == "accented");
    CHECK(theme.FindStyle("missing") == nullptr);
    REQUIRE(theme.FindToken("accent") != nullptr);
    CHECK(theme.FindToken("missing") == nullptr);
    CHECK(theme.FindMode("light") == 1u);
    CHECK_FALSE(theme.FindMode("missing").has_value());

    const Themes themes = BothThemes();
    REQUIRE(themes.Find(kContrastPath) != nullptr);
    CHECK(themes.Find(kContrastPath)->document.modes[0].View() == "contrast");
    CHECK(themes.Find("ui/Missing.amth") == nullptr);
}

// -----------------------------------------------------------------------------
// Resolving a look
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a node's own resting look wins over its style's, which fills what the node leaves")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    const ResolvedLook resolved = ResolveLook(OwnButton(), ThemeView{.theme = &dark, .mode = 0});

    CHECK(resolved.resting.values.background == kOwnResting);
    CHECK(resolved.resting.values.gap == Px(kStyleGap));
    CHECK(resolved.resting.written == (PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::Gap)));
}

TEST_CASE("Themes: in each state the style's look goes under the node's, and the states still stack")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    const ResolvedLook resolved = ResolveLook(OwnButton(), ThemeView{.theme = &dark, .mode = 0});

    const StylePatch *hover = LookFor(resolved, StyleState::Hover);
    REQUIRE(hover != nullptr);
    CHECK(hover->values.background == kOwnHover);

    // The node's own hover does not reach past the disabled look its style
    // has: a disabled control looks disabled.
    CHECK(Restyled(resolved.resting, resolved.states, Holds(false, true, false, false)).values.background ==
          kOwnHover);
    CHECK(Restyled(resolved.resting, resolved.states, Holds(false, true, false, true)).values.background ==
          kStyleDisabled);
}

TEST_CASE("Themes: a token takes the value its theme gives it in the mode the theme is in")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    NodeLook look;
    look.tokens = {TokenUse{.token = InternedString{"accent"}, .property = StyleProperty::Background}};
    look.written = PropertyBit(StyleProperty::Background);
    look.states = {Token("accent", StyleProperty::BorderColor, StyleState::Pressed)};

    const ResolvedLook inDark = ResolveLook(look, ThemeView{.theme = &dark, .mode = 0});
    CHECK(inDark.resting.values.background == kDarkAccent);
    REQUIRE(LookFor(inDark, StyleState::Pressed) != nullptr);
    CHECK(LookFor(inDark, StyleState::Pressed)->values.borderColor == kDarkAccent);

    const ResolvedLook inLight = ResolveLook(look, ThemeView{.theme = &dark, .mode = 1});
    CHECK(inLight.resting.values.background == kLightAccent);
    CHECK(LookFor(inLight, StyleState::Pressed)->values.borderColor == kLightAccent);
}

TEST_CASE("Themes: a style's tokens are filled as a node's own are")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    NodeLook look;
    look.styleName = InternedString{"accented"};

    CHECK(ResolveLook(look, ThemeView{.theme = &dark, .mode = 1}).resting.values.textColor == kLightAccent);
}

TEST_CASE("Themes: a style or a token the theme does not have leaves what the node writes itself")
{
    // Only a package cooked against other themes gets here; the cook refuses
    // a screen naming either.
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    NodeLook look;
    look.style.background = kOwnResting;
    look.written = PropertyBit(StyleProperty::Background) | PropertyBit(StyleProperty::BorderColor);
    look.tokens = {TokenUse{.token = InternedString{"missing"}, .property = StyleProperty::BorderColor}};
    look.styleName = InternedString{"missing"};

    const ResolvedLook resolved = ResolveLook(look, ThemeView{.theme = &dark, .mode = 0});
    CHECK(resolved.resting.values == look.style);
    CHECK(resolved.states.empty());
}

TEST_CASE("Themes: with no theme a node wears only its own look")
{
    const ResolvedLook resolved = ResolveLook(OwnButton(), ThemeView{});
    CHECK(resolved.resting.values == OwnButton().style);
    CHECK(resolved.resting.written == PropertyBit(StyleProperty::Background));
    REQUIRE(resolved.states.size() == 1);
    CHECK(resolved.states[0].values.background == kOwnHover);
}

// -----------------------------------------------------------------------------
// A tree under a theme
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a tree resolves a look as it is set, and every node again when its theme changes")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    NodeTree tree;
    tree.SetTheme(ThemeView{.theme = &dark, .mode = 0});
    const NodeId node = tree.Create(tree.Root());
    NodeLook look;
    look.styleName = InternedString{"accented"};
    tree.SetLook(node, look);
    CHECK(tree.Get(node)->style.textColor == kDarkAccent);

    tree.SetTheme(ThemeView{.theme = &dark, .mode = 1});
    CHECK(tree.Get(node)->style.textColor == kLightAccent);
    CHECK(tree.Get(node)->resting.values.textColor == kLightAccent);
}

TEST_CASE("Themes: a style set in code is all the node's own, so a style it names fills nothing at rest")
{
    const ListedTheme dark{.vpath = std::string{kDarkPath}, .document = Dark()};
    NodeTree tree;
    tree.SetTheme(ThemeView{.theme = &dark, .mode = 0});
    const NodeId node = tree.Create(tree.Root());
    tree.SetLook(node, OwnButton());

    Style own;
    own.background = kOwnResting;
    tree.SetStyle(node, own);
    CHECK(tree.Get(node)->resting.values == own);
    CHECK(tree.Get(node)->resting.written == kEveryProperty);
    // Its looks in each state are still its style's under its own.
    CHECK(tree.Get(node)->states.size() == 2);
}

// -----------------------------------------------------------------------------
// The UI's themes
// -----------------------------------------------------------------------------

TEST_CASE("Themes: the UI puts the first theme on in its first mode, on screens made before and after")
{
    Assisi::Core::EventQueue events;
    TestWorld world{events};
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    const Themes themes = BothThemes();
    Ui ui{universal};
    Screen before{ui, world, ScreenTraits{}, kSortHud, "before"};
    ui.SetThemes(&themes);
    Screen after{ui, world, ScreenTraits{}, kSortHud, "after"};

    CHECK(ui.GetTheme() == kDarkPath);
    CHECK(ui.GetThemeMode() == "dark");
    CHECK(before.Tree().GetTheme().theme == &themes.listed[0]);
    CHECK(after.Tree().GetTheme().theme == &themes.listed[0]);
    CHECK(after.Tree().GetTheme().mode == 0);
}

TEST_CASE("Themes: one screen wears two modes of a theme, and another theme, with no change to the screen")
{
    Stage stage;
    CHECK(stage.Worn("buy").background == kStyleResting);
    CHECK(stage.Worn("buy").gap == Px(kStyleGap));

    // States included: the button's hover look is the accent of the mode the
    // theme is in.
    stage.Step(PointerAt(stage.CentreOf("buy")));
    CHECK(stage.Worn("buy").background == kDarkAccent);

    REQUIRE(stage.ui.SetThemeMode("light").has_value());
    stage.Step(PointerAt(stage.CentreOf("buy")));
    CHECK(stage.Worn("buy").background == kLightAccent);

    REQUIRE(stage.ui.SetTheme(kContrastPath).has_value());
    stage.Step({});
    CHECK(stage.Worn("buy").background == kContrastButton);
    stage.Step(PointerAt(stage.CentreOf("buy")));
    CHECK(stage.Worn("buy").background == kContrastAccent);
}

TEST_CASE("Themes: a theme switched keeps the mode by name where it has one, and starts at its first otherwise")
{
    // Dark starts in dark, which Contrast has too, though not first.
    Stage stage;
    REQUIRE(stage.ui.SetTheme(kContrastPath).has_value());
    CHECK(stage.ui.GetThemeMode() == "dark");

    // Contrast has no light.
    REQUIRE(stage.ui.SetTheme(kDarkPath).has_value());
    REQUIRE(stage.ui.SetThemeMode("light").has_value());
    REQUIRE(stage.ui.SetTheme(kContrastPath).has_value());
    CHECK(stage.ui.GetThemeMode() == "contrast");
}

TEST_CASE("Themes: a theme or a mode the UI does not have is refused, and nothing changes")
{
    Stage stage;
    const std::expected<void, ThemeError> theme = stage.ui.SetTheme("ui/Missing.amth");
    REQUIRE_FALSE(theme.has_value());
    CHECK(theme.error() == ThemeError::UnknownTheme);

    const std::expected<void, ThemeError> mode = stage.ui.SetThemeMode("contrast");
    REQUIRE_FALSE(mode.has_value());
    CHECK(mode.error() == ThemeError::UnknownMode);

    CHECK(stage.ui.GetTheme() == kDarkPath);
    CHECK(stage.ui.GetThemeMode() == "dark");
    CHECK(stage.Worn("buy").background == kStyleResting);
}

TEST_CASE("Themes: taking the themes away leaves every node its own look")
{
    Stage stage;
    stage.ui.SetThemes(nullptr);
    stage.Step({});
    CHECK(stage.ui.GetTheme().empty());
    CHECK(stage.ui.GetThemeMode().empty());
    CHECK(stage.Worn("buy").background != kStyleResting);
    CHECK(stage.Worn("buy").gap != Px(kStyleGap));
}

// -----------------------------------------------------------------------------
// Inheritance
// -----------------------------------------------------------------------------

TEST_CASE("Themes: a text colour nothing writes is the parent's, frame by frame")
{
    Stage stage;
    CHECK(stage.Worn("label").textColor == kDefaultText);

    // The row's hover look writes its text colour, which the label inside it
    // takes while the row is hovered and gives back after.
    stage.Step(PointerAt(stage.CentreOf("label")));
    CHECK(stage.Worn("row").textColor == kDarkAccent);
    CHECK(stage.Worn("label").textColor == kDarkAccent);
    // One that writes its own keeps it.
    CHECK(stage.Worn("price").textColor == kOwnResting);

    stage.Step(PointerAt(stage.CentreOf("buy")));
    CHECK(stage.Worn("label").textColor == kDefaultText);
}
