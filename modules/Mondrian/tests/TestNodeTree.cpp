/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/NodeTree.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>

using namespace Assisi::Mondrian;

TEST_CASE("NodeTree: children link in the order they are created")
{
    NodeTree tree;
    const NodeId a = tree.Create(tree.Root());
    const NodeId b = tree.Create(tree.Root());
    const NodeId c = tree.Create(a);

    REQUIRE(tree.Get(tree.Root()) != nullptr);
    CHECK(tree.Get(tree.Root())->firstChild == a);
    CHECK(tree.Get(a)->nextSibling == b);
    CHECK_FALSE(tree.Get(b)->nextSibling);
    CHECK(tree.Get(a)->firstChild == c);
    CHECK(tree.Get(c)->parent == a);
}

TEST_CASE("NodeTree: a destroyed node's id, and its descendants', no longer reach anything")
{
    NodeTree tree;
    const NodeId panel = tree.Create(tree.Root());
    const NodeId label = tree.Create(panel);
    const NodeId kept = tree.Create(tree.Root());
    tree.SetText(kept, "kept");

    tree.Destroy(panel);
    CHECK_FALSE(tree.IsAlive(panel));
    CHECK_FALSE(tree.IsAlive(label));
    CHECK(tree.Get(tree.Root())->firstChild == kept);

    // Calls through stale ids change nothing and do not fail.
    tree.SetText(label, "stale");
    tree.SetVisible(panel, false);
    CHECK_FALSE(tree.Create(label));
    tree.Destroy(label);
    CHECK(tree.Get(kept)->text == "kept");

    // A new node may take a freed slot; the old ids still miss it.
    const NodeId reused = tree.Create(tree.Root());
    CHECK(tree.IsAlive(reused));
    CHECK_FALSE(tree.IsAlive(panel));
    CHECK_FALSE(tree.IsAlive(label));
    tree.SetText(panel, "still stale");
    tree.SetText(label, "still stale");
    CHECK(tree.Get(reused)->text.empty());
}

TEST_CASE("NodeTree: a slot freed and taken again gets a new generation")
{
    NodeTree tree;
    const NodeId first = tree.Create(tree.Root());
    tree.Destroy(first);
    const NodeId second = tree.Create(tree.Root());
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
}

TEST_CASE("NodeTree: a node is found by the name it was created with, until it is destroyed")
{
    NodeTree tree;
    const std::expected<NodeId, NameError> title = tree.Create(tree.Root(), "title");
    REQUIRE(title.has_value());
    CHECK(tree.Find("title") == *title);
    CHECK_FALSE(tree.Find("missing"));
    tree.Destroy(*title);
    CHECK_FALSE(tree.Find("title"));
}

TEST_CASE("NodeTree: a name means one node")
{
    NodeTree tree;
    const std::expected<NodeId, NameError> first = tree.Create(tree.Root(), "volume");
    REQUIRE(first.has_value());

    SUBCASE("a second node made with it is refused, and nothing is made")
    {
        const std::size_t slots = tree.Slots().size();
        const std::expected<NodeId, NameError> second = tree.Create(tree.Root(), "volume");
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error() == NameError::Taken);
        CHECK(tree.Slots().size() == slots);
        CHECK(tree.Get(tree.Root())->firstChild == *first);
        CHECK_FALSE(tree.Get(*first)->nextSibling);
        CHECK(tree.Find("volume") == *first);
    }

    SUBCASE("a node renamed to it is refused, and keeps its own")
    {
        const std::expected<NodeId, NameError> other = tree.Create(tree.Root(), "music");
        REQUIRE(other.has_value());
        const std::expected<void, NameError> renamed = tree.SetName(*other, "volume");
        REQUIRE_FALSE(renamed.has_value());
        CHECK(renamed.error() == NameError::Taken);
        CHECK(tree.Get(*other)->name == "music");
        CHECK(tree.Find("volume") == *first);
    }

    SUBCASE("a node given the name it already has keeps it")
    {
        CHECK(tree.SetName(*first, "volume").has_value());
        CHECK(tree.Find("volume") == *first);
    }

    SUBCASE("a destroyed node gives its name up")
    {
        tree.Destroy(*first);
        const std::expected<NodeId, NameError> again = tree.Create(tree.Root(), "volume");
        REQUIRE(again.has_value());
        CHECK(tree.Find("volume") == *again);
    }

    SUBCASE("a node renamed away gives its name up")
    {
        REQUIRE(tree.SetName(*first, "").has_value());
        CHECK(tree.Create(tree.Root(), "volume").has_value());
    }
}

TEST_CASE("NodeTree: any number of nodes may go unnamed")
{
    // A name is how something refers to a node; one nothing refers to needs
    // none, and unnamed nodes never collide with each other.
    NodeTree tree;
    const NodeId one = tree.Create(tree.Root());
    const NodeId two = tree.Create(tree.Root());
    CHECK(tree.IsAlive(one));
    CHECK(tree.IsAlive(two));
    CHECK(tree.Create(tree.Root(), "").has_value());
    CHECK(tree.SetName(one, "").has_value());
    CHECK(tree.SetName(two, "").has_value());
}

TEST_CASE("NodeTree: setters reach the node their id names")
{
    NodeTree tree;
    const NodeId node = tree.Create(tree.Root());
    Style style;
    style.gap = Px(7.f);
    tree.SetStyle(node, style);
    tree.SetText(node, "caf\xC3\xA9");
    tree.SetVisible(node, false);
    tree.SetBehaviour(node, 3);
    tree.SetImage(node, TextureId{4}, Rect{.x = 0.f, .y = 0.f, .width = 0.5f, .height = 0.5f});

    const Node *read = tree.Get(node);
    REQUIRE(read != nullptr);
    CHECK(read->style.gap.value == 7.f);
    CHECK(read->text == "caf\xC3\xA9");
    CHECK_FALSE(read->visible);
    CHECK(read->behaviour == 3);
    CHECK(read->hasImage);
    CHECK(read->image == TextureId{4});

    tree.ClearImage(node);
    CHECK_FALSE(tree.Get(node)->hasImage);
}

#ifndef NDEBUG
TEST_CASE("NodeTree: the root cannot be destroyed")
{
    const Assisi::Testing::ThrowOnContractViolation guard;
    NodeTree tree;
    CHECK_THROWS_AS(tree.Destroy(tree.Root()), Assisi::Core::ContractViolation);
}
#endif
