/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/Navigation.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <doctest/doctest.h>

using namespace Assisi::Mondrian;

namespace
{

constexpr Extent kScreen{1920, 1080};

Style Box(float width, float height)
{
    Style style;
    style.sizing = {Sizing::Fixed(Px(width)), Sizing::Fixed(Px(height))};
    return style;
}

Style FloatingBox(float x, float y, float width, float height)
{
    Style style = Box(width, height);
    style.floating.enabled = true;
    style.floating.offset = {Px(x), Px(y)};
    return style;
}

struct Scene
{
    NodeTree tree;
    LayoutResult layout;

    NodeId Add(NodeId parent, const Style &style)
    {
        const NodeId id = tree.Create(parent);
        tree.SetStyle(id, style);
        tree.SetFocusable(id, true);
        return id;
    }

    /// A focusable box at (@p x, @p y) on the screen.
    NodeId At(float x, float y) { return Add(tree.Root(), FloatingBox(x, y, 100.f, 50.f)); }

    void Run() { ComputeLayout(tree, kScreen, 1.f, nullptr, layout); }

    NodeId Move(NodeId from, NavDirection direction, NavWrap wrap = NavWrap::Stop) const
    {
        return Neighbour(tree, layout, from, direction, wrap);
    }
};

} // namespace

TEST_CASE("Navigation: moving goes to the nearest node ahead, and nowhere when none is")
{
    Scene scene;
    const NodeId top = scene.At(0.f, 0.f);
    const NodeId middle = scene.At(0.f, 100.f);
    const NodeId bottom = scene.At(0.f, 200.f);
    scene.Run();

    CHECK(scene.Move(top, NavDirection::Down) == middle);
    CHECK(scene.Move(middle, NavDirection::Down) == bottom);
    CHECK(scene.Move(bottom, NavDirection::Up) == middle);
    CHECK_FALSE(scene.Move(top, NavDirection::Up));
    CHECK_FALSE(scene.Move(top, NavDirection::Right));
}

TEST_CASE("Navigation: a node straight ahead beats a nearer one off to the side")
{
    Scene scene;
    const NodeId from = scene.At(0.f, 0.f);
    const NodeId below = scene.At(0.f, 200.f);
    scene.At(250.f, 80.f); // 30 below, but 150 to the side
    scene.Run();

    CHECK(scene.Move(from, NavDirection::Down) == below);
}

TEST_CASE("Navigation: past the last node, wrapping comes round to the far side's nearest")
{
    Scene scene;
    const NodeId left = scene.At(0.f, 0.f);
    scene.At(200.f, 0.f);
    const NodeId right = scene.At(400.f, 0.f);
    scene.At(0.f, 300.f); // on the far side, but well off the row
    scene.Run();

    CHECK_FALSE(scene.Move(right, NavDirection::Right, NavWrap::Stop));
    CHECK(scene.Move(right, NavDirection::Right, NavWrap::Around) == left);
    CHECK(scene.Move(left, NavDirection::Left, NavWrap::Around) == right);
}

TEST_CASE("Navigation: an override is taken wherever it points, unless it cannot take focus")
{
    Scene scene;
    const NodeId top = scene.At(0.f, 0.f);
    const NodeId bottom = scene.At(0.f, 100.f);
    const NodeId aside = scene.At(500.f, 0.f);
    scene.tree.SetNavOverride(bottom, NavDirection::Down, aside);
    scene.tree.SetNavOverride(top, NavDirection::Down, bottom);
    scene.Run();

    CHECK(scene.Move(bottom, NavDirection::Down) == aside);

    scene.tree.SetEnabled(aside, false);
    CHECK_FALSE(scene.Move(bottom, NavDirection::Down));
    CHECK(scene.Move(top, NavDirection::Down) == bottom);
}

TEST_CASE("Navigation: disabled and hidden nodes are passed over")
{
    Scene scene;
    const NodeId first = scene.At(0.f, 0.f);
    const NodeId disabled = scene.At(0.f, 100.f);
    const NodeId hidden = scene.At(0.f, 200.f);
    const NodeId last = scene.At(0.f, 300.f);
    scene.tree.SetEnabled(disabled, false);
    scene.tree.SetVisible(hidden, false);
    scene.Run();

    CHECK(scene.Move(first, NavDirection::Down) == last);
    CHECK_FALSE(CanFocus(scene.tree, scene.layout, disabled));
    CHECK_FALSE(CanFocus(scene.tree, scene.layout, hidden));
    CHECK(CanFocus(scene.tree, scene.layout, first));
}

TEST_CASE("Navigation: Tab order is tree order, wrapping at both ends, and starts at the first")
{
    Scene scene;
    const NodeId container = scene.tree.Create(scene.tree.Root());
    const NodeId first = scene.Add(container, Box(10.f, 10.f));
    const NodeId skipped = scene.Add(container, Box(10.f, 10.f));
    const NodeId second = scene.Add(container, Box(10.f, 10.f));
    scene.tree.SetEnabled(skipped, false);
    scene.Run();

    CHECK(FirstFocusable(scene.tree, scene.layout) == first);
    CHECK(NextFocusable(scene.tree, scene.layout, {}, TabOrder::Forward) == first);
    CHECK(NextFocusable(scene.tree, scene.layout, first, TabOrder::Forward) == second);
    CHECK(NextFocusable(scene.tree, scene.layout, second, TabOrder::Forward) == first);
    CHECK(NextFocusable(scene.tree, scene.layout, first, TabOrder::Backward) == second);
}

TEST_CASE("Navigation: scrolling into view moves each scrolling ancestor just far enough")
{
    Scene scene;
    Style scroller = FloatingBox(0.f, 0.f, 100.f, 100.f);
    scroller.direction = Direction::Column;
    scroller.enabledScrollBars = {false, true};
    const NodeId list = scene.tree.Create(scene.tree.Root());
    scene.tree.SetStyle(list, scroller);
    const NodeId first = scene.Add(list, Box(100.f, 80.f));
    scene.Add(list, Box(100.f, 80.f));
    const NodeId third = scene.Add(list, Box(100.f, 80.f));
    scene.Run();

    ScrollIntoView(scene.tree, scene.layout, third);
    CHECK(scene.tree.Get(list)->scrollOffset.y == doctest::Approx(140.f)); // its bottom, 240, meets the list's, 100

    scene.Run();
    ScrollIntoView(scene.tree, scene.layout, first);
    CHECK(scene.tree.Get(list)->scrollOffset.y == doctest::Approx(0.f));
}
