/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Layout.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace Assisi::Mondrian
{
namespace
{

/// Space smaller than this is none: it stops a distribution that would
/// otherwise chase float dust.
constexpr float kSizeEpsilon = 1.0e-3f;

/// Where Start, Center and End put something, as a fraction of the free space.
constexpr std::array<float, static_cast<std::size_t>(Alignment::Count)> kAlignFactor{0.f, 0.5f, 1.f};

constexpr Axis kAxes[] = {Axis::X, Axis::Y};

std::size_t At(Axis axis)
{
    return static_cast<std::size_t>(axis);
}

float AlignFactor(Alignment alignment)
{
    return kAlignFactor[static_cast<std::size_t>(alignment)];
}

float &Position(Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.x : rect.y;
}

float Position(const Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.x : rect.y;
}

float &Length(Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.width : rect.height;
}

float Length(const Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.width : rect.height;
}

float &Component(Point &point, Axis axis)
{
    return axis == Axis::X ? point.x : point.y;
}

float Component(const Point &point, Axis axis)
{
    return axis == Axis::X ? point.x : point.y;
}

float PaddingBefore(const Padding &padding, Axis axis)
{
    return axis == Axis::X ? padding.left : padding.top;
}

float PaddingAround(const Padding &padding, Axis axis)
{
    return axis == Axis::X ? padding.left + padding.right : padding.top + padding.bottom;
}

/// Whether @p axis is the one @p direction places children along.
bool IsMainAxis(Direction direction, Axis axis)
{
    return (direction == Direction::Row) == (axis == Axis::X);
}

Rect Intersect(const Rect &a, const Rect &b)
{
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.width, b.x + b.width);
    const float bottom = std::min(a.y + a.height, b.y + b.height);
    return Rect{.x = left, .y = top, .width = std::max(0.f, right - left), .height = std::max(0.f, bottom - top)};
}

/// @p rect with each edge rounded on its own, so neighbours that share an edge
/// keep sharing it.
Rect SnapEdges(const Rect &rect)
{
    const float left = std::round(rect.x);
    const float top = std::round(rect.y);
    return Rect{.x = left,
                .y = top,
                .width = std::round(rect.x + rect.width) - left,
                .height = std::round(rect.y + rect.height) - top};
}

/// The passes, over one tree into one result. A class only so the passes share
/// what every one of them reads.
class Layouter
{
  public:
    Layouter(const NodeTree &tree, LayoutResult &out, const Font *font, Extent viewport)
        : _slots(tree.Slots()), _out(out), _font(font), _root(tree.Root()), _viewport(viewport), _scale(out.scale)
    {
    }

    void Run()
    {
        const uint32_t root = _root.index;
        Prepare(root);
        FitAxis(root, Axis::X);
        DistributeAxis(root, Axis::X);
        WrapText();
        FitAxis(root, Axis::Y);
        DistributeAxis(root, Axis::Y);

        LayoutNode &rootNode = _out.nodes[root];
        rootNode.rect.x = 0.f;
        rootNode.rect.y = 0.f;
        rootNode.clip = kNoClip;
        Place(root);

        for (LayoutNode &node : _out.nodes)
        {
            if (node.placed)
            {
                node.rect = SnapEdges(node.rect);
                node.clip = SnapEdges(node.clip);
            }
        }
    }

  private:
    [[nodiscard]] const Node &Slot(uint32_t index) const { return _slots[index]; }
    [[nodiscard]] LayoutNode &Result(uint32_t index) const { return _out.nodes[index]; }
    [[nodiscard]] float Scaled(float logical) const { return logical * _scale; }

    [[nodiscard]] float ScaledMax(const Sizing &sizing) const
    {
        return sizing.max == kUnbounded ? kUnbounded : Scaled(sizing.max);
    }

    [[nodiscard]] float Clamp(const Sizing &sizing, float length) const
    {
        return std::clamp(length, Scaled(sizing.min), std::max(Scaled(sizing.min), ScaledMax(sizing)));
    }

    [[nodiscard]] bool IsInFlow(uint32_t index) const { return !Slot(index).style.floating.enabled; }

    /// Calls @p visit with the slot of every visible child of @p index, in order.
    template <typename Visit> void ForEachChild(uint32_t index, Visit &&visit) const
    {
        for (NodeId child = Slot(index).firstChild; child; child = Slot(child.index).nextSibling)
        {
            if (Slot(child.index).visible)
            {
                visit(child.index);
            }
        }
    }

    /// Marks what will be placed, and shapes each text once.
    void Prepare(uint32_t index)
    {
        const Node &node = Slot(index);
        LayoutNode &result = Result(index);
        result.placed = true;
        result.generation = node.generation;

        const float textSize = Scaled(node.style.textSize);
        if (!node.text.empty() && _font != nullptr && _font->pixelSize > 0.f && textSize > 0.f)
        {
            result.text = static_cast<uint32_t>(_out.texts.size());
            _out.texts.emplace_back();
            _shaped.push_back(Shape(node.text, *_font));
        }
        ForEachChild(index, [this](uint32_t child) { Prepare(child); });
    }

    /// Bottom-up: each node's length from its content, and the least it can
    /// shrink to. A Percent child counts as nothing here, which is what breaks
    /// the cycle of a Percent child inside a Fit parent.
    void FitAxis(uint32_t index, Axis axis)
    {
        ForEachChild(index, [this, axis](uint32_t child) { FitAxis(child, axis); });

        const Node &node = Slot(index);
        const Style &style = node.style;
        LayoutNode &result = Result(index);
        float content = 0.f;
        float minContent = 0.f;

        if (result.text != LayoutNode::kNoText)
        {
            if (axis == Axis::X)
            {
                const ShapedText &shaped = _shaped[result.text];
                const float size = Scaled(style.textSize);
                content = LayoutText(shaped, *_font, size, std::nullopt, style.textAlign).width;
                minContent = MeasureLongestWord(shaped, *_font, size);
            }
            else
            {
                content = _out.texts[result.text].height;
                minContent = content;
            }
        }
        else
        {
            const bool main = IsMainAxis(style.direction, axis);
            uint32_t count = 0;
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (!IsInFlow(child))
                             {
                                 return;
                             }
                             const bool percent = Slot(child).style.sizing[At(axis)].kind == SizingKind::Percent;
                             const float length = percent ? 0.f : Length(Result(child).rect, axis);
                             const float least = percent ? 0.f : Component(Result(child).minSize, axis);
                             content = main ? content + length : std::max(content, length);
                             minContent = main ? minContent + least : std::max(minContent, least);
                             ++count;
                         });
            if (main && count > 1)
            {
                const float gaps = Scaled(style.gap) * static_cast<float>(count - 1);
                content += gaps;
                minContent += gaps;
            }
        }

        const float padding = Scaled(PaddingAround(style.padding, axis));
        const Sizing &sizing = style.sizing[At(axis)];
        float &length = Length(result.rect, axis);
        float &least = Component(result.minSize, axis);
        if (index == _root.index)
        {
            length = static_cast<float>(axis == Axis::X ? _viewport.width : _viewport.height);
            least = length;
            return;
        }
        switch (sizing.kind)
        {
        case SizingKind::Fixed:
            length = Clamp(sizing, Scaled(sizing.value));
            least = length;
            break;
        case SizingKind::Percent:
            length = 0.f;
            least = 0.f;
            break;
        case SizingKind::Fit:
        case SizingKind::Grow:
        case SizingKind::Count:
            length = Clamp(sizing, content + padding);
            least = std::min(length, Clamp(sizing, minContent + padding));
            break;
        }
    }

    /// Top-down: resolves Percent children against this node's final length,
    /// then shares out what is left on the main axis, or takes back what
    /// overflows it; on the cross axis Grow children fill.
    void DistributeAxis(uint32_t index, Axis axis)
    {
        const Style &style = Slot(index).style;
        const float length = Length(Result(index).rect, axis);
        const float space = length - Scaled(PaddingAround(style.padding, axis));
        const bool main = IsMainAxis(style.direction, axis);
        const bool scrolls = style.scroll[At(axis)];

        float used = 0.f;
        uint32_t count = 0;
        ForEachChild(index,
                     [&](uint32_t child)
                     {
                         const Sizing &sizing = Slot(child).style.sizing[At(axis)];
                         float &childLength = Length(Result(child).rect, axis);
                         if (!IsInFlow(child))
                         {
                             FloatLength(child, axis, index);
                             return;
                         }
                         if (sizing.kind == SizingKind::Percent)
                         {
                             childLength = Clamp(sizing, sizing.value * space);
                             Component(Result(child).minSize, axis) = childLength;
                         }
                         if (!main)
                         {
                             if (sizing.kind == SizingKind::Grow)
                             {
                                 childLength = Clamp(sizing, space);
                             }
                             else if (childLength > space && !scrolls && sizing.kind == SizingKind::Fit)
                             {
                                 childLength = std::max(space, Component(Result(child).minSize, axis));
                             }
                         }
                         used += childLength;
                         ++count;
                     });

        if (main && count > 0)
        {
            const float remaining = space - used - Scaled(style.gap) * static_cast<float>(count - 1);
            if (remaining > kSizeEpsilon)
            {
                Grow(index, axis, remaining, count);
            }
            else if (remaining < -kSizeEpsilon && !scrolls)
            {
                Shrink(index, axis, -remaining, count);
            }
        }

        ForEachChild(index, [this, axis](uint32_t child) { DistributeAxis(child, axis); });
    }

    /// A floating node's Percent and Grow lengths, against what it floats on.
    void FloatLength(uint32_t index, Axis axis, uint32_t parent)
    {
        const Style &style = Slot(index).style;
        const Sizing &sizing = style.sizing[At(axis)];
        const float anchor = style.floating.target == FloatAnchor::Root
                                 ? static_cast<float>(axis == Axis::X ? _viewport.width : _viewport.height)
                                 : Length(Result(parent).rect, axis);
        float &length = Length(Result(index).rect, axis);
        if (sizing.kind == SizingKind::Percent)
        {
            length = Clamp(sizing, sizing.value * anchor);
        }
        else if (sizing.kind == SizingKind::Grow)
        {
            length = Clamp(sizing, anchor);
        }
    }

    [[nodiscard]] bool CanGrow(uint32_t child, Axis axis) const
    {
        const Sizing &sizing = Slot(child).style.sizing[At(axis)];
        return IsInFlow(child) && sizing.kind == SizingKind::Grow &&
               Length(Result(child).rect, axis) < ScaledMax(sizing) - kSizeEpsilon;
    }

    [[nodiscard]] bool CanShrink(uint32_t child, Axis axis) const
    {
        const SizingKind kind = Slot(child).style.sizing[At(axis)].kind;
        return IsInFlow(child) && (kind == SizingKind::Fit || kind == SizingKind::Grow) &&
               Length(Result(child).rect, axis) > Component(Result(child).minSize, axis) + kSizeEpsilon;
    }

    /// Hands @p remaining to the Grow children, always to the smallest first,
    /// so they even out before any grows past another; each stops at its max.
    void Grow(uint32_t index, Axis axis, float remaining, uint32_t children)
    {
        // Every round either spends what is left, brings the smallest up to the
        // next size, or retires a child at its max.
        const uint32_t rounds = 2 * children + 1;
        for (uint32_t round = 0; remaining > kSizeEpsilon; ++round)
        {
            ASSISI_ASSERT(round <= rounds, "Grow distribution did not settle");
            float smallest = kUnbounded;
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (CanGrow(child, axis))
                             {
                                 smallest = std::min(smallest, Length(Result(child).rect, axis));
                             }
                         });
            if (smallest == kUnbounded)
            {
                return;
            }

            float next = kUnbounded;
            float room = kUnbounded;
            uint32_t tied = 0;
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (!CanGrow(child, axis))
                             {
                                 return;
                             }
                             const float length = Length(Result(child).rect, axis);
                             if (length <= smallest + kSizeEpsilon)
                             {
                                 ++tied;
                                 room = std::min(room, ScaledMax(Slot(child).style.sizing[At(axis)]) - length);
                             }
                             else
                             {
                                 next = std::min(next, length);
                             }
                         });

            const float step = std::min({next - smallest, remaining / static_cast<float>(tied), room});
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (CanGrow(child, axis) && Length(Result(child).rect, axis) <= smallest + kSizeEpsilon)
                             {
                                 Length(Result(child).rect, axis) += step;
                             }
                         });
            remaining -= step * static_cast<float>(tied);
        }
    }

    /// Takes @p overflow back from the Fit and Grow children, always from the
    /// largest first; none goes below the least it can shrink to.
    void Shrink(uint32_t index, Axis axis, float overflow, uint32_t children)
    {
        const uint32_t rounds = 2 * children + 1;
        for (uint32_t round = 0; overflow > kSizeEpsilon; ++round)
        {
            ASSISI_ASSERT(round <= rounds, "Shrink distribution did not settle");
            float largest = -kUnbounded;
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (CanShrink(child, axis))
                             {
                                 largest = std::max(largest, Length(Result(child).rect, axis));
                             }
                         });
            if (largest == -kUnbounded)
            {
                return;
            }

            float next = -kUnbounded;
            float room = kUnbounded;
            uint32_t tied = 0;
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (!CanShrink(child, axis))
                             {
                                 return;
                             }
                             const float length = Length(Result(child).rect, axis);
                             if (length >= largest - kSizeEpsilon)
                             {
                                 ++tied;
                                 room = std::min(room, length - Component(Result(child).minSize, axis));
                             }
                             else
                             {
                                 next = std::max(next, length);
                             }
                         });

            const float step = std::min({largest - next, overflow / static_cast<float>(tied), room});
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (CanShrink(child, axis) && Length(Result(child).rect, axis) >= largest - kSizeEpsilon)
                             {
                                 Length(Result(child).rect, axis) -= step;
                             }
                         });
            overflow -= step * static_cast<float>(tied);
        }
    }

    /// Sets every text at its node's final width, which is what its height needs.
    void WrapText()
    {
        for (uint32_t index = 0; index < _out.nodes.size(); ++index)
        {
            const LayoutNode &result = _out.nodes[index];
            if (!result.placed || result.text == LayoutNode::kNoText)
            {
                continue;
            }
            const Style &style = Slot(index).style;
            const float space = result.rect.width - Scaled(PaddingAround(style.padding, Axis::X));
            // At least one pixel: a node squeezed to nothing still sets its text,
            // a glyph to a line.
            const float wrap = std::max(1.f, std::floor(space));
            _out.texts[result.text] =
                LayoutText(_shaped[result.text], *_font, Scaled(style.textSize), wrap, style.textAlign);
        }
    }

    /// Top-down: positions children from this node's, and the clip each draws with.
    void Place(uint32_t index)
    {
        const Style &style = Slot(index).style;
        LayoutNode &result = Result(index);
        const Rect rect = result.rect;
        const Axis main = style.direction == Direction::Row ? Axis::X : Axis::Y;
        const Axis cross = main == Axis::X ? Axis::Y : Axis::X;

        // The content's extent, which is what scrolling moves through.
        float mainExtent = 0.f;
        float crossExtent = 0.f;
        uint32_t count = 0;
        ForEachChild(index,
                     [&](uint32_t child)
                     {
                         if (IsInFlow(child))
                         {
                             mainExtent += Length(Result(child).rect, main);
                             crossExtent = std::max(crossExtent, Length(Result(child).rect, cross));
                             ++count;
                         }
                     });
        if (count > 1)
        {
            mainExtent += Scaled(style.gap) * static_cast<float>(count - 1);
        }
        Component(result.contentSize, main) = mainExtent + Scaled(PaddingAround(style.padding, main));
        Component(result.contentSize, cross) = crossExtent + Scaled(PaddingAround(style.padding, cross));

        Point origin;
        Point space;
        Point scroll;
        for (const Axis axis : kAxes)
        {
            Component(origin, axis) = Position(rect, axis) + Scaled(PaddingBefore(style.padding, axis));
            Component(space, axis) = Length(rect, axis) - Scaled(PaddingAround(style.padding, axis));
            if (style.scroll[At(axis)])
            {
                const float furthest = std::max(0.f, Component(result.contentSize, axis) - Length(rect, axis));
                Component(scroll, axis) = std::clamp(Scaled(Component(Slot(index).scrollOffset, axis)), 0.f, furthest);
            }
        }

        const bool clips = style.scroll[At(Axis::X)] || style.scroll[At(Axis::Y)];
        const Rect childClip = clips ? Intersect(result.clip, rect) : result.clip;
        const Rect parentClip = Intersect(result.clip, rect);

        const float leftover = Component(space, main) - mainExtent;
        float cursor = Component(origin, main) - Component(scroll, main) +
                       (leftover > 0.f ? leftover * AlignFactor(style.childAlign[At(main)]) : 0.f);
        ForEachChild(index,
                     [&](uint32_t child)
                     {
                         LayoutNode &childResult = Result(child);
                         if (!IsInFlow(child))
                         {
                             PlaceFloating(child, index);
                             childResult.clip = Slot(child).style.floating.clipToParent ? parentClip : kNoClip;
                             return;
                         }
                         Position(childResult.rect, main) = cursor;
                         cursor += Length(childResult.rect, main) + Scaled(style.gap);

                         const float free = Component(space, cross) - Length(childResult.rect, cross);
                         Position(childResult.rect, cross) =
                             Component(origin, cross) - Component(scroll, cross) +
                             (free > 0.f ? free * AlignFactor(style.childAlign[At(cross)]) : 0.f);
                         childResult.clip = childClip;
                     });

        ForEachChild(index, [this](uint32_t child) { Place(child); });
    }

    /// Lands a floating node's attach point on its anchor point, then offsets it.
    void PlaceFloating(uint32_t index, uint32_t parent)
    {
        const Floating &floating = Slot(index).style.floating;
        const Rect anchor = floating.target == FloatAnchor::Root ? Result(_root.index).rect : Result(parent).rect;
        LayoutNode &result = Result(index);
        for (const Axis axis : kAxes)
        {
            Position(result.rect, axis) = Position(anchor, axis) +
                                          Length(anchor, axis) * AlignFactor(floating.anchor[At(axis)]) -
                                          Length(result.rect, axis) * AlignFactor(floating.attach[At(axis)]) +
                                          Scaled(Component(floating.offset, axis));
        }
    }

    std::vector<ShapedText> _shaped; ///< by text index
    std::span<const Node> _slots;
    LayoutResult &_out;
    const Font *_font;
    NodeId _root;
    Extent _viewport;
    float _scale;
};

} // namespace

float UiScale(Extent viewport, float userScale)
{
    const float fit = std::min(static_cast<float>(viewport.width) / kReferenceWidth,
                               static_cast<float>(viewport.height) / kReferenceHeight);
    return fit * userScale;
}

const LayoutNode *LayoutResult::Get(NodeId id) const
{
    if (id.index >= nodes.size())
    {
        return nullptr;
    }
    const LayoutNode &node = nodes[id.index];
    return node.placed && node.generation == id.generation ? &node : nullptr;
}

void ComputeLayout(const NodeTree &tree, Extent viewport, float scale, const Font *font, LayoutResult &out)
{
    ASSISI_ASSERT(scale > 0.f && std::isfinite(scale), "ComputeLayout needs a positive, finite scale");

    out.nodes.assign(tree.Slots().size(), LayoutNode{});
    out.texts.clear();
    out.scale = scale;

    Layouter(tree, out, font, viewport).Run();
}

} // namespace Assisi::Mondrian
