/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/HitTest.hpp>

#include <cstdint>
#include <span>

namespace Assisi::Mondrian
{
namespace
{

/// Whether @p point lies in @p rect, taking the right and bottom edges as outside
/// so that neighbours sharing an edge never both claim it.
bool Contains(const Rect &rect, Point point)
{
    return point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
}

/// Hits one laid-out tree. A class only so the recursion shares what every node reads.
class HitTester
{
  public:
    HitTester(const NodeTree &tree, const LayoutResult &layout, Point point)
        : _tree(tree), _slots(tree.Slots()), _layout(layout), _point(point)
    {
    }

    /// The reverse of drawing @p index: its floating children last to first,
    /// then its in-flow ones, then itself.
    NodeId Hit(uint32_t index) const
    {
        // The layout is a frame old, so a node made since has no place in it
        // and a slot reused since holds a different node than it placed.
        if (_layout.Get(_tree.IdOf(index)) == nullptr)
        {
            return {};
        }
        const Node &node = _slots[index];
        const LayoutNode &placed = _layout.nodes[index];
        // What a control draws over its own content — a scroll bar — is the
        // control's to be pressed, before anything underneath it.
        if (const WidgetType *widget = _tree.Widgets().Get(node.behaviour);
            widget != nullptr && widget->claims != nullptr)
        {
            const NodeId id = _tree.IdOf(index);
            const WidgetView view{
                .node = &node, .layout = &placed, .context = widget->context, .scale = _layout.scale, .id = id};
            if (widget->claims(view, _point) && Contains(placed.clip, _point))
            {
                return id;
            }
        }

        for (const bool floating : {true, false})
        {
            NodeId found;
            for (NodeId child = node.firstChild; child; child = _slots[child.index].nextSibling)
            {
                if (_slots[child.index].style.floating.enabled != floating)
                {
                    continue;
                }
                // Later siblings draw over earlier ones, so the last hit wins.
                if (const NodeId hit = Hit(child.index))
                {
                    found = hit;
                }
            }
            if (found)
            {
                return found;
            }
        }

        const bool stops = node.focusable || node.blocksPointer;
        if (stops && Contains(placed.rect, _point) && Contains(placed.clip, _point))
        {
            return _tree.IdOf(index);
        }
        return {};
    }

  private:
    const NodeTree &_tree;
    std::span<const Node> _slots;
    const LayoutResult &_layout;
    Point _point;
};

} // namespace

NodeId HitTest(const NodeTree &tree, const LayoutResult &layout, Point point)
{
    return HitTester(tree, layout, point).Hit(tree.Root().index);
}

} // namespace Assisi::Mondrian
