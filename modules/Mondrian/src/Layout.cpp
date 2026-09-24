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

float &Span(Rect &rect, Axis axis)
{
    return axis == Axis::X ? rect.width : rect.height;
}

float Span(const Rect &rect, Axis axis)
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

/// @p axis's leading edge of @p insets, and both its edges together.
float Before(const Insets &insets, Axis axis)
{
    return axis == Axis::X ? insets.left : insets.top;
}

float Around(const Insets &insets, Axis axis)
{
    return axis == Axis::X ? insets.left + insets.right : insets.top + insets.bottom;
}

/// The viewport's length on @p axis, in device pixels.
float ViewportSide(Extent viewport, Axis axis)
{
    return static_cast<float>(axis == Axis::X ? viewport.width : viewport.height);
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
        : _slots(tree.Slots()), _widgets(&tree.Widgets()), _out(out), _font(font), _root(tree.Root()),
          _viewport(viewport), _scale(out.scale)
    {
    }

    void Run()
    {
        const uint32_t root = _root.index;
        Prepare(root, Scaled(kDefaultTextSize));
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
        ResolveDrawn();
    }

  private:
    [[nodiscard]] const Node &Slot(uint32_t index) const { return _slots[index]; }
    [[nodiscard]] LayoutNode &Result(uint32_t index) const { return _out.nodes[index]; }
    [[nodiscard]] float Scaled(float logical) const { return logical * _scale; }

    /// @p length in device pixels. @p share is what a `%` is of, and
    /// @p textSize, in device pixels, what an `em` is of.
    [[nodiscard]] float Resolve(Length length, float share, float textSize) const
    {
        switch (length.unit)
        {
        case LengthUnit::Px:
            return length.value == kUnbounded ? kUnbounded : Scaled(length.value);
        case LengthUnit::Percent:
            return length.value / kPercentOf * share;
        case LengthUnit::Vw:
            return length.value / kPercentOf * ViewportSide(_viewport, Axis::X);
        case LengthUnit::Vh:
            return length.value / kPercentOf * ViewportSide(_viewport, Axis::Y);
        case LengthUnit::Em:
            return length.value * textSize;
        case LengthUnit::Count:
            break;
        }
        return 0.f;
    }

    /// @p length held to what @p index's sizing allows on @p axis.
    [[nodiscard]] float Clamp(uint32_t index, Axis axis, float length) const
    {
        const LayoutNode &result = Result(index);
        const float least = result.minLength[At(axis)];
        return std::clamp(length, least, std::max(least, result.maxLength[At(axis)]));
    }

    /// Resolves @p index's lengths on @p axis — its padding there, its gap if
    /// that is the axis it places children along, its sizing — against
    /// @p share, what a `%` of them is of, and sizes it from the content
    /// FitAxis measured.
    ///
    /// The fit pass calls this with a share of nothing, since the parent is
    /// not sized yet; the parent's own pass calls it again with the parent's
    /// content size. Where no length is a `%` the two agree, so a node is sized
    /// by one formula whichever pass it is in.
    void SizeFromContent(uint32_t index, Axis axis, float share)
    {
        const Style &style = Slot(index).style;
        LayoutNode &result = Result(index);
        const float text = result.textSize;
        if (axis == Axis::X)
        {
            result.padding.left = Resolve(style.padding.left, share, text);
            result.padding.right = Resolve(style.padding.right, share, text);
        }
        else
        {
            result.padding.top = Resolve(style.padding.top, share, text);
            result.padding.bottom = Resolve(style.padding.bottom, share, text);
        }
        const bool main = IsMainAxis(style.direction, axis);
        if (main)
        {
            result.gap = Resolve(style.gap, share, text);
        }
        const Sizing &sizing = style.sizing[At(axis)];
        result.minLength[At(axis)] = Resolve(sizing.min, share, text);
        result.maxLength[At(axis)] = Resolve(sizing.max, share, text);

        float &length = Span(result.rect, axis);
        float &least = Component(result.minSize, axis);
        if (index == _root.index)
        {
            length = ViewportSide(_viewport, axis);
            least = length;
            return;
        }

        const float gaps =
            main && result.inFlowChildren > 1 ? result.gap * static_cast<float>(result.inFlowChildren - 1) : 0.f;
        const float around = Around(result.padding, axis) + gaps;
        switch (sizing.kind)
        {
        case SizingKind::Fixed:
            length = Clamp(index, axis, Resolve(sizing.value, share, text));
            least = length;
            break;
        case SizingKind::Fit:
        case SizingKind::Grow:
        case SizingKind::Count:
            length = Clamp(index, axis, Component(result.content, axis) + around);
            least = std::min(length, Clamp(index, axis, Component(result.minContent, axis) + around));
            break;
        }
    }

    /// The lengths with no axis of their own, against the node's own shorter
    /// side once its box is final: what drawing reads.
    void ResolveDrawn()
    {
        for (uint32_t index = 0; index < _out.nodes.size(); ++index)
        {
            LayoutNode &result = Result(index);
            if (!result.placed)
            {
                continue;
            }
            const Style &style = Slot(index).style;
            const float side = std::min(result.rect.width, result.rect.height);
            result.borderWidth = Resolve(style.borderWidth, side, result.textSize);
            result.cornerRadius = Resolve(style.cornerRadius, side, result.textSize);
            result.scrollBarMinLength = Resolve(style.scrollBarMinLength, side, result.textSize);
        }
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

    /// Marks what will be placed, resolves each node's text size against its
    /// parent's (@p parentTextSize, device pixels), and shapes each text once.
    void Prepare(uint32_t index, float parentTextSize)
    {
        const Node &node = Slot(index);
        LayoutNode &result = Result(index);
        result.placed = true;
        result.generation = node.generation;

        // A text size's `%` and `em` are of its parent's, as in CSS: a size
        // cannot be a share of itself.
        const float textSize = Resolve(node.style.textSize, parentTextSize, parentTextSize);
        result.textSize = textSize;

        // A field is shaped even while it is empty, so that it has a line to
        // stand its caret on and a height that does not change on the first
        // character typed.
        const bool field = node.edit.editing == TextEditing::Editable;
        if ((!node.text.empty() || field) && _font != nullptr && _font->pixelSize > 0.f && textSize > 0.f)
        {
            // An empty field stands its placeholder in for the text it has
            // none of, so the words are laid out and drawn by everything that
            // already handles text. Nothing else reads it: the caret has no
            // text to sit in, and what the field holds is still nothing.
            result.placeholder = field && node.text.empty() && !node.edit.placeholder.empty();
            const std::string_view source = result.placeholder ? std::string_view{node.edit.placeholder}
                                                               : ShownText(node.text, node.edit.mask, _marks);
            result.text = static_cast<uint32_t>(_out.texts.size());
            _out.texts.emplace_back();
            _shaped.push_back(Shape(source, *_font));
        }
        ForEachChild(index, [this, textSize](uint32_t child) { Prepare(child, textSize); });
    }

    /// Bottom-up: each node's length from its content, and the least it can
    /// shrink to. A `%` length counts as nothing here, the parent it is a
    /// share of not being sized yet; that is what breaks the cycle of a `%`
    /// child inside a Fit parent.
    void FitAxis(uint32_t index, Axis axis)
    {
        ForEachChild(index, [this, axis](uint32_t child) { FitAxis(child, axis); });

        const Node &node = Slot(index);
        const Style &style = node.style;
        LayoutNode &result = Result(index);
        float content = 0.f;
        float minContent = 0.f;
        uint32_t count = 0;

        const WidgetType *widget = _widgets->Get(node.behaviour);
        if (widget != nullptr && widget->measure != nullptr)
        {
            // A control's own size comes before whatever its text or children
            // would have asked for: a slider is as long as a slider, not as its
            // label.
            content = Scaled(Component(widget->measure(node, widget->context), axis));
            minContent = content;
        }
        else if (result.text != LayoutNode::kNoText)
        {
            if (axis == Axis::X && node.edit.editing == TextEditing::Editable)
            {
                // A field is as wide as it is given and no wider. Taking its
                // width from what has been typed into it makes the box grow
                // with every character, push its neighbours aside, and leave
                // the screen; the text scrolls inside the box instead.
                content = 0.f;
                minContent = 0.f;
            }
            else if (axis == Axis::X)
            {
                const ShapedText &shaped = _shaped[result.text];
                content = LayoutText(shaped, *_font, result.textSize, std::nullopt, style.textAlign).width;
                minContent = MeasureLongestWord(shaped, *_font, result.textSize);
            }
            else
            {
                content = FieldHeight(node, _out.texts[result.text]);
                minContent = content;
            }
        }
        else
        {
            const bool main = IsMainAxis(style.direction, axis);
            ForEachChild(index,
                         [&](uint32_t child)
                         {
                             if (!IsInFlow(child))
                             {
                                 return;
                             }
                             const float length = Span(Result(child).rect, axis);
                             const float least = Component(Result(child).minSize, axis);
                             content = main ? content + length : std::max(content, length);
                             minContent = main ? minContent + least : std::max(minContent, least);
                             ++count;
                         });
        }

        Component(result.content, axis) = content;
        Component(result.minContent, axis) = minContent;
        result.inFlowChildren = count;
        // The root is the viewport, which is what its own `%` is a share of.
        SizeFromContent(index, axis, index == _root.index ? ViewportSide(_viewport, axis) : 0.f);
    }

    /// Top-down: resizes each child against this node's final content size,
    /// which is what its `%` lengths are a share of, then shares out what is
    /// left on the main axis, or takes back what overflows it; on the cross
    /// axis Grow children fill.
    void DistributeAxis(uint32_t index, Axis axis)
    {
        const Style &style = Slot(index).style;
        const LayoutNode &result = Result(index);
        const float space = Span(result.rect, axis) - Around(result.padding, axis);
        const bool main = IsMainAxis(style.direction, axis);
        const bool scrolls = style.enabledScrollBars[At(axis)];

        float used = 0.f;
        uint32_t count = 0;
        ForEachChild(index,
                     [&](uint32_t child)
                     {
                         const Sizing &sizing = Slot(child).style.sizing[At(axis)];
                         float &childLength = Span(Result(child).rect, axis);
                         if (!IsInFlow(child))
                         {
                             FloatLength(child, axis, index);
                             return;
                         }
                         SizeFromContent(child, axis, space);
                         if (!main)
                         {
                             if (sizing.kind == SizingKind::Grow)
                             {
                                 childLength = Clamp(child, axis, space);
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
            const float remaining = space - used - result.gap * static_cast<float>(count - 1);
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

    /// A floating node's lengths, against what it floats on: its `%` are a
    /// share of that, and Grow fills it.
    void FloatLength(uint32_t index, Axis axis, uint32_t parent)
    {
        const Style &style = Slot(index).style;
        const float anchor = style.floating.target == FloatAnchor::Root ? ViewportSide(_viewport, axis)
                                                                        : Span(Result(parent).rect, axis);
        SizeFromContent(index, axis, anchor);
        if (style.sizing[At(axis)].kind == SizingKind::Grow)
        {
            Span(Result(index).rect, axis) = Clamp(index, axis, anchor);
        }
    }

    [[nodiscard]] bool CanGrow(uint32_t child, Axis axis) const
    {
        return IsInFlow(child) && Slot(child).style.sizing[At(axis)].kind == SizingKind::Grow &&
               Span(Result(child).rect, axis) < Result(child).maxLength[At(axis)] - kSizeEpsilon;
    }

    [[nodiscard]] bool CanShrink(uint32_t child, Axis axis) const
    {
        const SizingKind kind = Slot(child).style.sizing[At(axis)].kind;
        return IsInFlow(child) && (kind == SizingKind::Fit || kind == SizingKind::Grow) &&
               Span(Result(child).rect, axis) > Component(Result(child).minSize, axis) + kSizeEpsilon;
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
                                 smallest = std::min(smallest, Span(Result(child).rect, axis));
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
                             const float length = Span(Result(child).rect, axis);
                             if (length <= smallest + kSizeEpsilon)
                             {
                                 ++tied;
                                 room = std::min(room, Result(child).maxLength[At(axis)] - length);
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
                             if (CanGrow(child, axis) && Span(Result(child).rect, axis) <= smallest + kSizeEpsilon)
                             {
                                 Span(Result(child).rect, axis) += step;
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
                                 largest = std::max(largest, Span(Result(child).rect, axis));
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
                             const float length = Span(Result(child).rect, axis);
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
                             if (CanShrink(child, axis) && Span(Result(child).rect, axis) >= largest - kSizeEpsilon)
                             {
                                 Span(Result(child).rect, axis) -= step;
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
            const Node &node = Slot(index);
            const Style &style = node.style;
            const float wrap = TextWrapWidth(result);
            // A single line does not wrap however narrow its box is; it runs on
            // and the box scrolls along it.
            const bool oneLine = node.edit.editing != TextEditing::None && node.edit.lines == TextLines::Single;
            _out.texts[result.text] = LayoutText(_shaped[result.text], *_font, result.textSize,
                                                 oneLine ? std::nullopt : std::optional<float>{wrap}, style.textAlign);
            if (oneLine)
            {
                Result(index).textScroll =
                    LineScroll(node, _out.texts[result.text], result.rect.width - Around(result.padding, Axis::X));
            }
        }
    }

    /// How tall @p node's text makes it, in device pixels.
    ///
    /// A field of many lines may be held to a number of them, so that a box in
    /// a form keeps its size however much is typed into it. Everything else is
    /// as tall as its text.
    [[nodiscard]] static float FieldHeight(const Node &node, const TextLayout &text)
    {
        const TextEdit &edit = node.edit;
        const uint32_t showing = static_cast<uint32_t>(text.lines.size());
        switch (edit.height)
        {
        case TextHeight::UpTo:
            return BlockHeight(text, std::min(showing, edit.lineLimit));
        case TextHeight::Exactly:
            return BlockHeight(text, edit.lineLimit);
        case TextHeight::Unbounded:
        case TextHeight::Count:
            break;
        }
        return text.height;
    }

    /// How far a single line is shifted left so that its caret is inside the
    /// @p space its box has, in device pixels.
    ///
    /// It moves as little as it can: the line stays where it was unless the
    /// caret has gone off one end, which is what stops the text sliding about
    /// while someone is reading it.
    [[nodiscard]] float LineScroll(const Node &node, const TextLayout &text, float space) const
    {
        std::string marks;
        const std::string_view shown = ShownText(node.text, node.edit.mask, marks);
        const float caret = PlaceCaret(text, shown, node.edit.caret).x;
        // The caret sits at the right edge of the text it follows, so the line
        // has to shift by its own width again to leave it somewhere to draw.
        const float leftmost = std::min(caret, caret - space + Scaled(kCaretWidth));
        const float kept = std::clamp(Scaled(node.edit.scrolled), leftmost, caret);
        return std::clamp(kept, 0.f, std::max(0.f, text.width - space));
    }

    /// Top-down: positions children from this node's, and the clip each draws with.
    void Place(uint32_t index)
    {
        const Style &style = Slot(index).style;
        LayoutNode &result = Result(index);
        const Rect rect = result.rect;

        // A field keeps its text to itself: the line inside it runs on past
        // both ends, and what is off the end of the box must not be drawn over
        // whatever sits beside it.
        if (Slot(index).edit.editing == TextEditing::Editable)
        {
            result.clip = Intersect(result.clip, rect);
        }
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
                             mainExtent += Span(Result(child).rect, main);
                             crossExtent = std::max(crossExtent, Span(Result(child).rect, cross));
                             ++count;
                         }
                     });
        if (count > 1)
        {
            mainExtent += result.gap * static_cast<float>(count - 1);
        }
        Component(result.contentSize, main) = mainExtent + Around(result.padding, main);
        Component(result.contentSize, cross) = crossExtent + Around(result.padding, cross);

        Point origin;
        Point space;
        Point scroll;
        for (const Axis axis : kAxes)
        {
            Component(origin, axis) = Position(rect, axis) + Before(result.padding, axis);
            Component(space, axis) = Span(rect, axis) - Around(result.padding, axis);
            if (style.enabledScrollBars[At(axis)])
            {
                const float furthest = std::max(0.f, Component(result.contentSize, axis) - Span(rect, axis));
                Component(scroll, axis) = std::clamp(Scaled(Component(Slot(index).scrollOffset, axis)), 0.f, furthest);
            }
        }

        const bool clips = style.enabledScrollBars[At(Axis::X)] || style.enabledScrollBars[At(Axis::Y)];
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
                         cursor += Span(childResult.rect, main) + result.gap;

                         const float free = Component(space, cross) - Span(childResult.rect, cross);
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
            // A `%` offset is a share of what the node floats on.
            Position(result.rect, axis) = Position(anchor, axis) +
                                          Span(anchor, axis) * AlignFactor(floating.anchor[At(axis)]) -
                                          Span(result.rect, axis) * AlignFactor(floating.attach[At(axis)]) +
                                          Resolve(floating.offset[At(axis)], Span(anchor, axis), result.textSize);
        }
    }

    std::vector<ShapedText> _shaped; ///< by text index
    std::string _marks;              ///< scratch for the marks a masked field shows
    std::span<const Node> _slots;
    const WidgetRegistry *_widgets = nullptr;
    LayoutResult &_out;
    const Font *_font;
    NodeId _root;
    Extent _viewport;
    float _scale;
};

} // namespace

float UiScale(Extent viewport, float userScale, ScaleMatch match)
{
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    float matched = 0.f;
    switch (match)
    {
    case ScaleMatch::ShorterSide:
        matched = std::min(width, height) / kReferenceHeight;
        break;
    case ScaleMatch::Width:
        matched = width / kReferenceWidth;
        break;
    case ScaleMatch::Height:
        matched = height / kReferenceHeight;
        break;
    case ScaleMatch::Count:
        break;
    }
    return matched * userScale;
}

float TextWrapWidth(const LayoutNode &placed)
{
    const float space = placed.rect.width - (placed.padding.left + placed.padding.right);
    // Rounded up, never down. A node that fits its text is sized from a width
    // rounded up, and its box is then snapped to whole pixels wherever it
    // landed; rounding the room left inside it down can take it back under the
    // width the text was measured at, and a word with no space in it breaks
    // mid-word rather than anywhere a reader would accept. A wrap width a
    // fraction wider than the box costs a pixel of overflow, which the node
    // already clips.
    return std::max(1.f, std::ceil(space));
}

Point TextOrigin(const LayoutNode &placed)
{
    return {.x = std::round(placed.rect.x + placed.padding.left) - placed.textScroll,
            .y = std::round(placed.rect.y + placed.padding.top)};
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
