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
        style.textSize = Px(kFixtureSize);
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

/// A fixed size in UI pixels, which is what most of these cases are about.
Sizing Fixed(float pixels)
{
    return Sizing::Fixed(Px(pixels));
}

Sizing WithMin(Sizing sizing, float min)
{
    sizing.min = Px(min);
    return sizing;
}

Sizing WithMax(Sizing sizing, float max)
{
    sizing.max = Px(max);
    return sizing;
}

} // namespace

TEST_CASE("Layout: a UI pixel is 1/1080 of the viewport's shorter side, times the player's setting")
{
    // 16:9 landscape is unchanged from the reference.
    CHECK(UiScale({1920, 1080}, 1.f, ScaleMatch::ShorterSide) == 1.f);
    CHECK(UiScale({960, 540}, 1.f, ScaleMatch::ShorterSide) == 0.5f);
    CHECK(UiScale({1920, 1080}, 2.f, ScaleMatch::ShorterSide) == 2.f);

    // Wider, square and portrait screens all scale by the side that is shorter,
    // so text is as readable on each as on a landscape monitor of that height.
    CHECK(UiScale({3440, 1440}, 1.f, ScaleMatch::ShorterSide) == doctest::Approx(1440.f / 1080.f));
    CHECK(UiScale({1440, 1440}, 1.f, ScaleMatch::ShorterSide) == doctest::Approx(1440.f / 1080.f));
    CHECK(UiScale({1080, 1920}, 1.f, ScaleMatch::ShorterSide) == 1.f);

    CHECK(UiScale({0, 1080}, 1.f, ScaleMatch::ShorterSide) == 0.f);
}

TEST_CASE("Layout: a game may match the reference's width or height instead")
{
    // Against 1920 across: a portrait screen 1080 across is a little over half.
    CHECK(UiScale({1920, 1080}, 1.f, ScaleMatch::Width) == 1.f);
    CHECK(UiScale({1080, 1920}, 1.f, ScaleMatch::Width) == doctest::Approx(1080.f / 1920.f));

    // Against 1080 down: the same portrait screen is almost twice as tall.
    CHECK(UiScale({1920, 1080}, 1.f, ScaleMatch::Height) == 1.f);
    CHECK(UiScale({1080, 1920}, 1.f, ScaleMatch::Height) == doctest::Approx(1920.f / 1080.f));

    CHECK(UiScale({1920, 1080}, 2.f, ScaleMatch::Width) == 2.f);
}

TEST_CASE("Layout: UI lengths are scaled to device pixels")
{
    Scene scene;
    const NodeId box = scene.Add(scene.tree.Root(), Box(Fixed(100.f), Fixed(40.f)));
    scene.Run({960, 540}, UiScale({960, 540}, 1.f, ScaleMatch::ShorterSide));
    CHECK(scene.RectOf(box).width == 50.f);
    CHECK(scene.RectOf(box).height == 20.f);
}

TEST_CASE("Layout: text wraps at its final width before heights are fitted")
{
    // A single pass that measured text on both axes at once would size the
    // column for one unwrapped line.
    Scene scene;
    const NodeId column = scene.Add(scene.tree.Root(), Box(Fixed(200.f), Sizing::Fit(), Direction::Column));
    const NodeId text = scene.AddText(column, "AA AA AA AA AA", Sizing::Grow()); // 232 unwrapped
    scene.Run();

    CHECK(scene.RectOf(text).width == 200.f);
    CHECK(scene.RectOf(text).height == 2.f * kFixtureLineHeight);
    CHECK(scene.RectOf(column).height == 2.f * kFixtureLineHeight);
}

TEST_CASE("Layout: a % child counts as nothing while its Fit parent is fitted, then resolves")
{
    Scene scene;
    const NodeId fit = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Fixed(10.f)));
    const NodeId fixed = scene.Add(fit, Box(Fixed(100.f), Fixed(10.f)));
    const NodeId percent = scene.Add(fit, Box(Sizing::Fixed(Percent(50.f)), Fixed(10.f)));
    scene.Run();

    CHECK(scene.RectOf(fit).width == 100.f);
    CHECK(scene.RectOf(fixed).width == 100.f);
    CHECK(scene.RectOf(percent).width == 50.f);
}

TEST_CASE("Layout: a % child takes its share of a fixed parent's content")
{
    Scene scene;
    Style parentStyle = Box(Fixed(220.f), Fixed(10.f));
    parentStyle.padding = Padding::All(Px(10.f));
    const NodeId parent = scene.Add(scene.tree.Root(), parentStyle);
    const NodeId percent = scene.Add(parent, Box(Sizing::Fixed(Percent(50.f)), Fixed(10.f)));
    scene.Run();
    CHECK(scene.RectOf(percent).width == 100.f);
}

TEST_CASE("Layout: % padding is a share of the parent's content on the same axis")
{
    for (const float parentWidth : {400.f, 800.f})
    {
        Scene scene;
        const NodeId parent = scene.Add(scene.tree.Root(), Box(Fixed(parentWidth), Fixed(100.f)));
        Style childStyle = Box(Fixed(200.f), Fixed(50.f));
        childStyle.padding.left = Percent(10.f);
        const NodeId child = scene.Add(parent, childStyle);
        scene.Run();
        INFO("parent width: " << parentWidth);
        CHECK(scene.NodeOf(child).padding.left == parentWidth / 10.f);
    }
}

TEST_CASE("Layout: a Fit node with % padding grows by it once its parent is sized")
{
    // Fitted with its padding counting as nothing, then sized again against the
    // parent: 100 of content and 10% of 400 on each side.
    Scene scene;
    const NodeId parent = scene.Add(scene.tree.Root(), Box(Fixed(400.f), Fixed(100.f)));
    Style fitStyle = Box(Sizing::Fit(), Fixed(50.f));
    fitStyle.padding = Padding::All(Percent(10.f));
    const NodeId fit = scene.Add(parent, fitStyle);
    scene.Add(fit, Box(Fixed(100.f), Fixed(10.f)));
    scene.Run();
    CHECK(scene.RectOf(fit).width == 180.f);
}

TEST_CASE("Layout: vw and vh are shares of the viewport, whatever the UI scale")
{
    for (const uint32_t side : {1000u, 2000u})
    {
        Scene scene;
        Style rowStyle = Box(Sizing::Fit(), Sizing::Fit());
        rowStyle.gap = Vw(10.f);
        const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
        const NodeId first = scene.Add(row, Box(Fixed(10.f), Fixed(10.f)));
        const NodeId second = scene.Add(row, Box(Fixed(10.f), Fixed(10.f)));

        Style columnStyle = Box(Sizing::Fit(), Sizing::Fit(), Direction::Column);
        columnStyle.gap = Vh(10.f);
        columnStyle.floating.enabled = true;
        const NodeId column = scene.Add(scene.tree.Root(), columnStyle);
        const NodeId top = scene.Add(column, Box(Fixed(10.f), Fixed(10.f)));
        const NodeId bottom = scene.Add(column, Box(Fixed(10.f), Fixed(10.f)));

        scene.Run({side, side}, 2.f);
        INFO("viewport side: " << side);
        const float tenth = static_cast<float>(side) / 10.f;
        CHECK(scene.RectOf(second).x - (scene.RectOf(first).x + scene.RectOf(first).width) == tenth);
        CHECK(scene.RectOf(bottom).y - (scene.RectOf(top).y + scene.RectOf(top).height) == tenth);
    }
}

TEST_CASE("Layout: em is the node's own text size, and follows the scale with it")
{
    for (const float scale : {1.f, 2.f})
    {
        Scene scene;
        Style style = Box(Fixed(100.f), Fixed(100.f));
        style.textSize = Px(40.f);
        style.padding.left = Em(0.5f);
        const NodeId node = scene.Add(scene.tree.Root(), style);
        scene.Run(kReference, scale);
        INFO("scale: " << scale);
        CHECK(scene.NodeOf(node).padding.left == 20.f * scale);
    }
}

TEST_CASE("Layout: a text size in em is of its parent's, and chains down the tree")
{
    Scene scene;
    Style parentStyle = Box(Fixed(100.f), Fixed(100.f));
    parentStyle.textSize = Px(10.f);
    const NodeId parent = scene.Add(scene.tree.Root(), parentStyle);

    Style childStyle = Box(Fixed(50.f), Fixed(50.f));
    childStyle.textSize = Em(2.f);
    const NodeId child = scene.Add(parent, childStyle);

    Style grandchildStyle = Box(Fixed(10.f), Fixed(10.f));
    grandchildStyle.textSize = Em(1.f);
    grandchildStyle.padding.left = Em(1.f);
    const NodeId grandchild = scene.Add(child, grandchildStyle);
    scene.Run();

    CHECK(scene.NodeOf(child).textSize == 20.f);
    CHECK(scene.NodeOf(grandchild).textSize == 20.f);
    CHECK(scene.NodeOf(grandchild).padding.left == 20.f);
}

TEST_CASE("Layout: each edge of a padding keeps its own unit")
{
    Scene scene;
    const NodeId parent = scene.Add(scene.tree.Root(), Box(Fixed(400.f), Fixed(200.f)));
    Style style = Box(Fixed(100.f), Fixed(100.f));
    style.textSize = Px(10.f);
    style.padding = Padding{.left = Em(1.f), .top = Percent(5.f), .right = Px(3.f), .bottom = Vw(1.f)};
    const NodeId node = scene.Add(parent, style);
    scene.Run({1000, 500}, 1.f);

    const Insets &padding = scene.NodeOf(node).padding;
    CHECK(padding.left == 10.f);   // 1em of 10
    CHECK(padding.top == 10.f);    // 5% of the parent's 200 down
    CHECK(padding.right == 3.f);   // 3 UI pixels at scale one
    CHECK(padding.bottom == 10.f); // 1% of a 1000-wide viewport
}

TEST_CASE("Layout: a floating offset in vw is a share of the viewport")
{
    Scene scene;
    Style badge = Box(Fixed(10.f), Fixed(10.f));
    badge.floating.enabled = true;
    badge.floating.target = FloatAnchor::Root;
    badge.floating.offset = {Vw(2.f), Px(0.f)};
    const NodeId node = scene.Add(scene.tree.Root(), badge);
    scene.Run({1000, 500}, 2.f);
    CHECK(scene.RectOf(node).x == 20.f);
}

TEST_CASE("Layout: a % corner radius is of the node's own shorter side")
{
    Scene scene;
    Style pill = Box(Fixed(100.f), Fixed(40.f));
    pill.cornerRadius = Percent(50.f);
    const NodeId node = scene.Add(scene.tree.Root(), pill);
    scene.Run();
    CHECK(scene.NodeOf(node).cornerRadius == 20.f);
}

TEST_CASE("Layout: Grow children share the space left, smallest first, stopping at their max")
{
    SUBCASE("a max caps one child and the rest goes to the other")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Fixed(300.f), Fixed(10.f)));
        const NodeId small = scene.Add(row, Box(WithMax(Sizing::Grow(), 50.f), Fixed(10.f)));
        const NodeId large = scene.Add(row, Box(Sizing::Grow(), Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(small).width == 50.f);
        CHECK(scene.RectOf(large).width == 250.f);
    }

    SUBCASE("the smaller grows until it meets the larger, then both grow together")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Fixed(300.f), Fixed(10.f)));
        const NodeId a = scene.Add(row, Box(WithMin(Sizing::Grow(), 20.f), Fixed(10.f)));
        const NodeId b = scene.Add(row, Box(WithMin(Sizing::Grow(), 100.f), Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(a).width == 150.f);
        CHECK(scene.RectOf(b).width == 150.f);
    }

    SUBCASE("on the cross axis a Grow child fills its parent")
    {
        Scene scene;
        const NodeId column = scene.Add(scene.tree.Root(), Box(Fixed(200.f), Fixed(50.f), Direction::Column));
        const NodeId child = scene.Add(column, Box(Sizing::Grow(), Fixed(10.f)));
        scene.Run();
        CHECK(scene.RectOf(child).width == 200.f);
    }
}

TEST_CASE("Layout: an overflowing row shrinks its widest children first, never below their minimum")
{
    SUBCASE("the wider text wraps to make room")
    {
        Scene scene;
        const NodeId row = scene.Add(scene.tree.Root(), Box(Fixed(100.f), Sizing::Fit()));
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
        const NodeId row = scene.Add(scene.tree.Root(), Box(Fixed(60.f), Sizing::Fit()));
        const NodeId wide = scene.AddText(row, "AA AA");
        const NodeId thin = scene.AddText(row, "AA");
        scene.Run();
        CHECK(scene.RectOf(wide).width == 40.f);
        CHECK(scene.RectOf(thin).width == 40.f);
    }

    SUBCASE("a row that scrolls lets its children overflow instead")
    {
        Scene scene;
        Style rowStyle = Box(Fixed(100.f), Sizing::Fit());
        rowStyle.enabledScrollBars = {true, false};
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
    rowStyle.padding = Padding::All(Px(10.f));
    rowStyle.gap = Px(5.f);
    const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
    const NodeId first = scene.Add(row, Box(Fixed(20.f), Fixed(20.f)));
    const NodeId second = scene.Add(row, Box(Fixed(30.f), Fixed(10.f)));
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
        Style rowStyle = Box(Fixed(300.f), Fixed(100.f));
        rowStyle.childAlign = {align, Alignment::Center};
        const NodeId row = scene.Add(scene.tree.Root(), rowStyle);
        const NodeId child = scene.Add(row, Box(Fixed(100.f), Fixed(20.f)));
        scene.Run();
        CHECK(scene.RectOf(child).x == expectedX);
        CHECK(scene.RectOf(child).y == 40.f);
    }
}

TEST_CASE("Layout: edges snap to whole pixels, not sizes, so a row does not drift")
{
    Scene scene;
    const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Fixed(10.f)));
    const NodeId a = scene.Add(row, Box(Fixed(10.4f), Fixed(10.f)));
    const NodeId b = scene.Add(row, Box(Fixed(10.4f), Fixed(10.f)));
    const NodeId c = scene.Add(row, Box(Fixed(10.4f), Fixed(10.f)));
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
    const NodeId panel = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Fixed(100.f)));
    scene.Add(panel, Box(Fixed(200.f), Fixed(100.f)));

    Style badge = Box(Fixed(20.f), Fixed(20.f));
    badge.floating.enabled = true;
    badge.floating.anchor = {Alignment::End, Alignment::End};
    badge.floating.attach = {Alignment::End, Alignment::End};
    badge.floating.offset = {Px(-5.f), Px(-5.f)};
    badge.floating.clipToParent = true;
    const NodeId clipped = scene.Add(panel, badge);

    Style wide = Box(Fixed(500.f), Fixed(20.f));
    wide.floating.enabled = true;
    const NodeId unclipped = scene.Add(panel, wide);

    Style centred = Box(Fixed(100.f), Fixed(100.f));
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
    Style listStyle = Box(Fixed(100.f), Fixed(100.f), Direction::Column);
    listStyle.enabledScrollBars = {false, true};
    const NodeId list = scene.Add(scene.tree.Root(), listStyle);
    const NodeId first = scene.Add(list, Box(Fixed(80.f), Fixed(80.f)));
    const NodeId second = scene.Add(list, Box(Fixed(80.f), Fixed(80.f)));

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
    const NodeId row = scene.Add(scene.tree.Root(), Box(Sizing::Fit(), Fixed(10.f)));
    scene.Add(row, Box(Fixed(50.f), Fixed(10.f)));
    const NodeId hidden = scene.Add(row, Box(Fixed(70.f), Fixed(10.f)));
    const NodeId inside = scene.Add(hidden, Box(Fixed(5.f), Fixed(5.f)));
    scene.tree.SetVisible(hidden, false);
    scene.Run();

    CHECK(scene.RectOf(row).width == 50.f);
    CHECK(scene.layout.Get(hidden) == nullptr);
    CHECK(scene.layout.Get(inside) == nullptr);
}

TEST_CASE("Layout: a stale id has no result, even once its slot is reused")
{
    Scene scene;
    const NodeId gone = scene.Add(scene.tree.Root(), Box(Fixed(10.f), Fixed(10.f)));
    scene.tree.Destroy(gone);
    scene.Add(scene.tree.Root(), Box(Fixed(10.f), Fixed(10.f)));
    scene.Run();
    CHECK(scene.layout.Get(gone) == nullptr);
}

TEST_CASE("Layout: the root fills the viewport and a new viewport reflows the tree")
{
    Scene scene;
    const NodeId half = scene.Add(scene.tree.Root(), Box(Sizing::Fixed(Percent(50.f)), Sizing::Grow()));
    scene.Run({1000, 500}, 1.f);
    CHECK(scene.RectOf(scene.tree.Root()).width == 1000.f);
    CHECK(scene.RectOf(half).width == 500.f);
    CHECK(scene.RectOf(half).height == 500.f);

    scene.Run({600, 400}, 1.f);
    CHECK(scene.RectOf(half).width == 300.f);
    CHECK(scene.RectOf(half).height == 400.f);
}

TEST_CASE("Layout: a node sized to fit its own text keeps it on one line wherever it lands")
{
    // A node fits to a measurement, its box is snapped to whole pixels, and its
    // text is then wrapped at whatever is left inside it. Snapping takes up to
    // a pixel depending on where the node sits, and the padding subtracted
    // afterwards is fractional at any scale but one, so a box can hold its own
    // text at one position and not at another. The word here has no space in it
    // to break at, so when it does not fit it breaks mid-word.
    constexpr int32_t kScales = 24;
    constexpr int32_t kPositions = 24;

    for (int32_t s = 1; s <= kScales; ++s)
    {
        const float scale = 0.5f + (static_cast<float>(s) / static_cast<float>(kScales));
        for (int32_t offset = 0; offset < kPositions; ++offset)
        {
            Scene scene;

            Style outer;
            outer.padding.left = Px(static_cast<float>(offset) / 4.f);
            const NodeId parent = scene.Add(scene.tree.Root(), outer);

            // A button's shape: a label with room around it.
            Style inner;
            inner.sizing = {Sizing::Fit(), Sizing::Fit()};
            inner.textSize = Px(kFixtureSize);
            inner.padding = Padding{.left = Px(28.f), .top = Px(10.f), .right = Px(28.f), .bottom = Px(10.f)};
            const NodeId text = scene.Add(parent, inner);
            scene.tree.SetText(text, "AAA");

            scene.Run(kReference, scale);

            const LayoutNode &placed = scene.NodeOf(text);
            REQUIRE(placed.text != LayoutNode::kNoText);
            INFO("scale: " << scale << "  left padding: " << outer.padding.left.value);
            CHECK(scene.layout.texts[placed.text].lines.size() == 1);
        }
    }
}
