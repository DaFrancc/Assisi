/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/DrawList.hpp>

#include <Assisi/Core/Assert.hpp>

#include <algorithm>

namespace Assisi::Mondrian
{

QuadInstance &QuadBuilder::Instance()
{
    ASSISI_ASSERT(!_list->_finalized, "a quad changed after its DrawList was finalized");
    return _list->_instances[_index];
}

QuadBuilder &QuadBuilder::Fill(const Math::Color4<Math::ColorSpace::Srgb> &color)
{
    Instance().color = color;
    return *this;
}

QuadBuilder &QuadBuilder::Border(float width, const Math::Color4<Math::ColorSpace::Srgb> &color)
{
    QuadInstance &instance = Instance();
    instance.borderWidth   = width;
    instance.borderColor   = color;
    return *this;
}

QuadBuilder &QuadBuilder::Corners(float radius, CornerStyle style)
{
    for (uint32_t corner = 0; corner < static_cast<uint32_t>(Corner::Count); ++corner)
    {
        CornerAt(static_cast<Corner>(corner), radius, style);
    }
    return *this;
}

QuadBuilder &QuadBuilder::CornerAt(Corner corner, float radius, CornerStyle style)
{
    QuadInstance &instance                                  = Instance();
    instance.cornerRadius[static_cast<std::size_t>(corner)] = radius;
    instance.cornerStyles = PackCornerStyle(instance.cornerStyles, corner, style);
    return *this;
}

QuadBuilder &QuadBuilder::Clip(const Rect &clip)
{
    Instance().clip = clip;
    return *this;
}

QuadBuilder &QuadBuilder::Texture(TextureId texture, const Rect &uv)
{
    QuadInstance &instance            = Instance();
    instance.uv                       = uv;
    instance.kind                     = static_cast<uint32_t>(QuadKind::Image);
    _list->_bindings[_index].texture = texture;
    return *this;
}

QuadBuilder &QuadBuilder::Kind(QuadKind kind)
{
    Instance().kind = static_cast<uint32_t>(kind);
    return *this;
}

QuadBuilder &QuadBuilder::Material(MaterialId material)
{
    (void)Instance();
    _list->_bindings[_index].material = material;
    return *this;
}

QuadBuilder &QuadBuilder::Mask(MaskId mask)
{
    (void)Instance();
    _list->_bindings[_index].mask = mask;
    return *this;
}

QuadBuilder DrawList::Quad(const Rect &rect)
{
    ASSISI_ASSERT(!_finalized, "a quad added to a finalized DrawList");
    _instances.push_back(QuadInstance{.rect = rect});
    _bindings.push_back(Binding{});
    return QuadBuilder(*this, _instances.size() - 1);
}

namespace
{

/// Whether any part of @p quad has area and lies inside its clip.
bool CanBeSeen(const QuadInstance &quad)
{
    const Rect &rect = quad.rect;
    const Rect &clip = quad.clip;
    const float left   = std::max(rect.x, clip.x);
    const float top    = std::max(rect.y, clip.y);
    const float right  = std::min(rect.x + rect.width, clip.x + clip.width);
    const float bottom = std::min(rect.y + rect.height, clip.y + clip.height);
    return right > left && bottom > top;
}

} // namespace

void DrawList::Finalize()
{
    ASSISI_ASSERT(!_finalized, "a DrawList finalized twice");

    // Compact in place, keeping order: the list is back to front.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < _instances.size(); ++i)
    {
        if (!CanBeSeen(_instances[i]))
        {
            continue;
        }
        _instances[kept] = _instances[i];
        _bindings[kept]  = _bindings[i];
        ++kept;
    }
    _instances.resize(kept);
    _bindings.resize(kept);

    // Only neighbours merge: joining two runs of one texture across a run of
    // another would draw that other run out of order.
    _entries.clear();
    for (std::size_t i = 0; i < kept; ++i)
    {
        const Binding &binding = _bindings[i];
        if (!_entries.empty())
        {
            DrawEntry &last = _entries.back();
            if (last.texture == binding.texture && last.material == binding.material && last.mask == binding.mask)
            {
                ++last.instanceCount;
                continue;
            }
        }
        _entries.push_back(DrawEntry{.firstInstance = static_cast<uint32_t>(i),
                                     .instanceCount = 1,
                                     .texture       = binding.texture,
                                     .material      = binding.material,
                                     .mask          = binding.mask});
    }

    _finalized = true;
}

void DrawList::Clear()
{
    _instances.clear();
    _bindings.clear();
    _entries.clear();
    _finalized = false;
}

} // namespace Assisi::Mondrian
