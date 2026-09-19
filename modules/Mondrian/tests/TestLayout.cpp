/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TestFontFixture.hpp"

#include <Assisi/Mondrian/Layout.hpp>
#include <Assisi/Mondrian/NodeTree.hpp>

#include <doctest/doctest.h>

#include <string_view>
#include <utility>

using namespace Assisi::Mondrian;
using Assisi::Mondrian::Testing::FixtureFont;
using Assisi::Mondrian::Testing::kFixtureLineHeight;
using Assisi::Mondrian::Testing::kFixtureSize;

namespace
{

constexpr Extent kReference{1920, 1080};

/// A tree, a font and a place to lay them out, at scale one unless asked.
struct Scene
{
    NodeTree tree;
    LayoutResult layout;
    Font font = FixtureFont();

    NodeId Add(NodeId parent, const Style &style)
    {
        const NodeId id = tree.Create(parent);
        tree.SetStyle(id, style);
        return id;
    }

    /// A text node sized to its content on both axes unless @p width says otherwise.
    NodeId AddText(NodeId parent, std::string_view text, Sizing width = Sizing::Fit())
    {
        Style style;
        style.sizing = {width, Sizing::Fit()};
        style.textSize = kFixtureSize;
        const NodeId id = Add(parent, style);
        tree.SetText(id, text);
        return id;
    }

    void Run(Extent viewport = kReference, float scale = 1.f) { ComputeLayout(tree, viewport, scale, &font, layout); }

    Rect RectOf(NodeId id) const
    {
        const LayoutNode *node = layout.Get(id);
        REQUIRE(node != nullptr);
        return node->rect;
    }

    const LayoutNode &NodeOf(NodeId id) const
    {
        const LayoutNode *node = layout.Get(id);
        REQUIRE(node != nullptr);
        return *node;
    }
};

Style Box(Sizing width, Sizing height, Direction direction = Direction::Row)
{
    Style style;
    style.sizing = {width, height};
    style.direction = direction;
    return style;
}

Sizing WithMin(Sizing sizing, float min)
{
    sizing.min = min;
    return sizing;
}

Sizing WithMax(Sizing sizing, float max)
{
    sizing.max = max;
    return sizing;
}

} // namespace

TEST_CASE("Layout: the UI scale fits the reference screen inside the viewport, times the player's setting")
{
    CHECK(UiScale({960, 540}, 1.f) == 0.5f);
    CHECK(UiScale({1920, 540}, 1.f) == 0.5f); // the tighter axis decides
    CHECK(UiScale({1920, 1080}, 2.f) == 2.f);
    CHECK(UiScale({0, 1080}, 1.f) == 0.f);
}

TEST_CASE("Layout: logical lengths are scaled to device pixels")
{
    Scene scene;
    const NodeId box = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(100.f), Sizing::Fixed(40.f)));
    scene.Run({960, 540}, UiScale({960, 540}, 1.f));
    CHECK(scene.RectOf(box).width == 50.f);
    CHECK(scene.RectOf(box).height == 20.f);
}

TEST_CASE("Layout: text wraps at its final width before heights are fitted")
{
    // A single pass that measured text on both axes at once would size the
    // column for one unwrapped line.
    Scene scene;
    const NodeId column = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(200.f), Sizing::Fit(), Direction::Column));
    const NodeId text = scene.AddText(column, "AA AA AA AA AA", Sizing::Grow()); // 232 unwrapped
    scene.Run();

    CHECK(scene.RectOf(text).width == 200.f);
    CHECK(scene.RectOf(text).height == 2.f * kFixtureLineHeight);
    CHECK(scene.RectOf(column).height == 2.f * kFixtureLineHeight);
}

TEST_CASE("Layout: a Percent child counts as nothing while its Fit parent is fitted, then resolves")
{
    Scene scene;
    const NodeId fit = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Sizing::Fixed(10.f)));
    const NodeId fixed = scene.Add(fit, Box(Sizing::Fixed(100.f), Sizing::Fixed(10.f)));
    const NodeId percent = scene.Add(fit, Box(Sizing::Percent(0.5f), Sizing::Fixed(10.f)));
    scene.Run();

    CHECK(scene.RectOf(fit).width == 100.f);
    CHECK(scene.RectOf(fixed).width == 100.f);
    CHECK(scene.RectOf(percent).width == 50.f);
}

TEST_CASE("Layout: a Percent child takes its fraction of a fixed parent's content")
{
    Scene scene;
    Style parentStyle = Box(Sizing::Fixed(220.f), Sizing::Fixed(10.f));
    parentStyle.padding = Padding::All(10.f);
    const NodeId parent = scene.Add(scene.tree.Root(), parentStyle);
    const NodeId percent = scene.Add(parent, Box(Sizing::Percent(0.5f), Sizing::Fixed(10.f)));
    scene.Run();
    CHECK(scene.RectOf(percent).width == 100.f);
}

TEST_CASE("Layout: Grow children share the space left, smallest first, stopping at their max")
{
    SUBCASE("a max caps one child and the rest goes to the other")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(300.f), Sizing::Fixed(10.f)));
        const NodeId small = scene.Add(row, Box(WithMax(Sizing::Grow(), 50.f), Sizing::Fixed(10.f)));
        const NodeId large = scene.Add(row, Box(Sizing::Grow(), Sizing::Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(small).width == 50.f);
        CHECK(scene.RectOf(large).width == 250.f);
    }

    SUBCASE("the smaller grows until it meets the larger, then both grow together")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(300.f), Sizing::Fixed(10.f)));
        const NodeId a = scene.Add(row, Box(WithMin(Sizing::Grow(), 20.f), Sizing::Fixed(10.f)));
        const NodeId b = scene.Add(row, Box(WithMin(Sizing::Grow(), 100.f), Sizing::Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(a).width == 150.f);
        CHECK(scene.RectOf(b).width == 150.f);
    }

    SUBCASE("on the cross axis a Grow child fills its parent")
    {
        Scene scene;
        const NodeId column =
            scene.Add(scene.tree.Root(), Box(Sizing::Fixed(200.f), Sizing::Fixed(50.f), Direction::Column));
        const NodeId child = scene.Add(column, Box(Sizing::Grow(), Sizing::Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(child).width == 200.f);
    }
}

TEST_CASE("Layout: an overflowing row shrinks its widest children first, never below their minimum")
{
    SUBCASE("the wider text wraps to make room")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(100.f), Sizing::Fit()));
        const NodeId wide = scene.AddText(row, "AA AA"); // 88 unwrapped, 40 at its narrowest
        const NodeId thin = scene.AddText(row, "AA");    // 40
        scene.Run();
        CHECK(scene.RectOf(wide).width == 60.f);
        CHECK(scene.RectOf(thin).width == 40.f);
        CHECK(scene.RectOf(wide).height == 2.f * kFixtureLineHeight);
    }

    SUBCASE("what cannot shrink further overflows")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(60.f), Sizing::Fit()));
        const NodeId wide = scene.AddText(row, "AA AA");
        const NodeId thin = scene.AddText(row, "AA");
        scene.Run();
        CHECK(scene.RectOf(wide).width == 40.f);
        CHECK(scene.RectOf(thin).width == 40.f);
    }

    SUBCASE("a row that scrolls lets its children overflow instead")
    {
        Scene scene;
        Style rowStyle = Box(Sizing::Fixed(100.f), Sizing::Fit());
        rowStyle.scroll = {true, false};
        const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
        const NodeId wide = scene.AddText(row, "AA AA");
        scene.AddText(row, "AA");
        scene.Run();
        CHECK(scene.RectOf(wide).width == 88.f);
        CHECK(scene.NodeOf(row).contentSize.x == 128.f);
    }
}

TEST_CASE("Layout: padding and gaps surround and separate children")
{
    Scene scene;
    Style rowStyle = Box(Sizing::Fit(), Sizing::Fit());
    rowStyle.padding = Padding::All(10.f);
    rowStyle.gap = 5.f;
    const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
    const NodeId first = scene.Add(row, Box(Sizing::Fixed(20.f), Sizing::Fixed(20.f)));
    const NodeId second = scene.Add(row, Box(Sizing::Fixed(30.f), Sizing::Fixed(10.f)));
    scene.Run();

    CHECK(scene.RectOf(row).width == 75.f);
    CHECK(scene.RectOf(row).height == 40.f);
    CHECK(scene.RectOf(first).x == 10.f);
    CHECK(scene.RectOf(second).x == 35.f);
    CHECK(scene.RectOf(second).y == 10.f);
}

TEST_CASE("Layout: children align within their parent on each axis")
{
    for (const auto &[align, expectedX] :
         {std::pair{Alignment::Start, 0.f}, std::pair{Alignment::Center, 100.f}, std::pair{Alignment::End, 200.f}})
    {
        Scene scene;
        Style rowStyle = Box(Sizing::Fixed(300.f), Sizing::Fixed(100.f));
        rowStyle.childAlign = {align, Alignment::Center};
        const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
        const NodeId child = scene.Add(row, Box(Sizing::Fixed(100.f), Sizing::Fixed(20.f)));
        scene.Run();
        CHECK(scene.RectOf(child).x == expectedX);
        CHECK(scene.RectOf(child).y == 40.f);
    }
}

TEST_CASE("Layout: edges snap to whole pixels, not sizes, so a row does not drift")
{
    Scene scene;
    const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Sizing::Fixed(10.f)));
    const NodeId a = scene.Add(row, Box(Sizing::Fixed(10.4f), Sizing::Fixed(10.f)));
    const NodeId b = scene.Add(row, Box(Sizing::Fixed(10.4f), Sizing::Fixed(10.f)));
    const NodeId c = scene.Add(row, Box(Sizing::Fixed(10.4f), Sizing::Fixed(10.f)));
    scene.Run();

    // Exact edges 0, 10.4, 20.8, 31.2 round to 0, 10, 21, 31.
    CHECK(scene.RectOf(a).x == 0.f);
    CHECK(scene.RectOf(a).width == 10.f);
    CHECK(scene.RectOf(b).x == 10.f);
    CHECK(scene.RectOf(b).width == 11.f);
    CHECK(scene.RectOf(c).x == 21.f);
    CHECK(scene.RectOf(c).width == 10.f);
}

TEST_CASE("Layout: a floating node takes no space and lands where its anchor says")
{
    Scene scene;
    const NodeId panel = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Sizing::Fixed(100.f)));
    scene.Add(panel, Box(Sizing::Fixed(200.f), Sizing::Fixed(100.f)));

    Style badge = Box(Sizing::Fixed(20.f), Sizing::Fixed(20.f));
    badge.floating.enabled = true;
    badge.floating.anchor = {Alignment::End, Alignment::End};
    badge.floating.attach = {Alignment::End, Alignment::End};
    badge.floating.offset = {.x = -5.f, .y = -5.f};
    badge.floating.clipToParent = true;
    const NodeId clipped = scene.Add(panel, badge);

    Style wide = Box(Sizing::Fixed(500.f), Sizing::Fixed(20.f));
    wide.floating.enabled = true;
    const NodeId unclipped = scene.Add(panel, wide);

    Style centred = Box(Sizing::Fixed(100.f), Sizing::Fixed(100.f));
    centred.floating.enabled = true;
    centred.floating.target = FloatAnchor::Root;
    centred.floating.anchor = {Alignment::Center, Alignment::Center};
    centred.floating.attach = {Alignment::Center, Alignment::Center};
    const NodeId onRoot = scene.Add(panel, centred);
    scene.Run();

    // The 500-wide float does not widen its Fit parent.
    CHECK(scene.RectOf(panel).width == 200.f);

    CHECK(scene.RectOf(clipped).x == 175.f);
    CHECK(scene.RectOf(clipped).y == 75.f);
    const Rect clip = scene.NodeOf(clipped).clip;
    CHECK(clip.x == 0.f);
    CHECK(clip.width == 200.f);
    CHECK(clip.height == 100.f);

    CHECK(scene.NodeOf(unclipped).clip.width == kNoClip.width);

    CHECK(scene.RectOf(onRoot).x == 910.f);
    CHECK(scene.RectOf(onRoot).y == 490.f);
}

TEST_CASE("Layout: a scroll offset moves children, clips them, and stops at the end of the content")
{
    Scene scene;
    Style listStyle = Box(Sizing::Fixed(100.f), Sizing::Fixed(100.f), Direction::Column);
    listStyle.scroll = {false, true};
    const NodeId list = scene.Add(scene.tree.Root(), listStyle);
    const NodeId first = scene.Add(list, Box(Sizing::Fixed(80.f), Sizing::Fixed(80.f)));
    const NodeId second = scene.Add(list, Box(Sizing::Fixed(80.f), Sizing::Fixed(80.f)));

    scene.tree.SetScrollOffset(list, {.x = 0.f, .y = 30.f});
    scene.Run();
    CHECK(scene.RectOf(first).y == -30.f);
    CHECK(scene.RectOf(second).y == 50.f);
    CHECK(scene.NodeOf(list).contentSize.y == 160.f);
    const Rect clip = scene.NodeOf(first).clip;
    CHECK(clip.y == 0.f);
    CHECK(clip.height == 100.f);

    // 160 of content in 100 of view scrolls at most 60.
    scene.tree.SetScrollOffset(list, {.x = 0.f, .y = 500.f});
    scene.Run();
    CHECK(scene.RectOf(second).y == 20.f);
}

TEST_CASE("Layout: a hidden node takes no space and is not placed")
{
    Scene scene;
    const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Sizing::Fixed(10.f)));
    scene.Add(row, Box(Sizing::Fixed(50.f), Sizing::Fixed(10.f)));
    const NodeId hidden = scene.Add(row, Box(Sizing::Fixed(70.f), Sizing::Fixed(10.f)));
    const NodeId inside = scene.Add(hidden, Box(Sizing::Fixed(5.f), Sizing::Fixed(5.f)));
    scene.tree.SetVisible(hidden, false);
    scene.Run();

    CHECK(scene.RectOf(row).width == 50.f);
    CHECK(scene.layout.Get(hidden) == nullptr);
    CHECK(scene.layout.Get(inside) == nullptr);
}

TEST_CASE("Layout: a stale id has no result, even once its slot is reused")
{
    Scene scene;
    const NodeId gone = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(10.f), Sizing::Fixed(10.f)));
    scene.tree.Destroy(gone);
    scene.Add(scene.tree.Root(), Box(Sizing::Fixed(10.f), Sizing::Fixed(10.f)));
    scene.Run();
    CHECK(scene.layout.Get(gone) == nullptr);
}

TEST_CASE("Layout: the root fills the viewport and a new viewport reflows the tree")
{
    Scene scene;
    const NodeId half = scene.Add(scene.tree.Root(), Box(Sizing::Percent(0.5f), Sizing::Grow()));
    scene.Run({1000, 500}, 1.f);
    CHECK(scene.RectOf(scene.tree.Root()).width == 1000.f);
    CHECK(scene.RectOf(half).width == 500.f);
    CHECK(scene.RectOf(half).height == 500.f);

    scene.Run({600, 400}, 1.f);
    CHECK(scene.RectOf(half).width == 300.f);
    CHECK(scene.RectOf(half).height == 400.f);
}
