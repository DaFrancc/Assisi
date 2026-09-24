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

/// How much of its colour a field's placeholder keeps: faint enough to read as
/// a prompt rather than as text, and solid enough to read at all. The field's
/// own, until themes decide it.
constexpr float kPlaceholderOpacity = 0.45f;

/// Draws one laid-out tree. A class only so the recursion shares what every
/// node reads.
class Drawer
{
  public:
    Drawer(const NodeTree &tree, const LayoutResult &layout, DrawList &list, TextureId fontAtlas)
        : _tree(tree), _slots(tree.Slots()), _layout(layout), _list(list), _fontAtlas(fontAtlas)
    {
    }

    /// Which node is pressed, hovered or focused, for the controls that show it.
    void SetInteraction(const Interaction &interaction) { _interaction = &interaction; }

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
        const float radius = result.cornerRadius;
        _list.SetDefaultClip(result.clip);

        if (style.background.a > 0.f || result.borderWidth > 0.f)
        {
            // Whole device pixels, and never thinner than one, so a line stays
            // crisp and present at any scale.
            const float border =
                result.borderWidth > 0.f ? std::max(kMinBorderDevicePixels, std::round(result.borderWidth)) : 0.f;
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
        const WidgetType *widget = _tree.Widgets().Get(node.behaviour);

        // Behind the words: a selection highlight marks text rather than
        // covering it.
        if (widget != nullptr && widget->underlay != nullptr)
        {
            widget->underlay(ViewOf(index, result, *widget), _list);
        }
        if (result.text != LayoutNode::kNoText)
        {
            // A placeholder is faint, so that words standing in for text a
            // reader has not typed are not mistaken for words they have.
            Math::Color4<Math::ColorSpace::Srgb> ink = style.textColor;
            if (result.placeholder)
            {
                ink.a *= kPlaceholderOpacity;
            }
            DrawGlyphs(_list, _layout.texts[result.text], _fontAtlas, TextOrigin(result), ink);
        }

        // The control's own parts — a slider's thumb, a toggle's knob, a
        // caret — over the node they are on and under whatever it contains.
        if (widget != nullptr && widget->draw != nullptr)
        {
            widget->draw(ViewOf(index, result, *widget), _list);
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
    /// What the control on @p index sees of its node this frame.
    [[nodiscard]] WidgetView ViewOf(uint32_t index, const LayoutNode &result, const WidgetType &widget) const
    {
        const NodeId id = _tree.IdOf(index);
        WidgetView view;
        view.node = &_slots[index];
        view.layout = &result;
        view.text = result.text < _layout.texts.size() ? &_layout.texts[result.text] : nullptr;
        view.context = widget.context;
        view.scale = _layout.scale;
        view.id = id;
        view.focused = _interaction != nullptr && _interaction->focused == id;
        view.pressed = _interaction != nullptr && _interaction->pressed == id;
        view.hovered = _interaction != nullptr && _interaction->hovered == id;
        return view;
    }

    const NodeTree &_tree;
    std::span<const Node> _slots;
    const LayoutResult &_layout;
    DrawList &_list;
    const Interaction *_interaction = nullptr;
    TextureId _fontAtlas;
};

} // namespace

void DrawTree(const NodeTree &tree, const LayoutResult &layout, DrawList &list, TextureId fontAtlas,
              const Interaction &interaction)
{
    if (tree.Root().index < layout.nodes.size())
    {
        Drawer drawer(tree, layout, list, fontAtlas);
        drawer.SetInteraction(interaction);
        drawer.DrawNode(tree.Root().index);
    }
    list.SetDefaultClip(kNoClip);
}

} // namespace Assisi::Mondrian
