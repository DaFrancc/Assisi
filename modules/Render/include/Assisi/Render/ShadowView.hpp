/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowView.hpp
/// @brief One shadow map view: where it draws, where in its target it lands,
/// and how a lookup into it is biased.
///
/// A cascade, a spot light's map and one face of a point light differ in how
/// their matrix is fitted and in nothing else. Each rasterizes the scene's
/// depth from somewhere, into some rectangle of some target, and is later
/// sampled with a bias scaled to how much world one of its texels covers. This
/// is that common shape, so the depth renderer takes a view rather than a
/// cascade index and the same code path serves all three.
///
/// The rectangle is carried in texels rather than as a UV sub-rect, because
/// texels are what the rasterizer's viewport wants and what an allocator hands
/// out. The UV form the shader samples with is derived from it, so the two can
/// never disagree.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief A rectangle of a shadow-map target, in texels.
struct ShadowViewRect
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// @brief Where one shadow map view renders and how it is sampled.
struct ShadowView
{
    /// World space to this view's clip space. What the depth pass draws with
    /// and what a lookup transforms by.
    glm::mat4 viewProjection{1.f};

    /// The part of @ref targetResolution this view owns. A cascade owns its
    /// whole slice; an atlas tile owns a rectangle of the shared texture.
    ShadowViewRect rect;

    /// Width and height of the target the rect is measured in. Square, because
    /// both the cascade array and the atlas are.
    std::uint32_t targetResolution = 0;

    /// Which array slice of the target. Cascades take one each; every atlas
    /// tile shares slice zero.
    std::uint32_t arraySlice = 0;

    /// Constant depth bias, in the [0, 1] depth a lookup compares against.
    /// Already scaled by this view's world-units-per-texel.
    float depthBias = 0.f;

    /// How far along the surface normal a sample is pushed, in world units.
    float normalOffset = 0.f;

    /// UV distance between adjacent filter taps, in this view's target.
    float filterTapStepUv = 0.f;
};

/// @brief The view's rectangle as a UV transform: xy scale, zw offset.
///
/// A lookup that lands at @c uv inside the view samples the target at
/// `uv * scale + offset`. A view owning its whole target gets (1, 1, 0, 0), so
/// the atlas case costs the same multiply-add the cascade case already pays.
///
/// A zero target resolution — an unfitted view — returns the identity rather
/// than dividing by it.
[[nodiscard]] glm::vec4 ShadowViewUvScaleOffset(const ShadowView &view);

/// @brief The rasterizer viewport and scissor that confine drawing to the
/// view's rectangle.
[[nodiscard]] nvrhi::Viewport ShadowViewViewport(const ShadowView &view);

/// @brief One view's record in the table the GPU reads, std430-packed.
///
/// The array slice rides in a float lane because that is the form the array
/// sampler takes it in; there is no integer conversion at the sample site.
struct ShadowViewGpu
{
    glm::mat4 viewProjection{1.f};
    /// xy = UV scale, zw = UV offset. See ShadowViewUvScaleOffset.
    glm::vec4 uvScaleOffset{1.f, 1.f, 0.f, 0.f};
    /// x = depth bias, y = normal offset, z = filter tap step, w = array slice.
    glm::vec4 params{0.f};
};
static_assert(sizeof(ShadowViewGpu) == 96, "ShadowViewGpu must match the shader's std430 array stride.");

/// @brief @p view in the layout the GPU table carries it in.
[[nodiscard]] ShadowViewGpu PackShadowView(const ShadowView &view);

/// @brief The view for one fitted cascade of the sun.
///
/// A cascade owns its whole array slice, so its rectangle is the full target
/// and its UV transform is the identity. The biases arrive already multiplied
/// by the cascade's world-units-per-texel, which is what lets one setting hold
/// across cascades whose texels differ by an order of magnitude.
[[nodiscard]] ShadowView CascadeShadowView(const ShadowCascade &cascade, std::uint32_t slice,
                                           const SunShadowSettings &settings);

} // namespace Assisi::Render
