/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Draw.hpp>

#include <algorithm>
#include <cmath>

namespace Assisi::Mondrian
{
namespace
{

/// An image draws its texture's own colours.
constexpr Math::Color4<Math::ColorSpace::Srgb> kUntinted{1.f, 1.f, 1.f, 1.f};

/// Draws one laid-out tree. A class only so the recursion shares what every
/// node reads.
class Drawer
{
  public:
    Drawer(const NodeTree &tree, const LayoutResult &layout, DrawList &list, TextureId fontAtlas)
        : _slots(tree.Slots()), _layout(layout), _list(list), _fontAtlas(fontAtlas)
    {
    }

    /// @p index, then its in-flow children, then its floating ones.
    void DrawNode(uint32_t index)
    {
        const LayoutNode &result = _layout.nodes[index];
        if (!result.placed)
        {
            return;
        }
        const Node &node = _slots[index];
        const Style &style = node.style;
        const float scale = _layout.scale;
        const float radius = style.cornerRadius * scale;
        _list.SetDefaultClip(result.clip);

        if (style.background.a > 0.f || style.borderWidth > 0.f)
        {
            // Whole device pixels, and never thinner than one, so a line stays
            // crisp and present at any scale.
            const float border =
                style.borderWidth > 0.f ? std::max(kMinBorderDevicePixels, std::round(style.borderWidth * scale)) : 0.f;
            _list.Quad(result.rect)
                .Fill(style.background)
                .Border(border, style.borderColor)
                .Corners(radius, style.cornerStyle);
        }
        if (node.hasImage)
        {
            _list.Quad(result.rect)
                .Fill(kUntinted)
                .Texture(node.image, node.imageUv)
                .Corners(radius, style.cornerStyle);
        }
        if (result.text != LayoutNode::kNoText)
        {
            // Whole pixels, so the text layout's own whole-pixel lines land on them.
            const Point origin{.x = std::round(result.rect.x + style.padding.left * scale),
                               .y = std::round(result.rect.y + style.padding.top * scale)};
            DrawGlyphs(_list, _layout.texts[result.text], _fontAtlas, origin, style.textColor);
        }

        for (const bool floating : {false, true})
        {
            for (NodeId child = node.firstChild; child; child = _slots[child.index].nextSibling)
            {
                if (_slots[child.index].style.floating.enabled == floating)
                {
                    DrawNode(child.index);
                }
            }
        }
    }

  private:
    std::span<const Node> _slots;
    const LayoutResult &_layout;
    DrawList &_list;
    TextureId _fontAtlas;
};

} // namespace

void DrawTree(const NodeTree &tree, const LayoutResult &layout, DrawList &list, TextureId fontAtlas)
{
    if (tree.Root().index < layout.nodes.size())
    {
        Drawer(tree, layout, list, fontAtlas).DrawNode(tree.Root().index);
    }
    list.SetDefaultClip(kNoClip);
}

} // namespace Assisi::Mondrian
