/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;

constexpr Extent kViewport{1280, 720};

/// One background per look, none of them a default, so a node's background
/// says which look it is wearing.
constexpr Color kResting{0.1f, 0.1f, 0.1f, 1.f};
constexpr Color kFocused{0.2f, 0.f, 0.f, 1.f};
constexpr Color kHovered{0.f, 0.3f, 0.f, 1.f};
constexpr Color kPressed{0.f, 0.f, 0.4f, 1.f};
constexpr Color kDisabled{0.5f, 0.5f, 0.f, 1.f};

/// Sizes in UI pixels. These tests set no font, so every node is given a size
/// of its own to have something to point at.
constexpr float kRowWidth = 400.f;
constexpr float kGrownWidth = 600.f;
constexpr float kRowHeight = 80.f;
constexpr float kLabelWidth = 200.f;
constexpr float kControlWidth = 150.f;
constexpr float kControlHeight = 60.f;
constexpr float kGap = 20.f;

/// How far a laid-out edge may sit from where it was asked to: layout snaps
/// edges to whole pixels.
constexpr float kSnapTolerance = 1.f;

/// A screen that takes the keys, and one that is only looked at.
constexpr ScreenTraits kMenu{
    .input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Run};
constexpr ScreenTraits kHud{};

Style Box(float width, float height)
{
    Style style;
    style.sizing = {Sizing::Fixed(Px(width)), Sizing::Fixed(Px(height))};
    style.background = kResting;
    return style;
}

/// A look that changes only the background, to @p background.
StylePatch Wearing(StyleState state, const Color &background)
{
    Style look;
    look.background = background;
    return StylePatch::Changes(state, Style{}, look);
}

/// A style with every field away from its default, so a property that reaches
/// no field, or the wrong one, leaves a difference behind.
Style Distinct()
{
    Style style;
    style.background = Color{0.1f, 0.2f, 0.3f, 0.4f};
    style.borderColor = Color{0.5f, 0.6f, 0.7f, 0.8f};
    style.textColor = Color{0.9f, 0.8f, 0.7f, 0.6f};
    style.sizing = {Sizing::Fixed(Px(10.f)), Sizing::Grow()};
    style.padding = Padding{.left = Px(1.f), .top = Px(2.f), .right = Px(3.f), .bottom = Px(4.f)};
    style.floating = Floating{.offset = {Px(5.f), Px(6.f)},
                              .anchor = {Alignment::End, Alignment::End},
                              .attach = {Alignment::Center, Alignment::Center},
                              .target = FloatAnchor::Root,
                              .enabled = true,
                              .clipToParent = true};
    style.gap = Px(7.f);
    style.textSize = Px(30.f);
    style.borderWidth = Px(2.f);
    style.cornerRadius = Px(8.f);
    style.cornerStyle = CornerStyle::Cut;
    style.direction = Direction::Column;
    style.childAlign = {Alignment::End, Alignment::Center};
    style.textAlign = TextAlign::Right;
    style.scrollSmoothing = 0.3f;
    style.scrollBarMinLength = Px(40.f);
    style.enabledScrollBars = {true, true};
    style.scrollBarVisibility = ScrollBarVisibility::Always;
    style.scrollBarDrag = ScrollBarDrag::Smoothed;
    return style;
}

constexpr std::size_t kPropertyCount = static_cast<std::size_t>(StyleProperty::Count_);

/// Which states hold, in the order StyleState lists them.
StateSet Holds(bool focused, bool hover, bool pressed, bool disabled)
{
    return StateSet{focused, hover, pressed, disabled};
}

UiInput PointerAt(Point pointer)
{
    UiInput input;
    input.pointer = pointer;
    input.grant = InputGrant::Everything;
    return input;
}

UiInput Pressing(Point pointer)
{
    UiInput input = PointerAt(pointer);
    input.primaryDown = true;
    input.primaryPressed = true;
    return input;
}

/// The button still down, the pointer at @p pointer.
UiInput Holding(Point pointer)
{
    UiInput input = PointerAt(pointer);
    input.primaryDown = true;
    return input;
}

UiInput Releasing(Point pointer)
{
    UiInput input = PointerAt(pointer);
    input.primaryReleased = true;
    return input;
}

UiInput Pressed(UiAction action)
{
    UiInput input;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

/// A row holding a label and a button, a second button beside the row, and a
/// field: something to hover that is no control, a control inside it, one
/// outside it, and one that takes the keyboard.
struct Stage
{
    Assisi::Core::EventQueue events;
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    /// After the Ui, so it is destroyed first: a screen may not outlive the UI
    /// it registered with.
    std::unique_ptr<Screen> screen;
    NodeId slot;
    NodeId label;
    NodeId equip;
    NodeId other;
    NodeId field;

    explicit Stage(ScreenTraits traits = kMenu)
    {
        screen = std::make_unique<Screen>(ui, events, traits, kSortMenu, "states");
        NodeTree &tree = screen->Tree();

        Style column;
        column.direction = Direction::Column;
        column.gap = Px(kGap);
        column.padding = Padding::All(Px(kGap));
        tree.SetStyle(screen->Root(), column);

        slot = screen->Add(screen->Root(), Box(kRowWidth, kRowHeight));
        label = screen->AddText(slot, Box(kLabelWidth, kRowHeight), "Iron sword");
        equip = screen->AddButton(slot, "Equip").node;
        tree.SetStyle(equip, Box(kControlWidth, kControlHeight));
        other = screen->AddButton(screen->Root(), "Other").node;
        tree.SetStyle(other, Box(kControlWidth, kControlHeight));
        field = screen->AddTextField(screen->Root(), TextLines::Single).node;
        tree.SetStyle(field, Box(kRowWidth, kControlHeight));

        ui.Show(*screen);
        Step({});
    }

    void Step(const UiInput &input)
    {
        (void)ui.ProcessInput(input);
        ui.Sync(kViewport);
    }

    /// A press and a release on @p node.
    void Click(NodeId node)
    {
        const Point at = CentreOf(node);
        Step(Pressing(at));
        Step(Releasing(at));
    }

    [[nodiscard]] NodeTree &Tree() { return screen->Tree(); }

    [[nodiscard]] const Rect &Placed(NodeId node) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(node);
        REQUIRE(placed != nullptr);
        return placed->rect;
    }

    [[nodiscard]] Point CentreOf(NodeId node) const
    {
        const Rect &rect = Placed(node);
        return {.x = rect.x + (rect.width / 2.f), .y = rect.y + (rect.height / 2.f)};
    }

    /// What @p node wears this frame.
    [[nodiscard]] const Color &Background(NodeId node) const
    {
        const Node *found = screen->Tree().Get(node);
        REQUIRE(found != nullptr);
        return found->style.background;
    }
};

} // namespace

// -----------------------------------------------------------------------------
// Looks
// -----------------------------------------------------------------------------

TEST_CASE("States: a look built from two styles writes what differs between them, and nothing else")
{
    Style look;
    look.background = kHovered;
    look.sizing[0] = Sizing::Fixed(Px(kGrownWidth));

    const StylePatch patch = StylePatch::Changes(StyleState::Hover, Style{}, look);
    CHECK(patch.state == StyleState::Hover);
    for (std::size_t index = 0; index < kPropertyCount; ++index)
    {
        const StyleProperty property = static_cast<StyleProperty>(index);
        CAPTURE(index);
        CHECK(patch.Writes(property) == (property == StyleProperty::Background || property == StyleProperty::Width));
    }
}

TEST_CASE("States: a look puts on only what it writes")
{
    const StylePatch hovered = Wearing(StyleState::Hover, kHovered);
    Style worn = Distinct();
    hovered.ApplyTo(worn);

    Style expected = Distinct();
    expected.background = kHovered;
    CHECK(worn == expected);
}

TEST_CASE("States: each property reaches its own field, and together they reach every field")
{
    const Style distinct = Distinct();
    Style all;
    for (std::size_t index = 0; index < kPropertyCount; ++index)
    {
        const StyleProperty property = static_cast<StyleProperty>(index);
        CAPTURE(index);

        StylePatch one;
        one.Take(property, distinct);
        CHECK(one.Writes(property));

        // Alone, it changes something: a property reaching no field would not.
        Style single;
        one.ApplyTo(single);
        CHECK(single != Style{});

        one.ApplyTo(all);
    }
    CHECK(all == distinct);
    // And a look built across the two writes every one of them.
    const StylePatch every = StylePatch::Changes(StyleState::Pressed, Style{}, distinct);
    for (std::size_t index = 0; index < kPropertyCount; ++index)
    {
        CHECK(every.Writes(static_cast<StyleProperty>(index)));
    }
}

TEST_CASE("States: looks stack focused, hover, pressed, disabled, whatever order they are kept in")
{
    // Kept strongest first, so stacking in the order they are stored would give
    // the opposite of the answer.
    const std::vector<StylePatch> looks{Wearing(StyleState::Disabled, kDisabled), Wearing(StyleState::Pressed, kPressed),
                                        Wearing(StyleState::Hover, kHovered), Wearing(StyleState::Focused, kFocused)};
    Style resting;
    resting.background = kResting;

    CHECK(Restyled(resting, looks, Holds(false, false, false, false)).background == kResting);
    CHECK(Restyled(resting, looks, Holds(true, false, false, false)).background == kFocused);
    CHECK(Restyled(resting, looks, Holds(true, true, false, false)).background == kHovered);
    CHECK(Restyled(resting, looks, Holds(true, true, true, false)).background == kPressed);
    CHECK(Restyled(resting, looks, Holds(true, true, true, true)).background == kDisabled);
    CHECK(Restyled(resting, looks, Holds(false, true, false, true)).background == kDisabled);
}

// -----------------------------------------------------------------------------
// What holds, on a screen
// -----------------------------------------------------------------------------

TEST_CASE("States: the node under the pointer and everything holding it wear their hover look")
{
    Stage stage;
    stage.Tree().SetStateStyle(stage.slot, Wearing(StyleState::Hover, kHovered));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Hover, kHovered));

    // Over the label, which is no control: the row holding it is hovered all
    // the same.
    stage.Step(PointerAt(stage.CentreOf(stage.label)));
    CHECK(stage.Background(stage.slot) == kHovered);
    CHECK(stage.Background(stage.equip) == kResting);

    stage.Step(PointerAt(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.slot) == kHovered);
    CHECK(stage.Background(stage.equip) == kHovered);

    stage.Step(PointerAt(stage.CentreOf(stage.other)));
    CHECK(stage.Background(stage.slot) == kResting);
    CHECK(stage.Background(stage.equip) == kResting);
}

TEST_CASE("States: the control held down and everything holding it wear their pressed look until it is let go")
{
    Stage stage;
    stage.Tree().SetStateStyle(stage.slot, Wearing(StyleState::Pressed, kPressed));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Pressed, kPressed));

    // A press on the label holds no control, so nothing is pressed.
    stage.Step(Pressing(stage.CentreOf(stage.label)));
    CHECK(stage.Background(stage.slot) == kResting);
    stage.Step(Releasing(stage.CentreOf(stage.label)));

    stage.Step(Pressing(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.slot) == kPressed);
    CHECK(stage.Background(stage.equip) == kPressed);

    // Held while the pointer wanders off it, as the press itself is.
    stage.Step(Holding(stage.CentreOf(stage.other)));
    CHECK(stage.Background(stage.equip) == kPressed);

    stage.Step(Releasing(stage.CentreOf(stage.other)));
    CHECK(stage.Background(stage.slot) == kResting);
    CHECK(stage.Background(stage.equip) == kResting);
}

TEST_CASE("States: the focused node wears its focus look while keys were used last, and a field always")
{
    Stage stage;
    for (const NodeId node : {stage.slot, stage.equip, stage.other, stage.field})
    {
        stage.Tree().SetStateStyle(node, Wearing(StyleState::Focused, kFocused));
    }

    stage.Step(Pressed(UiAction::Down));
    REQUIRE(stage.ui.GetInteraction().focused == stage.equip);
    CHECK(stage.Background(stage.equip) == kFocused);
    // Focus is the node's alone, not what holds it.
    CHECK(stage.Background(stage.slot) == kResting);

    // A click moves focus without showing it, as it shows no ring.
    stage.Click(stage.other);
    REQUIRE(stage.ui.GetInteraction().focused == stage.other);
    CHECK(stage.Background(stage.other) == kResting);
    CHECK(stage.Background(stage.equip) == kResting);

    // A field shows it however it came by it: it is being typed into.
    stage.Click(stage.field);
    REQUIRE(stage.ui.GetInteraction().focused == stage.field);
    CHECK(stage.Background(stage.field) == kFocused);
}

TEST_CASE("States: a disabled node wears its disabled look, over whatever else holds")
{
    Stage stage;
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Hover, kHovered));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Disabled, kDisabled));

    stage.Tree().SetEnabled(stage.equip, false);
    stage.Step(PointerAt(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.equip) == kDisabled);

    stage.Tree().SetEnabled(stage.equip, true);
    stage.Step(PointerAt(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.equip) == kHovered);
}

TEST_CASE("States: a press shows over a hover")
{
    Stage stage;
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Hover, kHovered));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Pressed, kPressed));

    stage.Step(PointerAt(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.equip) == kHovered);
    stage.Step(Pressing(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.equip) == kPressed);
}

TEST_CASE("States: a look that changes a size lays the screen out in the frame it takes hold")
{
    Stage stage;
    Style grown = Box(kRowWidth, kRowHeight);
    grown.sizing[0] = Sizing::Fixed(Px(kGrownWidth));
    stage.Tree().SetStateStyle(stage.slot, StylePatch::Changes(StyleState::Hover, Box(kRowWidth, kRowHeight), grown));

    const float scale = stage.screen->GetLayout().scale;
    CHECK(std::abs(stage.Placed(stage.slot).width - (kRowWidth * scale)) <= kSnapTolerance);

    stage.Step(PointerAt(stage.CentreOf(stage.label)));
    CHECK(std::abs(stage.Placed(stage.slot).width - (kGrownWidth * scale)) <= kSnapTolerance);
}

TEST_CASE("States: a screen without the keys wears only its disabled looks")
{
    Stage stage{kHud};
    stage.Tree().SetStateStyle(stage.slot, Wearing(StyleState::Hover, kHovered));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Disabled, kDisabled));

    stage.Step(PointerAt(stage.CentreOf(stage.label)));
    CHECK(stage.Background(stage.slot) == kResting);

    stage.Tree().SetEnabled(stage.equip, false);
    stage.Step(PointerAt(stage.CentreOf(stage.label)));
    CHECK(stage.Background(stage.equip) == kDisabled);
}

TEST_CASE("States: a state's look set again replaces it, and a look writing nothing takes it off")
{
    Stage stage;
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Hover, kHovered));
    stage.Tree().SetStateStyle(stage.equip, Wearing(StyleState::Hover, kPressed));
    const Node *equip = stage.Tree().Get(stage.equip);
    REQUIRE(equip->states.size() == 1);
    CHECK(equip->states[0].values.background == kPressed);

    StylePatch nothing;
    nothing.state = StyleState::Hover;
    stage.Tree().SetStateStyle(stage.equip, nothing);
    CHECK(equip->states.empty());

    // And the node goes back to its resting look once it is restyled.
    stage.Step(PointerAt(stage.CentreOf(stage.equip)));
    CHECK(stage.Background(stage.equip) == kResting);
}
