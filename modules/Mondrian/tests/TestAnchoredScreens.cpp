/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnchoredScreens.cpp
/// @brief A screen placed at a point rather than across the window: sized by
/// its own content, put where its attach point lands on the point, slid back on
/// screen when asked, and scaled and faded as a whole.

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>

using namespace Assisi::Mondrian;
using Assisi::Core::EventQueue;

namespace
{

constexpr Extent kViewport{1280, 720};

/// How far a placed edge may sit from where the arithmetic puts it, in device
/// pixels: layout snaps every edge to a whole one.
constexpr float kSnap = 1.f;

/// The bar's size, in UI pixels.
constexpr float kBarWidth = 120.f;
constexpr float kBarHeight = 30.f;

/// A screen holding one red bar, shown, in a UI drawn at kViewport.
struct Stage
{
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    Screen screen{ui, world, ScreenTraits{}, kSortHud, "nameplate"};
    NodeId bar;

    Stage()
    {
        Style style;
        style.sizing = {Sizing::Fixed(Px(kBarWidth)), Sizing::Fixed(Px(kBarHeight))};
        style.background = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>{1.f, 0.f, 0.f, 0.8f};
        bar = screen.Add(screen.Root(), style);
        screen.Show();
    }

    const DrawList &Frame()
    {
        ui.ProcessInput({});
        ui.Sync(kViewport);
        return ui.GetDrawList();
    }

    [[nodiscard]] Rect Root() const { return screen.GetLayout().Get(screen.Root())->rect; }
    [[nodiscard]] Rect Bar() const { return screen.GetLayout().Get(bar)->rect; }
};

bool Near(float a, float b)
{
    return std::abs(a - b) <= kSnap;
}

} // namespace

TEST_CASE("AnchoredScreens: a screen with no anchor is laid across the whole window")
{
    Stage stage;
    stage.Frame();
    CHECK(stage.Root().width == static_cast<float>(kViewport.width));
    CHECK(stage.Root().height == static_cast<float>(kViewport.height));
}

TEST_CASE("AnchoredScreens: an anchored screen fits its content, its attach point on the anchor")
{
    constexpr Point kAt{.x = 400.f, .y = 300.f};
    Stage stage;

    SUBCASE("bottom middle, as a nameplate")
    {
        stage.screen.SetAttach({Alignment::Center, Alignment::End});
        stage.screen.SetAnchor(kAt, false);
        stage.Frame();
        const Rect root = stage.Root();
        CHECK(Near(root.width, stage.Bar().width));
        CHECK(Near(root.height, stage.Bar().height));
        CHECK(Near(root.x + (root.width * 0.5f), kAt.x));
        CHECK(Near(root.y + root.height, kAt.y));
        // What is inside moves with it.
        CHECK(Near(stage.Bar().x, root.x));
    }

    SUBCASE("top left")
    {
        stage.screen.SetAttach({Alignment::Start, Alignment::Start});
        stage.screen.SetAnchor(kAt, false);
        stage.Frame();
        CHECK(Near(stage.Root().x, kAt.x));
        CHECK(Near(stage.Root().y, kAt.y));
    }

    SUBCASE("cleared, across the window again")
    {
        stage.screen.SetAnchor(kAt, false);
        stage.Frame();
        stage.screen.ClearAnchor();
        stage.Frame();
        CHECK(stage.Root().width == static_cast<float>(kViewport.width));
        CHECK(stage.Root().x == 0.f);
    }
}

TEST_CASE("AnchoredScreens: a screen kept on screen is slid back inside the window, and one not kept is not")
{
    constexpr Point kPastCorner{.x = 1500.f, .y = 900.f};
    Stage stage;
    stage.screen.SetAnchor(kPastCorner, true);
    stage.Frame();
    Rect root = stage.Root();
    CHECK(Near(root.x + root.width, static_cast<float>(kViewport.width)));
    CHECK(Near(root.y + root.height, static_cast<float>(kViewport.height)));

    stage.screen.SetAnchor(Point{.x = -200.f, .y = -200.f}, true);
    stage.Frame();
    root = stage.Root();
    CHECK(Near(root.x, 0.f));
    CHECK(Near(root.y, 0.f));

    stage.screen.SetAnchor(kPastCorner, false);
    stage.Frame();
    CHECK(stage.Root().x + stage.Root().width > static_cast<float>(kViewport.width));
}

TEST_CASE("AnchoredScreens: a screen's scale and opacity apply to all of it")
{
    Stage stage;
    stage.screen.SetAnchor(Point{.x = 400.f, .y = 300.f}, false);
    const DrawList &whole = stage.Frame();
    const float wholeWidth = stage.Bar().width;
    REQUIRE_FALSE(whole.Instances().empty());
    const float wholeAlpha = whole.Instances().front().color.a;

    stage.screen.SetScale(0.5f);
    stage.screen.SetOpacity(0.5f);
    const DrawList &half = stage.Frame();
    CHECK(Near(stage.Bar().width, wholeWidth * 0.5f));
    REQUIRE_FALSE(half.Instances().empty());
    CHECK(half.Instances().front().color.a == doctest::Approx(wholeAlpha * 0.5f));

    // Shrunk to nothing, it draws nothing.
    stage.screen.SetScale(0.f);
    CHECK(stage.Frame().Instances().empty());
}
