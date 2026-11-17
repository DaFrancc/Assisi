/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestControlBindings.cpp
/// @brief Controls bound to fields: what they show, what they write and when,
/// on an entity's component and on a registered source.

#include <Assisi/Mondrian/Binding.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/TestReflectedStructs.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>
#include <Assisi/Core/Reflect/StructRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>

ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::Testing::ProbeLength);

using namespace Assisi::Mondrian;
using Assisi::Core::EventQueue;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::StructRegistry;
using Assisi::Core::Reflect::StructSpec;
using Assisi::Core::Reflect::StructTraits;
using Assisi::Mondrian::Testing::ProbeLength;
using Assisi::Mondrian::Testing::ProbeUnit;

namespace
{

constexpr Extent kScreen{1920, 1080};

constexpr EntityId kPlayer{.index = 1, .generation = 1};
constexpr EntityId kOther{.index = 2, .generation = 1};

constexpr SliderRange kPercent{.min = 0.f, .max = 100.f, .step = 10.f};
constexpr SliderRange kFourPositions{.min = 0.f, .max = 3.f, .step = 0.f};
constexpr int32_t kFourSteps = 4;

/// An enum whose values are not its positions, so a test can tell one from the
/// other.
enum class Mood : uint8_t
{
    Calm = 10,
    Angry = 20,
    Tired = 30,
    Count_
};

/// A component of every kind a control holds, and one none does.
struct Dial
{
    double fine = 0.0;
    Assisi::Core::ShortString title;
    float level = 0.f;
    int32_t count = 0;
    uint8_t small = 0;
    Mood mood = Mood::Calm;
    bool on = false;
};

FieldMeta Field(std::string name, FieldType type, std::size_t offset)
{
    FieldMeta field;
    field.name = std::move(name);
    field.type = type;
    field.offset = offset;
    return field;
}

ComponentMeta DialMeta()
{
    FieldMeta mood = Field("mood", FieldType::Enum, offsetof(Dial, mood));
    mood.enumSize = sizeof(Mood);
    mood.enumConstants = {{.name = "Calm", .value = 10}, {.name = "Angry", .value = 20}, {.name = "Tired", .value = 30}};

    return ComponentMeta{.name = "Dial",
                         .fields = {Field("fine", FieldType::Double, offsetof(Dial, fine)),
                                    Field("title", FieldType::String, offsetof(Dial, title)),
                                    Field("level", FieldType::Float, offsetof(Dial, level)),
                                    Field("count", FieldType::Int32, offsetof(Dial, count)),
                                    Field("small", FieldType::UInt8, offsetof(Dial, small)),
                                    mood,
                                    Field("on", FieldType::Bool, offsetof(Dial, on))},
                         .typeIndex = typeid(Dial)};
}

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

UiInput Holding(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryDown = true;
    return input;
}

UiInput Releasing(Point pointer)
{
    UiInput input = At(pointer);
    input.primaryReleased = true;
    return input;
}

UiInput Pressing(UiAction action)
{
    UiInput input;
    input.grant = InputGrant::Everything;
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

Point Middle(const Rect &rect)
{
    return {.x = rect.x + (rect.width / 2.f), .y = rect.y + (rect.height / 2.f)};
}

/// A menu in a world holding two entities, each with a Dial, and a row to put
/// controls in.
struct Menu
{
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    Dial player;
    Dial other;
    std::unique_ptr<Screen> held;
    Screen *screen = nullptr;
    NodeId row;

    Menu()
    {
        world.components = {DialMeta()};
        world.structs = {StructTraits<ProbeLength>::Spec()};
        world.alive = {kPlayer, kOther};
        world.held.push_back({.component = "Dial", .data = &player, .entity = kPlayer});
        world.held.push_back({.component = "Dial", .data = &other, .entity = kOther});
        held = std::make_unique<Screen>(ui, world,
                                        ScreenTraits{.input = ScreenInput::ConsumeInput,
                                                     .beneath = ScreenBeneath::HidesBeneath,
                                                     .pause = ScreenPause::Pause},
                                        kSortMenu, "menu");
        screen = held.get();
        screen->SetContext(kPlayer);
        ui.Show(*screen);

        Style style;
        style.floating.enabled = true;
        row = screen->Tree().Create(screen->Root());
        screen->Tree().SetStyle(row, style);
    }

    void Step(const UiInput &input = {})
    {
        ui.ProcessInput(input);
        ui.Sync(kScreen);
    }

    /// One press of @p action on @p node, focused first.
    void Press(NodeId node, UiAction action)
    {
        ui.SetFocus(*screen, node);
        Step(Pressing(action));
    }

    [[nodiscard]] Rect RectOf(NodeId node) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(node);
        REQUIRE(placed != nullptr);
        return placed->rect;
    }
};

/// A size the pointer can land on: these tests set no font, and a slider's
/// style is otherwise its own to fit.
Style Aimable()
{
    Style style;
    style.sizing = {Sizing::Fixed(Px(200.f)), Sizing::Fixed(Px(40.f))};
    return style;
}

/// Why @p written does not resolve against Dial as @p use.
BindingError Refusal(const ComponentMeta &dial, std::string_view written, BindingUse use)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    REQUIRE(source.has_value());
    const std::expected<FieldBinding, BindingError> binding = ResolveBinding(*source, {.component = &dial}, use);
    REQUIRE_FALSE(binding.has_value());
    return binding.error();
}

} // namespace

TEST_CASE("ControlBindings: {=...} reads as a binding that writes back")
{
    const std::expected<BindingSource, BindingError> plain = ReadSoleBinding("{Dial.level}");
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->twoWay);

    const std::expected<BindingSource, BindingError> both = ReadSoleBinding("{=boss:Dial.level}");
    REQUIRE(both.has_value());
    CHECK(both->twoWay);
    CHECK(both->context == "boss");
    CHECK(both->component == "Dial");
    CHECK(both->field == "level");

    CHECK(ReadSoleBinding("{=}").error() == BindingError::BadShape);
    CHECK(ReadSoleBinding("{= Dial.level}").error() == BindingError::BadShape);
}

TEST_CASE("ControlBindings: each control takes the fields it can hold, and only a control writes")
{
    const ComponentMeta dial = DialMeta();

    CHECK(Refusal(dial, "{Dial.level}", BindingUse::Toggle) == BindingError::WrongControl);
    CHECK(Refusal(dial, "{Dial.on}", BindingUse::Slider) == BindingError::WrongControl);
    CHECK(Refusal(dial, "{Dial.mood}", BindingUse::Slider) == BindingError::WrongControl);
    CHECK(Refusal(dial, "{Dial.title}", BindingUse::Stepped) == BindingError::WrongControl);

    CHECK(Refusal(dial, "{Dial.level:.1f}", BindingUse::Slider) == BindingError::BadFormat);
    CHECK(Refusal(dial, "{=Dial.level:.1f}", BindingUse::Slider) == BindingError::BadFormat);

    CHECK(Refusal(dial, "{=Dial.level}", BindingUse::Text) == BindingError::TwoWayElsewhere);
    CHECK(Refusal(dial, "{=Dial.on}", BindingUse::Bool) == BindingError::TwoWayElsewhere);

    // An enum no name was reflected for has no positions to give a stepped one.
    ComponentMeta nameless = DialMeta();
    std::ranges::find(nameless.fields, "mood", &FieldMeta::name)->enumConstants.clear();
    CHECK(Refusal(nameless, "{Dial.mood}", BindingUse::Stepped) == BindingError::WrongControl);
}

TEST_CASE("ControlBindings: a screen refuses a binding on a node that holds no value")
{
    Menu menu;
    const NodeId label = menu.screen->AddText(menu.row, Style{}, "");
    const ButtonId button = menu.screen->AddButton(menu.row, "ok");
    CHECK(menu.screen->BindControl(label, "{Dial.level}").error() == BindingError::NotAControl);
    CHECK(menu.screen->BindControl(button.node, "{Dial.level}").error() == BindingError::NotAControl);
    CHECK(menu.screen->BindControl(NodeId{}, "{Dial.level}").error() == BindingError::NotAControl);

    const ToggleId toggle = menu.screen->AddToggle(menu.row, false);
    CHECK(menu.screen->BindControl(toggle.node, "{Dial.level}").error() == BindingError::WrongControl);
    CHECK(menu.screen->BindControl(toggle.node, "words").error() == BindingError::NotOneBinding);
}

TEST_CASE("ControlBindings: a bound control shows its field and follows it")
{
    Menu menu;
    const ToggleId toggle = menu.screen->AddToggle(menu.row, false);
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    const SteppedSliderId stepped = menu.screen->AddSteppedSlider(menu.row, kFourPositions, kFourSteps, 0);
    REQUIRE(menu.screen->BindControl(toggle.node, "{=Dial.on}").has_value());
    REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
    REQUIRE(menu.screen->BindControl(stepped.node, "{=Dial.count}").has_value());

    menu.player.level = 40.f;
    menu.player.count = 2;
    menu.player.on = true;
    menu.Step();
    CHECK(std::get<bool>(menu.screen->Tree().Get(toggle.node)->value));
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(40.f));
    CHECK(menu.screen->GetValue(stepped) == 2);

    SUBCASE("something else changes the field")
    {
        menu.player.level = 70.f;
        menu.player.count = 1;
        menu.player.on = false;
        menu.Step();
        CHECK_FALSE(std::get<bool>(menu.screen->Tree().Get(toggle.node)->value));
        CHECK(menu.screen->GetValue(slider) == doctest::Approx(70.f));
        CHECK(menu.screen->GetValue(stepped) == 1);
    }

    SUBCASE("the context moves to another entity")
    {
        menu.other.level = 15.f;
        menu.screen->SetContext(kOther);
        menu.Step();
        CHECK(menu.screen->GetValue(slider) == doctest::Approx(15.f));
    }

    SUBCASE("a field past the control's range is shown at the end and left as it is")
    {
        menu.player.level = 500.f;
        menu.player.count = 90;
        menu.Step();
        CHECK(menu.screen->GetValue(slider) == doctest::Approx(kPercent.max));
        CHECK(menu.screen->GetValue(stepped) == kFourSteps - 1);
        CHECK(menu.player.level == doctest::Approx(500.f));
        CHECK(menu.player.count == 90);
    }

    SUBCASE("showing a field writes nothing")
    {
        menu.world.writes = 0;
        menu.Step();
        CHECK(menu.world.writes == 0);
    }
}

TEST_CASE("ControlBindings: moving a two-way control writes its field")
{
    Menu menu;

    SUBCASE("a toggle")
    {
        const ToggleId toggle = menu.screen->AddToggle(menu.row, false);
        REQUIRE(menu.screen->BindControl(toggle.node, "{=Dial.on}").has_value());
        menu.Step();
        menu.Press(toggle.node, UiAction::Accept);
        CHECK(menu.player.on);
    }

    SUBCASE("a slider")
    {
        const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
        REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
        menu.player.level = 30.f;
        menu.Step();
        menu.Press(slider.node, UiAction::Right);
        CHECK(menu.player.level == doctest::Approx(40.f));
        // What the field holds now is what shows after the sync that read it.
        CHECK(menu.screen->GetValue(slider) == doctest::Approx(40.f));
    }

    SUBCASE("a slider over a double")
    {
        const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
        REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.fine}").has_value());
        menu.Step();
        menu.Press(slider.node, UiAction::Right);
        CHECK(menu.player.fine == doctest::Approx(10.0));
    }

    SUBCASE("a slider over a whole number rounds, and stops at what it can store")
    {
        const ContinuousSliderId slider =
            menu.screen->AddContinuousSlider(menu.row, SliderRange{.min = -50.f, .max = 300.f, .step = 2.5f}, 0.f);
        REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.small}").has_value());
        menu.player.small = 10;
        menu.Step();
        menu.Press(slider.node, UiAction::Right);
        CHECK(menu.player.small == 13); // 12.5 rounds up

        menu.screen->Step(slider.node, 200);
        menu.Step();
        CHECK(menu.player.small == 255);

        menu.screen->Step(slider.node, -400);
        menu.Step();
        CHECK(menu.player.small == 0);
    }

    SUBCASE("a stepped slider over a whole number")
    {
        const SteppedSliderId stepped = menu.screen->AddSteppedSlider(menu.row, kFourPositions, kFourSteps, 0);
        REQUIRE(menu.screen->BindControl(stepped.node, "{=Dial.count}").has_value());
        menu.Step();
        menu.Press(stepped.node, UiAction::Right);
        CHECK(menu.player.count == 1);
    }

    SUBCASE("a move made by a step button")
    {
        const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
        REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
        menu.Step();
        menu.ui.SetFocus(*menu.screen, slider.node);
        menu.screen->Step(slider.node, 3);
        menu.Step();
        CHECK(menu.player.level == doctest::Approx(30.f));
    }
}

TEST_CASE("ControlBindings: a stepped slider over an enum has a step for each enumerator")
{
    Menu menu;
    const SteppedSliderId stepped = menu.screen->AddSteppedSlider(menu.row, SliderRange{}, 1, 0);
    REQUIRE(menu.screen->BindControl(stepped.node, "{=Dial.mood}").has_value());
    CHECK(menu.screen->GetSteps(stepped) == 3);
    CHECK(menu.screen->GetRange(stepped).max == doctest::Approx(2.f));

    menu.player.mood = Mood::Angry;
    menu.Step();
    CHECK(menu.screen->GetValue(stepped) == 1);

    menu.Press(stepped.node, UiAction::Right);
    CHECK(menu.player.mood == Mood::Tired);

    SUBCASE("a value with no enumerator leaves the control where it is, and the field alone")
    {
        menu.player.mood = static_cast<Mood>(99);
        menu.Step();
        CHECK(menu.screen->GetValue(stepped) == 2);
        CHECK(menu.player.mood == static_cast<Mood>(99));
    }
}

TEST_CASE("ControlBindings: a one-way control writes nothing, and its OnChange still hears the move")
{
    Menu menu;
    struct Aimed
    {
        float at = 0.f;
    };
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    REQUIRE(menu.screen->BindControl(slider.node, "{Dial.level}").has_value());
    menu.screen->OnChange(slider, [](float value) { return Aimed{value}; });
    menu.player.level = 30.f;
    menu.Step();

    menu.world.writes = 0;
    menu.Press(slider.node, UiAction::Right);

    CHECK(menu.world.writes == 0);
    CHECK(menu.player.level == doctest::Approx(30.f));
    REQUIRE(menu.events.Read<Aimed>().size() == 1);
    CHECK(menu.events.Read<Aimed>()[0].at == doctest::Approx(40.f));
    // Nobody applied it, so the sync put the control back.
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(30.f));
}

TEST_CASE("ControlBindings: the field is written before anything hears of the move")
{
    Menu menu;
    struct Moved
    {
        float fieldHeld = 0.f;
    };
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
    menu.screen->OnChange(slider, [dial = &menu.player](float) { return Moved{dial->level}; });
    menu.Step();

    menu.Press(slider.node, UiAction::Right);
    REQUIRE(menu.events.Read<Moved>().size() == 1);
    CHECK(menu.events.Read<Moved>()[0].fieldHeld == doctest::Approx(10.f));
}

TEST_CASE("ControlBindings: a drag is never overwritten, and the field shows again on release")
{
    Menu menu;
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    menu.screen->Tree().SetStyle(slider.node, Aimable());
    REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
    menu.Step();

    const Rect track = menu.RectOf(slider.node);
    const Point middle = Middle(track);
    menu.Step(Pressing(middle));
    REQUIRE(menu.ui.IsHeld(*menu.screen, slider.node));
    const float dragged = menu.screen->GetValue(slider);
    CHECK(dragged > 0.f);
    CHECK(menu.player.level == doctest::Approx(dragged));

    // A system clamping the field fights nothing: the player's hand wins.
    menu.player.level = 5.f;
    menu.Step(Holding(middle));
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(dragged));
    CHECK(menu.player.level == doctest::Approx(5.f));

    menu.Step(Releasing(middle));
    menu.Step(At(middle));
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(5.f));
}

TEST_CASE("ControlBindings: with nothing to write to, a move is dropped and the control stays")
{
    Menu menu;
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
    menu.player.level = 30.f;
    menu.Step();

    SUBCASE("the context has no entity")
    {
        menu.screen->SetContext(EntityId{});
    }

    SUBCASE("its entity has been destroyed")
    {
        menu.world.alive = {kOther};
        menu.world.held.clear();
    }

    SUBCASE("its entity has no such component")
    {
        menu.world.held.clear();
    }

    menu.Press(slider.node, UiAction::Right);
    CHECK(menu.player.level == doctest::Approx(30.f));
    // The control keeps what the player made it: there is no field to show.
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(40.f));
}

TEST_CASE("ControlBindings: a held control's entity can go mid-drag")
{
    Menu menu;
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    menu.screen->Tree().SetStyle(slider.node, Aimable());
    REQUIRE(menu.screen->BindControl(slider.node, "{=Dial.level}").has_value());
    menu.Step();

    const Rect track = menu.RectOf(slider.node);
    menu.Step(Pressing(Middle(track)));
    const float before = menu.player.level;

    menu.world.alive = {kOther};
    menu.world.held.clear();
    menu.Step(Holding({.x = track.x + (track.width * 0.9f), .y = Middle(track).y}));
    CHECK(menu.player.level == doctest::Approx(before));
}

TEST_CASE("ControlBindings: a registered source is read and written like a component, with no context")
{
    Menu menu;
    ProbeLength source{.value = 20.f, .unit = ProbeUnit::Share};
    menu.ui.RegisterSource(source);
    menu.screen->SetContext(EntityId{});

    const NodeId text = menu.screen->AddText(menu.row, Style{}, "");
    REQUIRE(menu.screen->BindText(text, "{ProbeLength.value:.0f} {ProbeLength.unit}").has_value());
    const ContinuousSliderId slider = menu.screen->AddContinuousSlider(menu.row, kPercent, 0.f);
    REQUIRE(menu.screen->BindControl(slider.node, "{=ProbeLength.value}").has_value());
    const SteppedSliderId stepped = menu.screen->AddSteppedSlider(menu.row, SliderRange{}, 1, 0);
    REQUIRE(menu.screen->BindControl(stepped.node, "{=ProbeLength.unit}").has_value());

    menu.Step();
    CHECK(menu.screen->Tree().Get(text)->text == "20 Share");
    CHECK(menu.screen->GetValue(slider) == doctest::Approx(20.f));
    CHECK(menu.screen->GetSteps(stepped) == 2);
    CHECK(menu.screen->GetValue(stepped) == 1);

    menu.Press(slider.node, UiAction::Right);
    CHECK(source.value == doctest::Approx(30.f));
    menu.Press(stepped.node, UiAction::Left);
    CHECK(source.unit == ProbeUnit::Pixels);
    CHECK(menu.screen->Tree().Get(text)->text == "30 Pixels");

    SUBCASE("another of the struct replaces it")
    {
        ProbeLength second{.value = 60.f, .unit = ProbeUnit::Pixels};
        menu.ui.RegisterSource(second);
        menu.Step();
        CHECK(menu.screen->GetValue(slider) == doctest::Approx(60.f));

        menu.Press(slider.node, UiAction::Right);
        CHECK(second.value == doctest::Approx(70.f));
        CHECK(source.value == doctest::Approx(30.f));
    }

    SUBCASE("unregistered, its bindings keep what they showed and moves are dropped")
    {
        menu.ui.UnregisterSource<ProbeLength>();
        source.value = 90.f;
        menu.Step();
        CHECK(menu.screen->Tree().Get(text)->text == "30 Pixels");

        menu.Press(slider.node, UiAction::Right);
        CHECK(source.value == doctest::Approx(90.f));
    }
}

TEST_CASE("ControlBindings: a screen made before its source reads it once it is there")
{
    Menu menu;
    const NodeId text = menu.screen->AddText(menu.row, Style{}, "");
    REQUIRE(menu.screen->BindText(text, "{ProbeLength.value:.0f}").has_value());

    menu.Step();
    CHECK(menu.screen->Tree().Get(text)->text.empty());

    ProbeLength source{.value = 12.f, .unit = ProbeUnit::Pixels};
    menu.ui.RegisterSource(source);
    menu.Step();
    CHECK(menu.screen->Tree().Get(text)->text == "12");
}

TEST_CASE("ControlBindings: a source drives visible")
{
    Menu menu;
    const StructSpec spec{.name = "Flags", .fields = {Field("shown", FieldType::Bool, 0)}};
    menu.world.structs.push_back(&spec);
    bool shown = false;
    menu.ui.RegisterSource(spec, &shown);

    const NodeId text = menu.screen->AddText(menu.row, Style{}, "");
    REQUIRE(menu.screen->BindVisible(text, "{Flags.shown}").has_value());

    menu.Step();
    CHECK_FALSE(menu.screen->Tree().Get(text)->visible);
    shown = true;
    menu.Step();
    CHECK(menu.screen->Tree().Get(text)->visible);
}

TEST_CASE("ControlBindings: a source is found by its table, so another struct of its name reads nothing")
{
    Menu menu;
    // What a binding finds under this name is not what is registered.
    const StructSpec lookalike{.name = "Probe", .fields = {Field("value", FieldType::Double, 0)}};
    const StructSpec registered{.name = "Probe", .fields = {Field("value", FieldType::Float, 0)}};
    menu.world.structs.push_back(&lookalike);
    float small = 3.f;
    menu.ui.RegisterSource(registered, &small);

    const NodeId text = menu.screen->AddText(menu.row, Style{}, "");
    REQUIRE(menu.screen->BindText(text, "{Probe.value}").has_value());
    menu.Step();
    CHECK(menu.screen->Tree().Get(text)->text.empty());
}

TEST_CASE("ControlBindings: what a name finds decides whether a binding may use it")
{
    const ComponentMeta dial = DialMeta();
    const StructSpec spec{.name = "Dial", .fields = {Field("level", FieldType::Float, 0)}};

    const std::expected<BindingSource, BindingError> plain = ReadSoleBinding("{Dial.level}");
    const std::expected<BindingSource, BindingError> through = ReadSoleBinding("{boss:Dial.level}");
    REQUIRE(plain.has_value());
    REQUIRE(through.has_value());

    CHECK(ResolveBinding(*plain, {.component = &dial, .structure = &spec}, BindingUse::Text).error() ==
          BindingError::AmbiguousName);
    CHECK(ResolveBinding(*through, {.structure = &spec}, BindingUse::Text).error() == BindingError::SourceContext);
    CHECK(ResolveBinding(*plain, {}, BindingUse::Text).error() == BindingError::UnknownComponent);
    CHECK(ResolveBinding(*plain, {.structure = &spec}, BindingUse::Text).has_value());
}

TEST_CASE("ControlBindings: every ASTRUCT can be found by name")
{
    CHECK(StructRegistry::Instance().Find("ProbeLength") == StructTraits<ProbeLength>::Spec());
    CHECK(StructRegistry::Instance().Find("ScreenNode") != nullptr);
    CHECK(StructRegistry::Instance().Find("NoSuchStruct") == nullptr);
}
