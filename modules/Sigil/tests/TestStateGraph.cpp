/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStateGraph.cpp
/// @brief A running machine: the path from the root to the playing state, the
/// outer block's transitions checked before the inner ones, the first true
/// one in written order firing, only a firing transition using up its
/// triggers, and a path carried across a changed graph by its names.

#include "BytecodeTesting.hpp"

#include <Assisi/Sigil/Graph.hpp>

#include <doctest/doctest.h>

using namespace Assisi::Sigil;
using namespace Assisi::Sigil::Testing;

namespace
{

/// A condition that is the bool in @p slot, appended to @p code.
uint32_t SlotCondition(std::vector<Word> &code, uint32_t slot)
{
    const uint32_t entry = static_cast<uint32_t>(code.size());
    code.push_back(MakeWord(Opcode::Load, slot));
    code.push_back(MakeWord(Opcode::Return));
    return entry;
}

/// The slots the conditions below read.
enum TestSlot : uint32_t
{
    OuterGo,
    InnerGo,
    Hit,
    Spare,
    Count_,
};

/// root { idle, combat { ready, swing }, dead }, with
///   transition 0 (root):   combat -> dead  when OuterGo
///   transition 1 (root):   idle -> combat  when Hit, reading the trigger Hit
///   transition 2 (combat): ready -> swing  when InnerGo
///   transition 3 (combat): ready -> swing  when Hit, reading the trigger Hit
/// Nodes, breadth first: 0 root, 1 idle, 2 combat, 3 dead, 4 ready, 5 swing.
struct Combat
{
    Graph graph;
    std::vector<Word> code;
};

Combat MakeCombat()
{
    Combat combat;
    std::vector<Word> &code = combat.code;
    combat.graph.nodes = {
        Node{.name = "", .firstChild = 1, .childCount = 3, .firstTransition = 0, .transitionCount = 2},
        Node{.name = "idle", .firstChild = 0, .childCount = 0, .firstTransition = 2, .transitionCount = 0},
        Node{.name = "combat", .firstChild = 4, .childCount = 2, .firstTransition = 2, .transitionCount = 2},
        Node{.name = "dead", .firstChild = 0, .childCount = 0, .firstTransition = 4, .transitionCount = 0},
        Node{.name = "ready", .firstChild = 0, .childCount = 0, .firstTransition = 4, .transitionCount = 0},
        Node{.name = "swing", .firstChild = 0, .childCount = 0, .firstTransition = 4, .transitionCount = 0},
    };
    combat.graph.transitions = {
        GraphTransition{.sources = {1}, .triggersRead = {}, .condition = SlotCondition(code, OuterGo), .target = 2},
        GraphTransition{.sources = {0}, .triggersRead = {Hit}, .condition = SlotCondition(code, Hit), .target = 1},
        GraphTransition{.sources = {0}, .triggersRead = {}, .condition = SlotCondition(code, InnerGo), .target = 1},
        GraphTransition{.sources = {0}, .triggersRead = {Hit}, .condition = SlotCondition(code, Hit), .target = 1},
    };
    return combat;
}

std::vector<Word> EmptyBlock()
{
    return std::vector<Word>(TestSlot::Count_, FromBool(false));
}

} // namespace

TEST_CASE("State graph: entering a state goes down to its first state that holds none")
{
    const Combat combat = MakeCombat();
    std::vector<uint32_t> path{0};
    Descend(combat.graph, path);
    CHECK(path == std::vector<uint32_t>{0, 1});

    Enter(combat.graph, 0, 1, path);
    CHECK(path == std::vector<uint32_t>{0, 2, 4});
    CHECK(PathNames(combat.graph, path) == std::vector<std::string>{"combat", "ready"});
}

TEST_CASE("State graph: the outer block's transitions are checked before the inner ones")
{
    const Combat combat = MakeCombat();
    std::vector<Word> block = EmptyBlock();
    block[OuterGo] = FromBool(true);
    block[InnerGo] = FromBool(true);
    const std::vector<uint32_t> path{0, 2, 4};
    const std::optional<Fired> fired = StepGraph(combat.graph, path, combat.code, block, {});
    REQUIRE(fired.has_value());
    CHECK(fired->transition == 0);
    CHECK(fired->depth == 0);

    block[OuterGo] = FromBool(false);
    const std::optional<Fired> inner = StepGraph(combat.graph, path, combat.code, block, {});
    REQUIRE(inner.has_value());
    CHECK(inner->transition == 2);
    CHECK(inner->depth == 1);
}

TEST_CASE("State graph: of two true transitions the first written fires, using up only its own triggers")
{
    const Combat combat = MakeCombat();
    std::vector<Word> block = EmptyBlock();
    block[InnerGo] = FromBool(true);
    block[Hit] = FromBool(true);
    const std::vector<uint32_t> path{0, 2, 4};
    const std::optional<Fired> fired = StepGraph(combat.graph, path, combat.code, block, {});
    REQUIRE(fired.has_value());
    CHECK(fired->transition == 2);
    // Transition 3 also reads Hit, but didn't fire.
    CHECK(ToBool(block[Hit]));

    block[InnerGo] = FromBool(false);
    const std::optional<Fired> second = StepGraph(combat.graph, path, combat.code, block, {});
    REQUIRE(second.has_value());
    CHECK(second->transition == 3);
    CHECK_FALSE(ToBool(block[Hit]));
}

TEST_CASE("State graph: a transition only leaves its sources, and a closed one is passed over untouched")
{
    const Combat combat = MakeCombat();
    std::vector<Word> block = EmptyBlock();
    block[Hit] = FromBool(true);
    // From dead nothing leaves: transition 1 leaves idle only.
    CHECK_FALSE(StepGraph(combat.graph, std::vector<uint32_t>{0, 3}, combat.code, block, {}).has_value());
    CHECK(ToBool(block[Hit]));

    std::vector<Gate> gates(combat.graph.transitions.size(), Gate::Open);
    gates[1] = Gate::Closed;
    CHECK_FALSE(StepGraph(combat.graph, std::vector<uint32_t>{0, 1}, combat.code, block, gates).has_value());
    CHECK(ToBool(block[Hit]));
}

TEST_CASE("State graph: a path is carried to a changed graph by its names")
{
    const Combat combat = MakeCombat();
    std::vector<uint32_t> path;
    Remap(combat.graph, std::vector<std::string>{"combat", "swing"}, path);
    CHECK(path == std::vector<uint32_t>{0, 2, 5});

    // A state that is gone falls back to the first state of the deepest one left.
    Remap(combat.graph, std::vector<std::string>{"combat", "parry"}, path);
    CHECK(path == std::vector<uint32_t>{0, 2, 4});
    Remap(combat.graph, std::vector<std::string>{"flee"}, path);
    CHECK(path == std::vector<uint32_t>{0, 1});
}

TEST_CASE("State graph: a damaged graph is refused before it runs")
{
    const Combat combat = MakeCombat();
    CHECK(VerifyGraph(combat.graph, combat.code, TestSlot::Count_).has_value());

    Graph target = combat.graph;
    target.transitions[2].target = 2;
    CHECK_FALSE(VerifyGraph(target, combat.code, TestSlot::Count_).has_value());

    Graph children = combat.graph;
    children.nodes[2].childCount = 5;
    CHECK_FALSE(VerifyGraph(children, combat.code, TestSlot::Count_).has_value());

    // A child before its parent could lead a descent round in a circle.
    Graph circle = combat.graph;
    circle.nodes[4].firstChild = 2;
    circle.nodes[4].childCount = 1;
    CHECK_FALSE(VerifyGraph(circle, combat.code, TestSlot::Count_).has_value());

    Graph trigger = combat.graph;
    trigger.transitions[1].triggersRead = {TestSlot::Count_};
    CHECK_FALSE(VerifyGraph(trigger, combat.code, TestSlot::Count_).has_value());

    CHECK_FALSE(VerifyGraph(combat.graph, combat.code, Hit).has_value());
}

TEST_CASE("State graph: a checked block lowers breadth first, each node's states and transitions together")
{
    const std::expected<Program, Diagnostics> program =
        CompileRobot("use robot;\nparam go: bool;\nparam hit: trigger;\nmachine m {\n"
                     "    node idle { }\n    node combat {\n        node ready { }\n        node swing { }\n"
                     "        ready -> swing when go;\n    }\n"
                     "    idle -> combat when hit;\n    any -> idle when !go;\n}\n");
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    const Layout layout = MakeLayout(*program, Robot());
    std::vector<Word> code;
    const std::expected<LoweredGraph, std::string> lowered =
        LowerGraph(layout, program->root.children[0], code);
    REQUIRE_MESSAGE(lowered.has_value(), lowered.error());
    const Graph &graph = lowered->graph;
    CHECK(VerifyGraph(graph, code, static_cast<uint32_t>(layout.slots.size())).has_value());

    REQUIRE(graph.nodes.size() == 5);
    CHECK(graph.nodes[1].name == "idle");
    CHECK(graph.nodes[2].name == "combat");
    CHECK(graph.nodes[3].name == "ready");
    CHECK(graph.nodes[4].name == "swing");
    REQUIRE(lowered->blocks.size() == graph.nodes.size());
    CHECK(lowered->blocks[2]->name == "combat");

    CHECK(graph.nodes[0].transitionCount == 2);
    CHECK(graph.nodes[2].transitionCount == 1);
    CHECK(graph.transitions[0].triggersRead == std::vector<uint32_t>{layout.ParamSlot(1)});
    CHECK(graph.transitions[1].sources == std::vector<uint32_t>{1});
    REQUIRE(lowered->transitions.size() == graph.transitions.size());

    std::vector<Word> block(layout.slots.size(), FromBool(false));
    std::vector<uint32_t> path{0};
    Descend(graph, path);
    block[layout.ParamSlot(1)] = FromBool(true);
    const std::optional<Fired> fired = StepGraph(graph, path, code, block, {});
    REQUIRE(fired.has_value());
    Enter(graph, fired->depth, graph.transitions[fired->transition].target, path);
    CHECK(PathNames(graph, path) == std::vector<std::string>{"combat", "ready"});
}
