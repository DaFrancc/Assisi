/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Graph.hpp>

#include <Assisi/Sigil/Evaluate.hpp>
#include <Assisi/Sigil/Verify.hpp>

#include <algorithm>
#include <cstddef>
#include <format>

namespace Assisi::Sigil
{

namespace
{

/// Whether [first, first + count) lies inside an array of @p size.
bool InRange(uint32_t first, uint32_t count, std::size_t size)
{
    return first <= size && count <= size - first;
}

std::expected<void, std::string> VerifyNode(const Graph &graph, uint32_t index)
{
    const Node &node = graph.nodes[index];
    const bool statesAfter =
        node.firstChild > index && InRange(node.firstChild, node.childCount, graph.nodes.size());
    if (node.childCount > 0 && !statesAfter)
    {
        return std::unexpected(std::format("node {}'s states are not after it in the graph", index));
    }
    if (!InRange(node.firstTransition, node.transitionCount, graph.transitions.size()))
    {
        return std::unexpected(std::format("node {}'s transitions run past the graph's", index));
    }
    return {};
}

std::expected<void, std::string> VerifyTransition(const GraphTransition &transition, uint32_t states,
                                                  std::span<const Word> code, uint32_t slotCount)
{
    const bool sourcesInside =
        std::ranges::all_of(transition.sources, [states](uint32_t source) { return source < states; });
    if (!sourcesInside || transition.target >= states)
    {
        return std::unexpected("a transition names a state its block doesn't have");
    }
    const bool triggersInside =
        std::ranges::all_of(transition.triggersRead, [slotCount](uint32_t slot) { return slot < slotCount; });
    if (!triggersInside)
    {
        return std::unexpected("a transition's trigger is outside the block");
    }
    return Verify(code, transition.condition, slotCount);
}

} // namespace

std::expected<void, std::string> VerifyGraph(const Graph &graph, std::span<const Word> code, uint32_t slotCount)
{
    if (graph.nodes.empty())
    {
        return std::unexpected("a graph has no root");
    }
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
    {
        if (std::expected<void, std::string> verified = VerifyNode(graph, index); !verified)
        {
            return verified;
        }
        const Node &node = graph.nodes[index];
        for (uint32_t t = node.firstTransition; t < node.firstTransition + node.transitionCount; ++t)
        {
            std::expected<void, std::string> verified =
                VerifyTransition(graph.transitions[t], node.childCount, code, slotCount);
            if (!verified)
            {
                return std::unexpected(std::format("node {}: {}", index, verified.error()));
            }
        }
    }
    return {};
}

void Descend(const Graph &graph, std::vector<uint32_t> &path)
{
    while (graph.nodes[path.back()].childCount > 0)
    {
        path.push_back(graph.nodes[path.back()].firstChild);
    }
}

void Enter(const Graph &graph, uint32_t depth, uint32_t state, std::vector<uint32_t> &path)
{
    path.resize(depth + 1);
    path.push_back(graph.nodes[path[depth]].firstChild + state);
    Descend(graph, path);
}

std::optional<Fired> StepGraph(const Graph &graph, std::span<const uint32_t> path, std::span<const Word> code,
                               std::span<Word> block, std::span<const Gate> gates)
{
    for (uint32_t depth = 0; depth + 1 < path.size(); ++depth)
    {
        const Node &node = graph.nodes[path[depth]];
        const uint32_t current = path[depth + 1] - node.firstChild;
        for (uint32_t t = node.firstTransition; t < node.firstTransition + node.transitionCount; ++t)
        {
            const GraphTransition &transition = graph.transitions[t];
            if (!gates.empty() && gates[t] == Gate::Closed)
            {
                continue;
            }
            if (std::ranges::find(transition.sources, current) == transition.sources.end())
            {
                continue;
            }
            if (!ToBool(Evaluate(code, transition.condition, block)))
            {
                continue;
            }
            for (const uint32_t slot : transition.triggersRead)
            {
                block[slot] = FromBool(false);
            }
            return Fired{.transition = t, .depth = depth};
        }
    }
    return std::nullopt;
}

std::vector<std::string> PathNames(const Graph &graph, std::span<const uint32_t> path)
{
    std::vector<std::string> names;
    for (std::size_t i = 1; i < path.size(); ++i)
    {
        names.push_back(graph.nodes[path[i]].name);
    }
    return names;
}

void Remap(const Graph &graph, std::span<const std::string> names, std::vector<uint32_t> &path)
{
    path.assign(1, 0);
    for (const std::string &name : names)
    {
        const Node &node = graph.nodes[path.back()];
        const std::span<const Node> states{graph.nodes.data() + node.firstChild, node.childCount};
        const std::span<const Node>::iterator found =
            std::ranges::find_if(states, [&name](const Node &state) { return state.name == name; });
        if (found == states.end())
        {
            break;
        }
        path.push_back(node.firstChild + static_cast<uint32_t>(found - states.begin()));
    }
    Descend(graph, path);
}

} // namespace Assisi::Sigil
