/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTargets.cpp
/// @brief Screens drawn into a target other than the window, as a monitor in
/// the world is: laid out against the target, kept apart from the window's
/// screens, drawn again only when they change, and worked only while used.

#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

using namespace Assisi::Mondrian;
using Assisi::Core::EventQueue;
using Assisi::Mondrian::Testing::FixtureFont;

namespace
{

constexpr Extent kViewport{1280, 720};
constexpr Extent kMonitor{512, 256};
constexpr TextureId kFontTexture{9};

/// How much larger the window's pixels are than the monitor's in the pointer
/// map these cases install.
constexpr float kMapShrink = 2.f;

/// What the host's maker hands back for every target in these cases.
constexpr TargetTexture kMadeTexture{7};

/// The panel on the monitor, in UI pixels.
constexpr float kPanelWidth = 200.f;
constexpr float kPanelHeight = 100.f;

/// A menu taking everything, hiding what is beneath it and stopping the world.
constexpr ScreenTraits kMenuTraits{.input = ScreenInput::ConsumeInput,
                                   .beneath = ScreenBeneath::HidesBeneath,
                                   .pause = ScreenPause::Pause};

UiInput Keys()
{
    UiInput input;
    input.grant = InputGrant::Everything;
    return input;
}

UiInput Press(UiAction action)
{
    UiInput input = Keys();
    input.actionPressed[static_cast<std::size_t>(action)] = true;
    input.actionDown[static_cast<std::size_t>(action)] = true;
    return input;
}

/// A UI with a monitor screen drawn into a target, and a menu on the window
/// that is not shown until a case shows it.
struct Stage
{
    Font font = FixtureFont();
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    /// Before the UI, which releases what is left into it as it goes.
    std::vector<TargetId> released;
    Ui ui{universal};
    TargetId target;
    /// After the Ui, so they are destroyed first.
    std::unique_ptr<Screen> monitor;
    std::unique_ptr<Screen> menu;
    NodeId panel;

    Stage()
    {
        ui.SetFont(&font, kFontTexture);
        ui.SetTargetHost([](TargetId, Extent) { return std::optional<TargetTexture>{kMadeTexture}; },
                         [this](TargetId gone) { released.push_back(gone); });
        target = ui.AddTarget(kMonitor);

        // Traits that would take the keys, hide the window and stop the world
        // if it were on the window, so each case shows a target keeps them off.
        monitor = std::make_unique<Screen>(ui, world, kMenuTraits, kSortMenu, "monitor");
        monitor->SetTarget(target);
        Style style;
        style.sizing = {Sizing::Fixed(Px(kPanelWidth)), Sizing::Fixed(Px(kPanelHeight))};
        style.background = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>{0.f, 0.f, 1.f, 1.f};
        panel = monitor->Add(monitor->Root(), style);
        monitor->Show();

        menu = std::make_unique<Screen>(ui, world, kMenuTraits, kSortMenu, "menu");
        menu->Add(menu->Root(), style);
    }

    void Frame(const UiInput &input = {})
    {
        ui.ProcessInput(input);
        ui.Sync(kViewport);
    }

    [[nodiscard]] bool Redrawn() const { return ui.TargetRedrawn(target); }

    [[nodiscard]] const DrawList &Drawn() const
    {
        const DrawList *list = ui.TargetDrawList(target);
        REQUIRE(list != nullptr);
        return *list;
    }

    /// Maps the window pointer onto the monitor at half its pixels.
    void MapPointer()
    {
        ui.SetTargetPointer(target, [](Point at)
                            { return std::optional<Point>{Point{.x = at.x / kMapShrink, .y = at.y / kMapShrink}}; });
    }
};

/// The window pointer the stage's map puts on the middle of @p box.
Point WindowPointOver(const Rect &box)
{
    return {.x = (box.x + (box.width * 0.5f)) * kMapShrink, .y = (box.y + (box.height * 0.5f)) * kMapShrink};
}

} // namespace

TEST_CASE("Targets: a screen on a target is laid out against it and drawn into it, not the window")
{
    Stage stage;
    stage.Frame();
    const LayoutNode *root = stage.monitor->GetLayout().Get(stage.monitor->Root());
    REQUIRE(root != nullptr);
    CHECK(root->rect.width == static_cast<float>(kMonitor.width));
    CHECK(root->rect.height == static_cast<float>(kMonitor.height));
    CHECK(stage.ui.GetDrawList().Instances().empty());
    CHECK_FALSE(stage.Drawn().Instances().empty());
    CHECK(stage.ui.TargetSize(stage.target) == kMonitor);
}

TEST_CASE("Targets: the host makes what shows a target and releases it when the target goes")
{
    Stage stage;
    CHECK(stage.ui.GetTargetTexture(stage.target) == kMadeTexture);
    stage.ui.RemoveTarget(stage.target);
    CHECK(stage.released == std::vector<TargetId>{stage.target});
    CHECK_FALSE(stage.ui.GetTargetTexture(stage.target).has_value());
    CHECK(stage.ui.TargetDrawList(stage.target) == nullptr);

    SUBCASE("and those left when the UI goes")
    {
        std::vector<TargetId> released;
        EventQueue universal{Assisi::Core::EventScope::Universal};
        std::unique_ptr<Ui> ui = std::make_unique<Ui>(universal);
        ui->SetTargetHost([](TargetId, Extent) { return std::optional<TargetTexture>{}; },
                          [&released](TargetId gone) { released.push_back(gone); });
        const TargetId left = ui->AddTarget(kMonitor);
        CHECK(released.empty());
        ui.reset();
        CHECK(released == std::vector<TargetId>{left});
    }
}

TEST_CASE("Targets: a window screen hiding what is beneath it does not hide a target's")
{
    Stage stage;
    stage.menu->Show();
    stage.Frame();
    CHECK_FALSE(stage.ui.GetDrawList().Instances().empty());
    CHECK(stage.Redrawn());
    CHECK_FALSE(stage.Drawn().Instances().empty());
}

TEST_CASE("Targets: a screen on a target takes no input, pauses nothing and is no part of the back-stack")
{
    Stage stage;
    stage.Frame();
    CHECK(stage.ui.InputScreen() == nullptr);
    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());
    CHECK_FALSE(stage.ui.Back());
    CHECK(stage.monitor->IsShown());
}

TEST_CASE("Targets: a target is drawn again only when what it shows changes")
{
    Stage stage;
    stage.Frame();
    REQUIRE(stage.Redrawn());
    stage.Frame();
    CHECK_FALSE(stage.Redrawn());

    SUBCASE("a node changed")
    {
        Style wider = stage.monitor->Tree().Get(stage.panel)->resting.values;
        wider.sizing[0] = Sizing::Fixed(Px(kPanelWidth * 2.f));
        stage.monitor->Tree().SetStyle(stage.panel, wider);
        stage.Frame();
        CHECK(stage.Redrawn());
        stage.Frame();
        CHECK_FALSE(stage.Redrawn());
    }
    SUBCASE("a node set to what it already was")
    {
        stage.monitor->Tree().SetVisible(stage.panel, true);
        stage.Frame();
        CHECK_FALSE(stage.Redrawn());
    }
    SUBCASE("a node added")
    {
        stage.monitor->Add(stage.monitor->Root(), Style{});
        stage.Frame();
        CHECK(stage.Redrawn());
    }
    SUBCASE("hidden, then shown")
    {
        stage.monitor->Hide();
        stage.Frame();
        CHECK(stage.Redrawn());
        CHECK(stage.Drawn().Instances().empty());
        stage.monitor->Show();
        stage.Frame();
        CHECK(stage.Redrawn());
        CHECK_FALSE(stage.Drawn().Instances().empty());
    }
    SUBCASE("scaled")
    {
        stage.monitor->SetScale(0.5f);
        stage.Frame();
        CHECK(stage.Redrawn());
    }
    SUBCASE("faded")
    {
        stage.monitor->SetOpacity(0.5f);
        stage.Frame();
        CHECK(stage.Redrawn());
    }
    SUBCASE("a second screen shown on it")
    {
        stage.menu->SetTarget(stage.target);
        stage.menu->Show();
        stage.Frame();
        CHECK(stage.Redrawn());
    }
    SUBCASE("the font changed")
    {
        stage.ui.SetFont(&stage.font, kFontTexture);
        stage.Frame();
        CHECK(stage.Redrawn());
    }
    SUBCASE("a settled scrolling list")
    {
        Style smooth;
        smooth.scrollSmoothing = 0.1f;
        stage.monitor->AddScroll(stage.monitor->Root(), smooth, {false, true});
        stage.Frame();
        REQUIRE(stage.Redrawn());
        stage.Frame();
        CHECK_FALSE(stage.Redrawn());
    }
}

TEST_CASE("Targets: a used screen takes the keys, and the pointer through its target's map")
{
    Stage stage;
    stage.MapPointer();
    const ButtonId button = stage.monitor->AddButton(stage.monitor->Root(), "press");
    bool pressed = false;
    stage.monitor->OnActivate(button.node, [&pressed](Screen &) { pressed = true; });
    const TextFieldId field = stage.monitor->AddTextField(stage.monitor->Root(), TextLines::Single);
    stage.Frame();

    stage.ui.Use(*stage.monitor);
    CHECK(stage.ui.InUse() == stage.monitor.get());
    CHECK(stage.ui.InputScreen() == stage.monitor.get());
    CHECK(stage.ui.TakesInput());
    CHECK_FALSE(stage.ui.PausesWorld());

    SUBCASE("typing")
    {
        stage.ui.SetFocus(*stage.monitor, field.node);
        UiInput typing = Keys();
        typing.typed = "hi";
        stage.Frame(typing);
        CHECK(stage.monitor->GetText(field) == "hi");
        CHECK(stage.Redrawn());
    }
    SUBCASE("a click where the map puts it")
    {
        const LayoutNode *placed = stage.monitor->GetLayout().Get(button.node);
        REQUIRE(placed != nullptr);
        UiInput press = Keys();
        press.pointer = WindowPointOver(placed->rect);
        press.primaryPressed = true;
        press.primaryDown = true;
        stage.Frame(press);
        UiInput release = Keys();
        release.pointer = press.pointer;
        release.primaryReleased = true;
        stage.Frame(release);
        CHECK(pressed);
    }
    SUBCASE("the pointer off the surface is over nothing")
    {
        stage.ui.SetTargetPointer(stage.target, [](Point) { return std::optional<Point>{}; });
        const LayoutNode *placed = stage.monitor->GetLayout().Get(button.node);
        REQUIRE(placed != nullptr);
        UiInput over = Keys();
        over.pointer = WindowPointOver(placed->rect);
        stage.Frame(over);
        CHECK_FALSE(stage.ui.GetInteraction().hovered);
    }
    SUBCASE("drawn every frame while used, and once more when put down")
    {
        stage.Frame(Keys());
        CHECK(stage.Redrawn());
        stage.ui.StopUsing();
        stage.Frame();
        CHECK(stage.Redrawn());
        stage.Frame();
        CHECK_FALSE(stage.Redrawn());
    }
}

TEST_CASE("Targets: a window screen taking input outranks the used screen, which has the keys back after it")
{
    Stage stage;
    stage.ui.Use(*stage.monitor);
    stage.menu->Show();
    CHECK(stage.ui.InputScreen() == stage.menu.get());
    stage.menu->Hide();
    CHECK(stage.ui.InputScreen() == stage.monitor.get());
}

TEST_CASE("Targets: Back leaves a field on the used screen first, then puts the screen down")
{
    Stage stage;
    const TextFieldId field = stage.monitor->AddTextField(stage.monitor->Root(), TextLines::Single);
    stage.Frame();
    stage.ui.Use(*stage.monitor);
    stage.ui.SetFocus(*stage.monitor, field.node);
    UiInput typing = Keys();
    typing.typed = "a";
    stage.Frame(typing);
    REQUIRE(stage.ui.GetInteraction().focused == field.node);

    stage.Frame(Press(UiAction::Back));
    CHECK(stage.ui.InUse() == stage.monitor.get());
    CHECK_FALSE(stage.ui.GetInteraction().focused);

    stage.Frame(Press(UiAction::Back));
    CHECK(stage.ui.InUse() == nullptr);
    CHECK(stage.ui.InputScreen() == nullptr);
    // Put down, not turned off.
    CHECK(stage.monitor->IsShown());
}

TEST_CASE("Targets: a screen on the window is not used, and a used screen stops being used when it goes")
{
    Stage stage;
    stage.ui.Use(*stage.menu);
    CHECK(stage.ui.InUse() == nullptr);

    SUBCASE("hidden")
    {
        stage.ui.Use(*stage.monitor);
        stage.monitor->Hide();
        CHECK(stage.ui.InUse() == nullptr);
    }
    SUBCASE("its target taken away")
    {
        stage.ui.Use(*stage.monitor);
        stage.ui.RemoveTarget(stage.target);
        CHECK(stage.ui.InUse() == nullptr);
        CHECK(stage.ui.InputScreen() == nullptr);
    }
    SUBCASE("destroyed")
    {
        stage.ui.Use(*stage.monitor);
        stage.monitor.reset();
        CHECK(stage.ui.InUse() == nullptr);
        CHECK(stage.ui.InputScreen() == nullptr);
    }
    SUBCASE("moved to the window")
    {
        stage.ui.Use(*stage.monitor);
        stage.monitor->SetTarget(kWindowTarget);
        CHECK(stage.ui.InUse() == nullptr);
    }
}
