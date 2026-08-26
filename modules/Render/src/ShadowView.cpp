/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

glm::vec4 ShadowViewUvScaleOffset(const ShadowView &view)
{
    if (view.targetResolution == 0)
    {
        return glm::vec4(1.f, 1.f, 0.f, 0.f);
    }
    const float target = static_cast<float>(view.targetResolution);
    return glm::vec4(static_cast<float>(view.rect.width) / target, static_cast<float>(view.rect.height) / target,
                     static_cast<float>(view.rect.x) / target, static_cast<float>(view.rect.y) / target);
}

nvrhi::Viewport ShadowViewViewport(const ShadowView &view)
{
    const auto x = static_cast<float>(view.rect.x);
    const auto y = static_cast<float>(view.rect.y);
    return nvrhi::Viewport(x, x + static_cast<float>(view.rect.width), y, y + static_cast<float>(view.rect.height),
                           0.f, 1.f);
}

ShadowViewGpu PackShadowView(const ShadowView &view)
{
    ShadowViewGpu packed;
    packed.viewProjection = view.viewProjection;
    packed.uvScaleOffset = ShadowViewUvScaleOffset(view);
    packed.params = glm::vec4(view.depthBias, view.normalOffset, view.filterTapStepUv,
                              static_cast<float>(view.arraySlice));
    return packed;
}

ShadowView CascadeShadowView(const ShadowCascade &cascade, std::uint32_t slice, const SunShadowSettings &settings)
{
    const SunShadowSettings safe = Sanitized(settings);

    ShadowView view;
    view.viewProjection = cascade.viewProjection;
    view.rect = ShadowViewRect{.x = 0, .y = 0, .width = safe.resolution, .height = safe.resolution};
    view.targetResolution = safe.resolution;
    view.arraySlice = slice;
    view.depthBias = CascadeDepthBiasNdc(cascade, safe);
    view.normalOffset = CascadeNormalOffsetWorld(cascade, safe);
    view.filterTapStepUv = FilterTapStepUv(safe);
    return view;
}

} // namespace Assisi::Render
