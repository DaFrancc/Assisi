/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/HitTest.hpp>
#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <doctest/doctest.h>

using namespace Assisi::Mondrian;

namespace
{

constexpr Extent kScreen{1920, 1080};

/// A box of a fixed size.
Style Box(float width, float height)
{
    Style style;
    style.sizing = {Sizing::Fixed(Px(width)), Sizing::Fixed(Px(height))};
    return style;
}

/// A box of a fixed size floating at (@p x, @p y) relative to its parent.
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
        return id;
    }

    NodeId AddFocusable(NodeId parent, const Style &style)
    {
        const NodeId id = Add(parent, style);
        tree.SetFocusable(id, true);
        return id;
    }

    void Run() { ComputeLayout(tree, kScreen, 1.f, nullptr, layout); }

    NodeId At(float x, float y) const { return HitTest(tree, layout, {.x = x, .y = y}); }
};

} // namespace

TEST_CASE("HitTest: the deepest node that stops the pointer is hit, and nothing where none is")
{
    Scene scene;
    const NodeId panel = scene.Add(scene.tree.Root(), FloatingBox(100.f, 100.f, 200.f, 200.f));
    scene.tree.SetBlocksPointer(panel, true);
    const NodeId button = scene.AddFocusable(panel, Box(50.f, 50.f));
    scene.Run();

    CHECK(scene.At(110.f, 110.f) == button);
    CHECK(scene.At(250.f, 250.f) == panel);
    CHECK_FALSE(scene.At(10.f, 10.f));
}

TEST_CASE("HitTest: a node that stops nothing is looked through to what lies beneath")
{
    Scene scene;
    const NodeId button = scene.AddFocusable(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f));
    scene.Add(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f)); // drawn over it
    scene.Run();

    CHECK(scene.At(50.f, 50.f) == button);
}

TEST_CASE("HitTest: of two overlapping nodes, the one drawn later is hit")
{
    Scene scene;
    scene.AddFocusable(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f));
    const NodeId later = scene.AddFocusable(scene.tree.Root(), FloatingBox(50.f, 50.f, 100.f, 100.f));
    scene.Run();

    CHECK(scene.At(75.f, 75.f) == later);
}

TEST_CASE("HitTest: a floating child is hit over its in-flow siblings, even when made first")
{
    Scene scene;
    const NodeId row = scene.Add(scene.tree.Root(), Box(400.f, 100.f));
    const NodeId badge = scene.AddFocusable(row, FloatingBox(0.f, 0.f, 100.f, 100.f));
    scene.AddFocusable(row, Box(100.f, 100.f));
    scene.Run();

    CHECK(scene.At(50.f, 50.f) == badge);
}

TEST_CASE("HitTest: a floating child escapes its scrolling parent's clip unless it clips to it")
{
    Scene scene;
    Style scroller = FloatingBox(0.f, 0.f, 100.f, 100.f);
    scroller.enabledScrollBars = {true, true};
    const NodeId parent = scene.Add(scene.tree.Root(), scroller);
    Style outside = FloatingBox(150.f, 0.f, 50.f, 50.f);
    const NodeId child = scene.AddFocusable(parent, outside);
    scene.Run();
    CHECK(scene.At(160.f, 10.f) == child);

    outside.floating.clipToParent = true;
    scene.tree.SetStyle(child, outside);
    scene.Run();
    CHECK_FALSE(scene.At(160.f, 10.f));
}

TEST_CASE("HitTest: content scrolled out of view is not hit, and is once scrolled in")
{
    Scene scene;
    Style scroller = FloatingBox(0.f, 0.f, 100.f, 100.f);
    scroller.direction = Direction::Column;
    scroller.enabledScrollBars = {false, true};
    const NodeId list = scene.Add(scene.tree.Root(), scroller);
    const NodeId first = scene.AddFocusable(list, Box(100.f, 80.f));
    const NodeId second = scene.AddFocusable(list, Box(100.f, 80.f));
    scene.Run();
    CHECK(scene.At(50.f, 40.f) == first);
    CHECK_FALSE(scene.At(50.f, 150.f)); // the second row's lower part, below the list: clipped away

    scene.tree.SetScrollOffset(list, {.x = 0.f, .y = 60.f});
    scene.Run();
    CHECK(scene.At(50.f, 40.f) == second);
}

TEST_CASE("HitTest: a hidden subtree, and a tree never laid out, hit nothing")
{
    Scene scene;
    const NodeId panel = scene.Add(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f));
    const NodeId button = scene.AddFocusable(panel, Box(50.f, 50.f));
    CHECK_FALSE(scene.At(10.f, 10.f));

    scene.Run();
    CHECK(scene.At(10.f, 10.f) == button);

    scene.tree.SetVisible(panel, false);
    scene.Run();
    CHECK_FALSE(scene.At(10.f, 10.f));
}

TEST_CASE("HitTest: a disabled node still stops the pointer")
{
    Scene scene;
    scene.Add(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f));
    const NodeId button = scene.AddFocusable(scene.tree.Root(), FloatingBox(0.f, 0.f, 100.f, 100.f));
    scene.tree.SetEnabled(button, false);
    scene.Run();

    CHECK(scene.At(50.f, 50.f) == button);
}
