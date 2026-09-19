/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Navigation.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace Assisi::Mondrian
{
namespace
{

/// A rect's extent along one axis, from its start to its end.
struct Span
{
    float start = 0.f;
    float end = 0.f;

    [[nodiscard]] float Centre() const { return (start + end) / 2.f; }
};

/// @p rect along the axis @p direction moves on, measured the way it moves, so
/// that ahead is always larger whichever way that is.
Span Ahead(const Rect &rect, NavDirection direction)
{
    switch (direction)
    {
    case NavDirection::Up:
        return {.start = -(rect.y + rect.height), .end = -rect.y};
    case NavDirection::Down:
        return {.start = rect.y, .end = rect.y + rect.height};
    case NavDirection::Left:
        return {.start = -(rect.x + rect.width), .end = -rect.x};
    case NavDirection::Right:
    case NavDirection::Count:
        break;
    }
    return {.start = rect.x, .end = rect.x + rect.width};
}

/// @p rect across the axis @p direction moves on.
Span Across(const Rect &rect, NavDirection direction)
{
    const bool vertical = direction == NavDirection::Up || direction == NavDirection::Down;
    return vertical ? Span{.start = rect.x, .end = rect.x + rect.width}
                    : Span{.start = rect.y, .end = rect.y + rect.height};
}

/// The space between two spans, zero where they overlap.
float Gap(const Span &a, const Span &b)
{
    return std::max({0.f, b.start - a.end, a.start - b.end});
}

/// Every node that can take focus, in tree order.
std::vector<NodeId> Focusables(const NodeTree &tree, const LayoutResult &layout)
{
    std::vector<NodeId> found;
    std::vector<NodeId> pending{tree.Root()};
    std::vector<NodeId> children;
    while (!pending.empty())
    {
        const NodeId current = pending.back();
        pending.pop_back();
        if (CanFocus(tree, layout, current))
        {
            found.push_back(current);
        }
        // Pushed in reverse so the first child comes off the stack first.
        children.clear();
        for (NodeId child = tree.Get(current)->firstChild; child; child = tree.Get(child)->nextSibling)
        {
            children.push_back(child);
        }
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return found;
}

/// The best candidate by @p score, lowest first and then closest centre, or null.
template <typename Score> NodeId Best(const NodeTree &tree, const LayoutResult &layout, NodeId from, Score score)
{
    const Rect origin = layout.Get(from)->rect;
    const Point centre{.x = origin.x + (origin.width / 2.f), .y = origin.y + (origin.height / 2.f)};
    NodeId best;
    float bestScore = std::numeric_limits<float>::max();
    float bestDistance = std::numeric_limits<float>::max();
    for (const NodeId candidate : Focusables(tree, layout))
    {
        if (candidate == from)
        {
            continue;
        }
        const Rect rect = layout.Get(candidate)->rect;
        const std::optional<float> value = score(rect);
        if (!value)
        {
            continue;
        }
        const float dx = rect.x + (rect.width / 2.f) - centre.x;
        const float dy = rect.y + (rect.height / 2.f) - centre.y;
        const float distance = (dx * dx) + (dy * dy);
        if (*value < bestScore || (*value == bestScore && distance < bestDistance))
        {
            best = candidate;
            bestScore = *value;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace

bool CanFocus(const NodeTree &tree, const LayoutResult &layout, NodeId id)
{
    const Node *node = tree.Get(id);
    return node != nullptr && node->focusable && node->enabled && layout.Get(id) != nullptr;
}

NodeId FirstFocusable(const NodeTree &tree, const LayoutResult &layout)
{
    const std::vector<NodeId> all = Focusables(tree, layout);
    return all.empty() ? NodeId{} : all.front();
}

NodeId NextFocusable(const NodeTree &tree, const LayoutResult &layout, NodeId from, TabOrder order)
{
    const std::vector<NodeId> all = Focusables(tree, layout);
    if (all.empty())
    {
        return {};
    }
    const bool forward = order == TabOrder::Forward;
    const auto at = std::ranges::find(all, from);
    if (at == all.end())
    {
        return forward ? all.front() : all.back();
    }
    const std::size_t index = static_cast<std::size_t>(at - all.begin());
    const std::size_t count = all.size();
    return all[forward ? (index + 1) % count : (index + count - 1) % count];
}

NodeId Neighbour(const NodeTree &tree, const LayoutResult &layout, NodeId from, NavDirection direction, NavWrap wrap)
{
    const Node *node = tree.Get(from);
    if (node == nullptr || layout.Get(from) == nullptr)
    {
        return {};
    }
    const NodeId chosen = node->navOverride[static_cast<std::size_t>(direction)];
    if (CanFocus(tree, layout, chosen))
    {
        return chosen;
    }

    const Rect origin = layout.Get(from)->rect;
    const Span ahead = Ahead(origin, direction);
    const Span across = Across(origin, direction);

    const NodeId next = Best(tree, layout, from,
                             [&](const Rect &rect) -> std::optional<float>
                             {
                                 const Span candidate = Ahead(rect, direction);
                                 if (candidate.Centre() <= ahead.Centre())
                                 {
                                     return std::nullopt;
                                 }
                                 return std::max(0.f, candidate.start - ahead.end) +
                                        (kNavSidewaysWeight * Gap(across, Across(rect, direction)));
                             });
    if (next || wrap == NavWrap::Stop)
    {
        return next;
    }

    // Round to the far side: whatever lies furthest back is nearest from there.
    return Best(tree, layout, from, [&](const Rect &rect) -> std::optional<float>
                { return Ahead(rect, direction).start + (kNavSidewaysWeight * Gap(across, Across(rect, direction))); });
}

void ScrollIntoView(NodeTree &tree, const LayoutResult &layout, NodeId id)
{
    const LayoutNode *target = layout.Get(id);
    const Node *node = tree.Get(id);
    if (target == nullptr || node == nullptr || layout.scale <= 0.f)
    {
        return;
    }

    // Each ancestor that scrolls moves the rect it shows, which the ancestors
    // above it then see where it has moved to.
    Rect rect = target->rect;
    for (NodeId ancestor = node->parent; ancestor; ancestor = tree.Get(ancestor)->parent)
    {
        const LayoutNode *container = layout.Get(ancestor);
        const Node *scroller = tree.Get(ancestor);
        if (container == nullptr)
        {
            return;
        }
        Point offset = scroller->scrollOffset;
        const Rect &view = container->rect;

        // How far the content moves on @p axis, in device pixels.
        const auto scroll = [&](Axis axis) -> float
        {
            if (!scroller->style.scroll[static_cast<std::size_t>(axis)])
            {
                return 0.f;
            }
            const bool x = axis == Axis::X;
            const float start = x ? rect.x : rect.y;
            const float end = start + (x ? rect.width : rect.height);
            const float viewStart = x ? view.x : view.y;
            const float viewEnd = viewStart + (x ? view.width : view.height);

            // A node larger than the view shows its start rather than its end.
            float shift = 0.f;
            if (start < viewStart)
            {
                shift = start - viewStart;
            }
            else if (end > viewEnd)
            {
                shift = std::min(end - viewEnd, start - viewStart);
            }

            float &current = x ? offset.x : offset.y;
            const float content = x ? container->contentSize.x : container->contentSize.y;
            const float furthest = std::max(0.f, content - (viewEnd - viewStart)) / layout.scale;
            const float before = std::clamp(current, 0.f, furthest);
            current = std::clamp(before + (shift / layout.scale), 0.f, furthest);
            return (current - before) * layout.scale;
        };
        const float shiftX = scroll(Axis::X);
        const float shiftY = scroll(Axis::Y);
        tree.SetScrollOffset(ancestor, offset);
        rect.x -= shiftX;
        rect.y -= shiftY;
    }
}

} // namespace Assisi::Mondrian
