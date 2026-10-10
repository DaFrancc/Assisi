/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Graph.hpp
/// @brief A block of states and the transitions between them, as a table a
///        cooked asset stores, and how a running machine moves through it.
///
/// States may hold states. A running machine is a path: the root, the state it
/// is in, the state that one is in, and so on down to a state that holds none.
/// Each frame the root's transitions are checked first, then those of each
/// state down the path, so a transition written on an outer state wins over
/// any inside it. Within one block the first true transition, in the order
/// written, fires.

#include <Assisi/Sigil/Bytecode.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Assisi::Sigil
{

/// @brief A transition between two states of one block.
struct GraphTransition
{
    /// The states it leaves, as indices among its block's states.
    std::vector<uint32_t> sources;
    /// Slots of the triggers its condition reads, which firing sets to false.
    std::vector<uint32_t> triggersRead;
    uint32_t condition = 0; ///< Where its condition's code starts.
    uint32_t target = 0;    ///< The state it enters, as an index among its block's states.
};

/// @brief A state, or the root. Its states are the nodes from `firstChild`,
///        and its transitions those from `firstTransition`.
struct Node
{
    std::string name; ///< Empty for the root.
    uint32_t firstChild = 0;
    uint32_t childCount = 0; ///< 0 for a state that holds none.
    uint32_t firstTransition = 0;
    uint32_t transitionCount = 0;
};

/// @brief Every node and transition of one machine. nodes[0] is the root, and
///        a node's states come after it, so going down never comes back round.
struct Graph
{
    std::vector<Node> nodes;
    std::vector<GraphTransition> transitions;
};

/// @brief Whether a transition may fire this frame. The owner of a machine
///        closes one when something outside the graph forbids it, such as a
///        fade it may not interrupt.
enum class Gate : uint8_t
{
    Open,
    Closed,
    Count_,
};

/// @brief The transition that fired, and the place in the path of the node it
///        belongs to, below which the path is replaced.
struct Fired
{
    uint32_t transition = 0;
    uint32_t depth = 0;
};

/// @brief Whether @p graph is safe to run against a block of @p slotCount
///        words with @p code, or why not: every range inside its array, every
///        node's states after it, every source and target one of its block's
///        states, every trigger a slot, and every condition passing Verify.
[[nodiscard]] std::expected<void, std::string> VerifyGraph(const Graph &graph, std::span<const Word> code,
                                                           uint32_t slotCount);

/// @brief Extends @p path from its last node through each first state down to
///        one that holds none.
void Descend(const Graph &graph, std::vector<uint32_t> &path);

/// @brief Replaces @p path below @p depth with state @p state of the node at
///        @p depth, then descends from it.
void Enter(const Graph &graph, uint32_t depth, uint32_t state, std::vector<uint32_t> &path);

/// @brief The transition that fires from @p path this frame, if one does.
///
/// Checks each node's transitions from the root down, in the order written,
/// passing over one whose gate is closed or that doesn't leave the state the
/// path is in. Firing sets the triggers its condition read to false; a
/// transition that doesn't fire leaves every trigger alone. An empty @p gates
/// opens every transition. Leaves @p path alone: the caller enters the target.
[[nodiscard]] std::optional<Fired> StepGraph(const Graph &graph, std::span<const uint32_t> path,
                                             std::span<const Word> code, std::span<Word> block,
                                             std::span<const Gate> gates);

/// @brief The names of the states @p path goes through, below the root.
[[nodiscard]] std::vector<std::string> PathNames(const Graph &graph, std::span<const uint32_t> path);

/// @brief Sets @p path to the states @p names name in @p graph, as far down as
///        each is still there, then descends: a state that is gone falls back to
///        the first state of the deepest one left.
void Remap(const Graph &graph, std::span<const std::string> names, std::vector<uint32_t> &path);

} // namespace Assisi::Sigil
