/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Draw.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Assisi::Mondrian
{
namespace
{

/// Where between its two ends the middle of a span is: a contained picture
/// sits there, and a covering one loses as much off each side.
constexpr float kMiddle = 0.5f;

/// The most a nine-slice inset can say, in whole pixels.
constexpr float kMaxSliceInset = static_cast<float>(kSliceInsetMask);

/// Where a picture lands in the space it fills, and the part of it that shows.
struct Placement
{
    Rect rect;
    Rect uv;
};

/// @p sprite placed in @p box as @p fit says. A picture of no known size
/// stretches, having no shape to keep.
Placement Fitted(const Sprite &sprite, const Rect &box, ImageFit fit)
{
    if (fit == ImageFit::Stretch || sprite.width == 0 || sprite.height == 0 || box.width <= 0.f || box.height <= 0.f)
    {
        return {.rect = box, .uv = sprite.uv};
    }
    const float width = static_cast<float>(sprite.width);
    const float height = static_cast<float>(sprite.height);
    const float scaleX = box.width / width;
    const float scaleY = box.height / height;
    if (fit == ImageFit::Contain)
    {
        const float scale = std::min(scaleX, scaleY);
        const float shownWidth = width * scale;
        const float shownHeight = height * scale;
        return {.rect = {.x = box.x + ((box.width - shownWidth) * kMiddle),
                         .y = box.y + ((box.height - shownHeight) * kMiddle),
                         .width = shownWidth,
                         .height = shownHeight},
                .uv = sprite.uv};
    }
    // Cover: the larger scale fills both ways, and the share of the picture
    // that fits shows from its middle.
    const float scale = std::max(scaleX, scaleY);
    const float shownX = box.width / (width * scale);
    const float shownY = box.height / (height * scale);
    const Rect &uv = sprite.uv;
    return {.rect = box,
            .uv = {.x = uv.x + (uv.width * (1.f - shownX) * kMiddle),
                   .y = uv.y + (uv.height * (1.f - shownY) * kMiddle),
                   .width = uv.width * shownX,
                   .height = uv.height * shownY}};
}

/// A whole number of pixels that fits an inset.
uint16_t Inset(float pixels)
{
    return static_cast<uint16_t>(std::clamp(pixels, 0.f, kMaxSliceInset));
}

/// A nine-slice's two insets on one axis of a quad @p length long, from its
/// borders @p start and @p end in texels at @p scale device pixels to one:
/// whole pixels, shrunk in proportion where the two would overlap, so a small
/// frame keeps its shape rather than its corners crossing.
std::array<uint16_t, 2> InsetsAlong(uint32_t start, uint32_t end, float scale, float length)
{
    float first = std::round(static_cast<float>(start) * scale);
    float second = std::round(static_cast<float>(end) * scale);
    if (first + second > length)
    {
        const float shrink = std::max(length, 0.f) / (first + second);
        first = std::floor(first * shrink);
        second = std::floor(second * shrink);
    }
    return {Inset(first), Inset(second)};
}

/// How much its padding leaves of @p placed: where a node's picture goes.
Rect InsidePadding(const LayoutNode &placed)
{
    const Rect &rect = placed.rect;
    const Insets &padding = placed.padding;
    return {.x = rect.x + padding.left,
            .y = rect.y + padding.top,
            .width = std::max(0.f, rect.width - padding.left - padding.right),
            .height = std::max(0.f, rect.height - padding.top - padding.bottom)};
}

/// How much its colour a field's placeholder keeps of its colour a field's placeholder keeps: faint enough to read as
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
        if (node.hasBackgroundImage)
        {
            DrawSprite(node.background, Placement{.rect = result.rect, .uv = node.background.uv}, true, style, radius);
        }
        if (node.hasImage)
        {
            DrawSprite(node.image, Fitted(node.image, InsidePadding(result), style.fit),
                       style.fit == ImageFit::Stretch, style, radius);
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
    /// @p sprite where @p placed says, tinted and shaped as @p style says, with
    /// corners of @p radius. Its borders slice it only when @p stretched: a
    /// picture fitted to keep its shape has no stretching for its corners to
    /// stay out of.
    void DrawSprite(const Sprite &sprite, const Placement &placed, bool stretched, const Style &style, float radius)
    {
        QuadBuilder quad = _list.Quad(placed.rect)
                               .Fill(style.tint)
                               .Texture(sprite.texture, placed.uv)
                               .Corners(radius, style.cornerStyle);
        if (!stretched || !sprite.borders.Any())
        {
            return;
        }
        const SpriteBorders &borders = sprite.borders;
        const float scale = _layout.scale;
        const std::array<uint16_t, 2> across = InsetsAlong(borders.left, borders.right, scale, placed.rect.width);
        const std::array<uint16_t, 2> down = InsetsAlong(borders.top, borders.bottom, scale, placed.rect.height);
        quad.NineSlice({.left = across[0], .top = down[0], .right = across[1], .bottom = down[1]},
                       {.left = Inset(static_cast<float>(borders.left)),
                        .top = Inset(static_cast<float>(borders.top)),
                        .right = Inset(static_cast<float>(borders.right)),
                        .bottom = Inset(static_cast<float>(borders.bottom))});
    }

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
