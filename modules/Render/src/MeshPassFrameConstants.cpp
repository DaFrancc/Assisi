/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include "MeshPassFrameConstants.hpp"

#include <cmath>
#include <limits>

#include <Assisi/Render/ClusterGrid.hpp>
#include <Assisi/Render/ShadowCascades.hpp>

namespace Assisi::Render
{
namespace
{
/// The nearest a near plane is taken to be for the cluster slice logs. Cameras
/// validate their own, so this only guards the log against a zero.
constexpr float kMinClusterNearZ = 0.001f;

/// How much deeper than the near plane a far plane at or before it is taken to
/// be, so the slice scale's log(far / near) stays positive.
constexpr float kFallbackFarOverNear = 2.f;
} // namespace

void MeshPass::UpdateFrameConstants(nvrhi::ICommandList *commandList, const FrameConstantsParams &params) const
{
    // A new frame has submitted nothing yet. Without this, a frame whose draw
    // path found nothing to submit would leave Redraw replaying the last one's.
    _lastSubmission = Submission::None;

    MeshPassFrameConstants constants;
    constants.viewProjection = params.viewProjection;
    constants.view = params.view;
    constants.gridDim = glm::uvec4(ClusterGrid::kNumX, ClusterGrid::kNumY, ClusterGrid::kNumZ, 0u);
    constants.screenSizeNearFar = glm::vec4(static_cast<float>(params.screenWidth),
                                            static_cast<float>(params.screenHeight), params.nearZ, params.farZ);
    constants.lightCounts =
        glm::uvec4(params.dirLightCount, static_cast<uint32_t>(params.debugView), 0u, 0u);

    // View is a rigid transform (View = R | t, t = -R * cameraPos), so the camera
    // position is -R^-1 * t, and R^-1 == transpose(R) because R is orthonormal.
    constants.cameraPosition =
        glm::vec4(-glm::transpose(glm::mat3(params.view)) * glm::vec3(params.view[3]), 0.f);

    // Guard the logs: a zero/negative near or far plane would make these inf/NaN
    // and poison every cluster lookup.
    const float safeNear = params.nearZ > 0.f ? params.nearZ : kMinClusterNearZ;
    const float safeFar = params.farZ > safeNear ? params.farZ : safeNear * kFallbackFarOverNear;
    const float logRatio = std::log(safeFar / safeNear);
    const float sliceScale = static_cast<float>(ClusterGrid::kNumZ) / logRatio;
    constants.clusterScale =
        glm::vec4(static_cast<float>(ClusterGrid::kNumX) / static_cast<float>(params.screenWidth),
                  static_cast<float>(ClusterGrid::kNumY) / static_cast<float>(params.screenHeight), sliceScale,
                  -sliceScale * std::log(safeNear));

    constants.indirectSky = glm::vec4(params.indirect.skyRadiance, 0.f);
    constants.indirectGround = glm::vec4(params.indirect.groundRadiance, 0.f);
    // z is not the provider's: occlusion is visibility, applied to whatever
    // radiance the provider answered, and rides in this lane only because the
    // lane belongs to the indirect term it darkens.
    constants.indirectSpecular = glm::vec4(params.indirect.specularEnvironment, params.indirect.specularMaxLod,
                                           params.screenOcclusion ? 1.f : 0.f, 0.f);

    // Shadows. A zero cascade count is the whole of "nothing shadows this frame"
    // as far as the shader is concerned: it takes no lookup, so an unshadowed
    // scene pays one comparison against a constant.
    const ShadowFrameData &shadows = params.shadows;
    const uint32_t cascadeCount = shadows.fit != nullptr ? shadows.fit->count : 0u;
    constants.shadowCounts = glm::uvec4(cascadeCount, shadows.sunLightIndex,
                                        static_cast<uint32_t>(shadows.settings.filter),
                                        static_cast<uint32_t>(shadows.debugView));
    // The local half switches on its own flag rather than on the cascade count:
    // a scene may have shadowed lamps and no sun, or a sun and no shadowed lamp,
    // and neither should pay for the other's lookup.
    constants.localShadowCounts = glm::uvec4(static_cast<uint32_t>(shadows.localSettings.filter),
                                             shadows.localActive ? 1u : 0u, shadows.localPcss ? 1u : 0u, 0u);
    for (uint32_t i = 0; i < kMaxShadowCascades; ++i)
    {
        constants.shadowCascade[i] = glm::vec4(0.f);
        constants.shadowViewProjection[i] = glm::mat4(1.f);
    }
    for (uint32_t i = 0; i < cascadeCount && i < kMaxShadowCascades; ++i)
    {
        const ShadowCascade &cascade = shadows.fit->cascades[i];
        // w is the cascade's depth range in world units, which is what turns the
        // [0, 1] depths the shader compares back into metres.
        constants.shadowCascade[i] = glm::vec4(cascade.splitFarView,
                                               CascadeDepthBiasNdc(cascade, shadows.settings),
                                               CascadeNormalOffsetWorld(cascade, shadows.settings),
                                               cascade.depthRange);
        constants.shadowViewProjection[i] = cascade.viewProjection;
    }
    // z is the penumbra cap divided by the filter's radius, so the shader can
    // turn it into a tap step with one divide by the cascade's own depth range —
    // which is the same 2r the cascade's box is wide. Infinite for a filter with
    // no kernel of its own, so the min below it never bites.
    const float radiusTaps = FilterRadiusTaps(shadows.settings.filter);
    const float cappedStepNumerator =
        radiusTaps > 0.f ? kMaxPenumbraWorld / radiusTaps : std::numeric_limits<float>::max();
    // The cascade's depth range cancels: a blocker distance read in the map's own
    // [0, 1] depth, times this, is already the UV step that gives the penumbra
    // the sun's angular radius calls for at that distance.
    const float contactStepNumerator =
        radiusTaps > 0.f ? kSunPenumbraPerWorldUnit / radiusTaps : std::numeric_limits<float>::max();
    constants.shadowParams = glm::vec4(ShadowTexelSizeUv(shadows.settings), shadows.settings.cascadeBlend,
                                       cappedStepNumerator, contactStepNumerator);

    constants.shadowPcss = glm::vec4(0.f);
    if (shadows.sunPcss)
    {
        const SunPcssConstants sunPcss = SunPcssFrameConstants(shadows.settings);
        constants.shadowPcss =
            glm::vec4(sunPcss.penumbraUvPerDepth, sunPcss.maxReachUv, sunPcss.texelDepth, kMaxPenumbraWorld);
    }

    commandList->writeBuffer(_frameConstantsBuffer, &constants, sizeof(constants));
}

} // namespace Assisi::Render
