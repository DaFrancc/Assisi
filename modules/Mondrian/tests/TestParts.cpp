/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Mondrian/Widget.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Core/Format.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;
using Assisi::Core::InternedString;

namespace
{

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;

constexpr Extent kScreen{1920, 1080};

/// A range a game would recognise, rather than a bare fraction.
constexpr SliderRange kPercent{.min = 0.f, .max = 100.f, .step = 10.f};

/// Colours no default has, so a part wearing one says where it came from.
constexpr Color kGold{0.9f, 0.7f, 0.1f, 1.f};
constexpr Color kBlue{0.1f, 0.2f, 0.9f, 1.f};
constexpr Color kGreen{0.1f, 0.8f, 0.2f, 1.f};
constexpr Color kRed{0.8f, 0.1f, 0.1f, 1.f};

/// The built-ins' own colours, which their parts wear with no theme.
constexpr Color kTrackColor{0.24f, 0.26f, 0.32f, 1.f};
constexpr Color kFillColor{0.90f, 0.20f, 0.10f, 1.f};

/// A UI showing one screen that takes the keys, with a row to put controls in.
struct Bench
{
    Assisi::Core::EventQueue events;
    TestWorld world{events};
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    /// After the queues, so they outlive it.
    Ui ui{universal};
    /// After the Ui, so it is destroyed first.
    std::unique_ptr<Screen> held;
    Screen *screen = nullptr;
    NodeId row;

    Bench()
    {
        held = std::make_unique<Screen>(ui, world,
                                        ScreenTraits{.input = ScreenInput::ConsumeInput,
                                                     .beneath = ScreenBeneath::HidesBeneath,
                                                     .pause = ScreenPause::Pause},
                                        kSortMenu, "bench");
        screen = held.get();
        ui.Show(*screen);
        row = screen->Tree().Create(screen->Root());
        Step({});
    }

    void Step(const UiInput &input)
    {
        (void)ui.ProcessInput(input);
        ui.Sync(kScreen);
    }

    [[nodiscard]] Rect RectOf(NodeId node) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(node);
        REQUIRE(placed != nullptr);
        return placed->rect;
    }

    /// The part of @p control called @p name, which every test expects to exist.
    [[nodiscard]] NodeId PartOf(NodeId control, std::string_view name) const
    {
        const NodeId part = screen->Tree().Part(control, name);
        REQUIRE(part);
        return part;
    }

    [[nodiscard]] const Style &StyleOf(NodeId node) const { return screen->Tree().Get(node)->style; }
};

UiInput At(Point pointer)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = InputGrant::Everything;
    return input;
}

UiInput Pressing(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryDown = true;
    input.primaryPressed = true;
    return input;
}

Point Middle(const Rect &rect)
{
    return {.x = rect.x + (rect.width / 2.f), .y = rect.y + (rect.height / 2.f)};
}

/// A look for @p state that writes only @p property, as @p from has it.
StylePatch Writing(StyleState state, StyleProperty property, const Style &from)
{
    StylePatch look;
    look.state = state;
    look.Take(property, from);
    return look;
}

/// A look for @p state that writes only the background, @p background.
StylePatch Background(StyleState state, const Color &background)
{
    Style from;
    from.background = background;
    return Writing(state, StyleProperty::Background, from);
}

/// A theme's look writing only the background, @p background.
ThemedPatch ThemedBackground(const Color &background)
{
    Style from;
    from.background = background;
    ThemedPatch look;
    look.Take(StyleProperty::Background, from);
    return look;
}

/// A theme with a style for every slider, whose thumb is gold, and one called
/// "big" a slider may name, whose thumb is blue.
ThemeDocument SliderTheme()
{
    ThemeDocument theme;
    theme.modes = {InternedString{"dark"}};
    ThemeStyle slider{.name = InternedString{"slider"}, .resting = {}, .states = {}, .parts = {}};
    slider.parts.push_back(
        ThemePartStyle{.name = InternedString{"thumb"}, .resting = ThemedBackground(kGold), .states = {}});
    ThemeStyle big{.name = InternedString{"big"}, .resting = {}, .states = {}, .parts = {}};
    big.parts.push_back(
        ThemePartStyle{.name = InternedString{"thumb"}, .resting = ThemedBackground(kBlue), .states = {}});
    theme.styles = {slider, big};
    return theme;
}

UiInput Pressing(UiAction action)
{
    UiInput input;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

/// Every quad @p list holds, one to a line, with everything that decides how
/// it looks.
std::string Written(const DrawList &list)
{
    using Assisi::Core::Format;
    std::string out;
    for (const QuadInstance &quad : list.Instances())
    {
        out += Format("rect {} {} {} {} ", quad.rect.x, quad.rect.y, quad.rect.width, quad.rect.height);
        out += Format("color {} {} {} {} ", quad.color.r, quad.color.g, quad.color.b, quad.color.a);
        out += Format("border {} {} {} {} {} ", quad.borderWidth, quad.borderColor.r, quad.borderColor.g,
                      quad.borderColor.b, quad.borderColor.a);
        out += Format("radius {} {} {} {} styles {} ", quad.cornerRadius[0], quad.cornerRadius[1],
                      quad.cornerRadius[2], quad.cornerRadius[3], quad.cornerStyles);
        out += Format("clip {} {} {} {} kind {}\n", quad.clip.x, quad.clip.y, quad.clip.width, quad.clip.height,
                      quad.kind);
    }
    return out;
}

} // namespace

TEST_CASE("Parts: every built-in control draws its default look as it always has")
{
    // What each control drew before its parts were nodes of their own, quad
    // for quad: the defaults the parts carry reproduce it exactly.
    Bench bench;
    Screen &screen = *bench.screen;
    (void)screen.AddToggle(bench.row, false);
    (void)screen.AddToggle(bench.row, true);
    (void)screen.AddContinuousSlider(bench.row, kPercent, 25.f);
    (void)screen.AddSteppedSlider(bench.row, kPercent, 4, 1);
    Style list;
    list.sizing = {Sizing::Fixed(Px(200.f)), Sizing::Fixed(Px(100.f))};
    list.scrollBarVisibility = ScrollBarVisibility::Always;
    const NodeId scroll = screen.AddScroll(bench.row, list, {true, true});
    Style line;
    line.sizing = {Sizing::Fixed(Px(400.f)), Sizing::Fixed(Px(300.f))};
    screen.Tree().SetStyle(screen.Tree().Create(scroll), line);
    const ButtonId button = screen.AddButton(bench.row, "");
    Style box;
    box.sizing = {Sizing::Fixed(Px(120.f)), Sizing::Fixed(Px(40.f))};
    screen.Tree().SetStyle(button.node, box);
    bench.Step({});
    bench.ui.SetFocus(screen, button.node);
    // A key used, so the focus ring shows.
    bench.Step(Pressing(UiAction::Accept));

    // Two toggles, off and on; a slider a quarter along and a stepped slider on
    // its second of four steps, each track, fill and thumb; a scroll's two bars,
    // their thumbs on whole pixels; and the focused button's ring, over all.
    constexpr std::string_view kDrawn =
        "rect 0 0 56 28 color 0.24 0.26 0.32 1 border 0 0 0 0 0 radius 14 14 14 14 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 0 0 28 28 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 14 14 14 14 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 56 0 56 28 color 0.9 0.2 0.1 1 border 0 0 0 0 0 radius 14 14 14 14 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 84 0 28 28 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 14 14 14 14 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 112 11 220 6 color 0.24 0.26 0.32 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 112 11 59 6 color 0.9 0.2 0.1 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 163 0 16 28 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 8 8 8 8 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 332 11 220 6 color 0.24 0.26 0.32 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 332 11 76 6 color 0.9 0.2 0.1 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 400 0 16 28 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 8 8 8 8 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 552 94 194 6 color 0.24 0.26 0.32 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 552 94 94 6 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 746 0 6 94 color 0.24 0.26 0.32 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 746 0 6 29 color 0.92 0.93 0.96 1 border 0 0 0 0 0 radius 3 3 3 3 styles 85 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n"
        "rect 746 -6 132 52 color 0 0 0 0 border 3 1 1 1 1 radius 0 0 0 0 styles 0 "
        "clip -1e+06 -1e+06 2e+06 2e+06 kind 0\n";
    CHECK(Written(bench.ui.GetDrawList()) == kDrawn);
}

TEST_CASE("Parts: a control's parts are nodes of its own, which it places from what it holds")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ContinuousSliderId volume = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    const ToggleId mute = screen.AddToggle(bench.row, false);
    bench.Step({});

    const NodeId thumb = bench.PartOf(volume.node, "thumb");
    const NodeId fill = bench.PartOf(volume.node, "fill");
    CHECK(screen.Tree().Get(thumb)->parent == volume.node);
    CHECK_FALSE(screen.Tree().Get(thumb)->focusable);
    CHECK_FALSE(screen.Tree().Part(volume.node, "knob"));

    const Rect slider = bench.RectOf(volume.node);
    CHECK(bench.RectOf(thumb).x == slider.x);
    // The fill reaches the thumb's middle.
    CHECK(bench.RectOf(fill).width == doctest::Approx(bench.RectOf(thumb).width / 2.f));

    screen.SetValue(volume, kPercent.max);
    bench.Step({});
    CHECK(bench.RectOf(thumb).x + bench.RectOf(thumb).width == slider.x + slider.width);
    CHECK(bench.RectOf(fill).width == doctest::Approx(slider.width - (bench.RectOf(thumb).width / 2.f)));
    CHECK(screen.GetThumbRect(volume) == bench.RectOf(thumb));

    const NodeId knob = bench.PartOf(mute.node, "knob");
    const Rect toggle = bench.RectOf(mute.node);
    CHECK(bench.RectOf(knob).x == toggle.x);
    screen.SetValue(mute, true);
    bench.Step({});
    CHECK(bench.RectOf(knob).x + bench.RectOf(knob).width == toggle.x + toggle.width);
}

TEST_CASE("Parts: a part's own size is its style's, and the control places it by that size")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ContinuousSliderId volume = screen.AddContinuousSlider(bench.row, kPercent, kPercent.max);
    const NodeId thumb = bench.PartOf(volume.node, "thumb");
    constexpr float kWideThumb = 40.f;
    Style wide;
    wide.sizing[0] = Sizing::Fixed(Px(kWideThumb));
    screen.Tree().SetOwnProperty(thumb, StyleProperty::Width, wide);
    bench.Step({});

    const Rect slider = bench.RectOf(volume.node);
    CHECK(bench.RectOf(thumb).width == doctest::Approx(kWideThumb));
    // Still at the end, however wide it now is.
    CHECK(bench.RectOf(thumb).x + bench.RectOf(thumb).width == slider.x + slider.width);
}

TEST_CASE("Parts: a part wears its control's state, wherever on the control the pointer is")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ContinuousSliderId volume = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    const NodeId thumb = bench.PartOf(volume.node, "thumb");
    screen.Tree().SetStateStyle(thumb, Background(StyleState::Hover, kGold));
    screen.Tree().SetStateStyle(thumb, Background(StyleState::Pressed, kBlue));
    bench.Step({});

    // Over the far end of the track, nowhere near the thumb.
    const Rect slider = bench.RectOf(volume.node);
    const Point farEnd{.x = slider.x + slider.width - 2.f, .y = Middle(slider).y};
    bench.Step(At(farEnd));
    bench.Step(At(farEnd));
    CHECK(bench.StyleOf(thumb).background == kGold);

    bench.Step(Pressing(farEnd));
    bench.Step(Pressing(farEnd));
    CHECK(bench.StyleOf(thumb).background == kBlue);

    UiInput letGo = At(farEnd);
    letGo.primaryReleased = true;
    bench.Step(letGo);
    bench.Step(At({.x = slider.x, .y = slider.y + (3.f * slider.height)}));
    bench.Step(At({.x = slider.x, .y = slider.y + (3.f * slider.height)}));
    CHECK(bench.StyleOf(thumb).background != kGold);
    CHECK(bench.StyleOf(thumb).background != kBlue);
}

TEST_CASE("Parts: a toggle that is on puts itself and its parts in the on state")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ToggleId mute = screen.AddToggle(bench.row, false);
    const NodeId track = bench.PartOf(mute.node, "track");
    const NodeId knob = bench.PartOf(mute.node, "knob");
    screen.Tree().SetStateStyle(knob, Background(StyleState::On, kGreen));
    bench.Step({});
    CHECK(bench.StyleOf(track).background == kTrackColor);
    CHECK(bench.StyleOf(knob).background != kGreen);

    screen.SetValue(mute, true);
    bench.Step({});
    // The track's own default fills it while it is on.
    CHECK(bench.StyleOf(track).background == kFillColor);
    CHECK(bench.StyleOf(knob).background == kGreen);
}

TEST_CASE("Parts: a theme's style named for a kind of control dresses every one, and a part's own look wins")
{
    Themes themes;
    themes.listed.push_back(ListedTheme{.vpath = "ui/Sliders.amth", .document = SliderTheme()});
    Bench bench;
    bench.ui.SetThemes(&themes);
    Screen &screen = *bench.screen;
    const ContinuousSliderId plain = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    const ContinuousSliderId big = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    NodeLook named;
    named.styleName = InternedString{"big"};
    screen.Tree().SetLook(big.node, named);
    const ContinuousSliderId own = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    Style green;
    green.background = kGreen;
    screen.Tree().SetOwnProperty(bench.PartOf(own.node, "thumb"), StyleProperty::Background, green);
    bench.Step({});

    CHECK(bench.StyleOf(bench.PartOf(plain.node, "thumb")).background == kGold);
    CHECK(bench.StyleOf(bench.PartOf(big.node, "thumb")).background == kBlue);
    CHECK(bench.StyleOf(bench.PartOf(own.node, "thumb")).background == kGreen);
    // What the theme leaves alone keeps the built-in look.
    CHECK(bench.StyleOf(bench.PartOf(plain.node, "track")).background == kTrackColor);

    // Taking the themes away leaves the built-in look.
    bench.ui.SetThemes(nullptr);
    bench.Step({});
    CHECK(bench.StyleOf(bench.PartOf(plain.node, "thumb")).background != kGold);
}

TEST_CASE("Parts: what a part holds goes where the part goes, and a press on it is the control's")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ContinuousSliderId volume = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    const NodeId thumb = bench.PartOf(volume.node, "thumb");
    const NodeId label = screen.Tree().Create(thumb);
    Style dot;
    dot.sizing = {Sizing::Fixed(Px(4.f)), Sizing::Fixed(Px(4.f))};
    screen.Tree().SetStyle(label, dot);
    bench.Step({});
    CHECK(bench.RectOf(label).x == bench.RectOf(thumb).x);

    screen.SetValue(volume, kPercent.max / 2.f);
    bench.Step({});
    CHECK(bench.RectOf(label).x == bench.RectOf(thumb).x);
    CHECK(bench.RectOf(label).x > bench.RectOf(volume.node).x);

    bench.Step(Pressing(Middle(bench.RectOf(label))));
    CHECK(bench.ui.GetInteraction().pressed == volume.node);
}

TEST_CASE("Parts: a hidden part draws nothing, and its control still works")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ContinuousSliderId volume = screen.AddContinuousSlider(bench.row, kPercent, 0.f);
    bench.Step({});
    const std::size_t shown = bench.ui.GetDrawList().Instances().size();

    screen.Tree().SetVisible(bench.PartOf(volume.node, "track"), false);
    bench.Step({});
    CHECK(bench.ui.GetDrawList().Instances().size() == shown - 1);

    const Rect slider = bench.RectOf(volume.node);
    bench.Step(Pressing({.x = slider.x + slider.width - 2.f, .y = Middle(slider).y}));
    CHECK(screen.GetValue(volume) > kPercent.max / 2.f);
}

TEST_CASE("Parts: a scroll's two bars are its track and thumb parts, styled as one")
{
    ThemeDocument theme;
    theme.modes = {InternedString{"dark"}};
    ThemeStyle scroll{.name = InternedString{"scroll"}, .resting = {}, .states = {}, .parts = {}};
    scroll.parts.push_back(
        ThemePartStyle{.name = InternedString{"thumb"}, .resting = ThemedBackground(kGold), .states = {}});
    theme.styles = {scroll};
    Themes themes;
    themes.listed.push_back(ListedTheme{.vpath = "ui/Scrolls.amth", .document = theme});

    Bench bench;
    bench.ui.SetThemes(&themes);
    Screen &screen = *bench.screen;
    Style list;
    list.sizing = {Sizing::Fixed(Px(200.f)), Sizing::Fixed(Px(100.f))};
    list.scrollBarVisibility = ScrollBarVisibility::Always;
    const NodeId scroller = screen.AddScroll(bench.row, list, {true, true});
    Style line;
    line.sizing = {Sizing::Fixed(Px(400.f)), Sizing::Fixed(Px(300.f))};
    screen.Tree().SetStyle(screen.Tree().Create(scroller), line);
    bench.Step({});

    uint32_t gold = 0;
    for (const QuadInstance &quad : bench.ui.GetDrawList().Instances())
    {
        gold += quad.color == kGold ? 1U : 0U;
    }
    CHECK(gold == 2);
}

TEST_CASE("Parts: the focus ring is the outline a control's focused look writes, which a look can change")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ButtonId button = screen.AddButton(bench.row, "");
    Style box;
    box.sizing = {Sizing::Fixed(Px(120.f)), Sizing::Fixed(Px(40.f))};
    screen.Tree().SetStyle(button.node, box);
    bench.Step({});
    const std::size_t unfocused = bench.ui.GetDrawList().Instances().size();

    bench.ui.SetFocus(screen, button.node);
    bench.Step(Pressing(UiAction::Accept));
    CHECK(bench.ui.GetDrawList().Instances().size() == unfocused + 1);

    // A wider, red ring further out.
    constexpr float kWidth = 5.f;
    constexpr float kGap = 2.f;
    Style ring;
    ring.outlineWidth = Px(kWidth);
    ring.outlineGap = Px(kGap);
    ring.outlineColor = kRed;
    StylePatch focused = Writing(StyleState::Focused, StyleProperty::OutlineWidth, ring);
    focused.Take(StyleProperty::OutlineGap, ring);
    focused.Take(StyleProperty::OutlineColor, ring);
    screen.Tree().SetStateStyle(button.node, focused);
    bench.Step(Pressing(UiAction::Accept));
    const QuadInstance &drawn = bench.ui.GetDrawList().Instances().back();
    const Rect rect = bench.RectOf(button.node);
    CHECK(drawn.borderColor == kRed);
    CHECK(drawn.borderWidth == doctest::Approx(kWidth));
    CHECK(drawn.rect.x == doctest::Approx(rect.x - kWidth - kGap));
    CHECK(drawn.rect.width == doctest::Approx(rect.width + (2.f * (kWidth + kGap))));

    // None at all.
    ring.outlineWidth = Px(0.f);
    screen.Tree().SetStateStyle(button.node, Writing(StyleState::Focused, StyleProperty::OutlineWidth, ring));
    bench.Step(Pressing(UiAction::Accept));
    CHECK(bench.ui.GetDrawList().Instances().size() == unfocused);
}

TEST_CASE("Parts: a theme's text_field style dresses fields and not text a player may select")
{
    ThemeDocument theme;
    theme.modes = {InternedString{"dark"}};
    theme.styles = {ThemeStyle{.name = InternedString{"text_field"}, .resting = ThemedBackground(kGold), .states = {}}};
    Themes themes;
    themes.listed.push_back(ListedTheme{.vpath = "ui/Fields.amth", .document = theme});

    Bench bench;
    bench.ui.SetThemes(&themes);
    Screen &screen = *bench.screen;
    const TextFieldId field = screen.AddTextField(bench.row, TextLines::Single);
    // A field built in code writes every property of its own at rest, so one
    // made from a look the theme can reach stands in for one from a file.
    screen.Tree().SetLook(field.node, NodeLook{});
    const NodeId label = screen.Tree().Create(bench.row);
    screen.SetSelectable(label, true);
    bench.Step({});

    CHECK(bench.StyleOf(field.node).background == kGold);
    CHECK(bench.StyleOf(label).background != kGold);
}

TEST_CASE("Parts: an outline written at rest draws whatever holds")
{
    Bench bench;
    Screen &screen = *bench.screen;
    const ButtonId button = screen.AddButton(bench.row, "");
    Style box;
    box.sizing = {Sizing::Fixed(Px(120.f)), Sizing::Fixed(Px(40.f))};
    screen.Tree().SetStyle(button.node, box);
    bench.ui.SetFocus(screen, button.node);
    bench.Step(At({.x = 1000.f, .y = 1000.f}));
    const std::size_t pointed = bench.ui.GetDrawList().Instances().size();

    Style ring;
    ring.outlineWidth = Px(2.f);
    ring.outlineColor = kRed;
    const NodeId plain = screen.Tree().Create(bench.row);
    Style outlined = box;
    outlined.outlineWidth = ring.outlineWidth;
    outlined.outlineColor = ring.outlineColor;
    screen.Tree().SetStyle(plain, outlined);
    bench.Step(At({.x = 1000.f, .y = 1000.f}));
    CHECK(bench.ui.GetDrawList().Instances().size() == pointed + 1);
    CHECK(bench.ui.GetDrawList().Instances().back().borderColor == kRed);
}
