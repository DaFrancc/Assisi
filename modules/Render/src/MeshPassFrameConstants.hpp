/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshPassFrameConstants.hpp
/// @brief The mesh pass's per-frame constant block as the GPU sees it. Private
/// to the pass: MeshPass.cpp sizes the buffer from it, and
/// MeshPassFrameConstants.cpp fills it.

#include <array>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

/// @brief Per-frame data (view-projection + camera + cluster-grid parameters),
/// uploaded once per frame via MeshPass::UpdateFrameConstants into a constant
/// buffer. viewProjection leads so the vertex shader can form clip position from
/// each instance's world matrix. Mirrors the `uniform FrameConstants` block in
/// mesh/frame.glsl and mesh.vert.
struct MeshPassFrameConstants
{
    glm::mat4 viewProjection;
    glm::mat4 view;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4 screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    glm::uvec4 lightCounts;       // x = directional light count, y = debug view, zw unused
    /// World-space camera position, w unused. Derived once here rather than per
    /// fragment: it is constant across the frame, but view is a uniform, so the
    /// shader compiler cannot hoist the -transpose(mat3(view)) * view[3] out.
    glm::vec4 cameraPosition;
    /// Froxel lookup scale/bias, so mesh.frag's ClusterIndex() is FMAs and a
    /// single log instead of three divides and two logs:
    ///   xy = gridDim.xy / screenSize        (screen pixel -> cluster column/row)
    ///   z  = gridDim.z / log(farZ / nearZ)  (log-depth slice scale)
    ///   w  = -z * log(nearZ)                (matching bias)
    /// slice = log(|viewZ|) * z + w, which is gridDim.z * log(|viewZ|/nearZ) / log(farZ/nearZ).
    glm::vec4 clusterScale;
    /// The frame's indirect term, as its provider answered it (see
    /// Render::IndirectConstants): rgb = the radiance a surface facing straight
    /// up receives, then the same facing straight down, w unused in both.
    glm::vec4 indirectSky;
    glm::vec4 indirectGround;
    /// x = 1 while a prefiltered environment answers the specular half, y =
    /// its last mip, z = 1 while screen-space occlusion ran this frame, w unused.
    glm::vec4 indirectSpecular;

    /// x = cascade count (0 = nothing shadows this frame, and the shader takes
    /// no lookup at all), y = which directional light the cascades belong to,
    /// z = ShadowFilter, w = ShadowDebugView.
    glm::uvec4 shadowCounts;
    /// x = the ShadowFilter the atlas is sampled with, y = 1 while any local
    /// light holds a tile and the two light loops should look one up, z = 1
    /// while those lookups take the contact-hardening path, w unused.
    ///
    /// The biases and the tap step are deliberately absent: a demoted tile is
    /// biased for the smaller map it got, so those belong per view rather than
    /// per frame, and they ride in the shadow view table with the matrix.
    glm::uvec4 localShadowCounts;
    /// x = one texel of the map, in UV, which is the step between PCF taps in
    /// every cascade whose texels are small enough,
    /// y = the fraction of each cascade spent fading into the next,
    /// z = the penumbra cap over the filter's radius, which the shader divides
    /// by a cascade's depth range to get the widest step that cascade may use,
    /// w = the sun's penumbra per world unit of blocker distance over the same
    /// radius, which the shader multiplies by the distance it reads out of the
    /// map to get the step the scene actually calls for. The cascade's depth
    /// range cancels in that product, which is why no cascade term appears.
    glm::vec4 shadowParams;
    /// The sun's contact-hardening constants (see SunPcssConstants): x = its
    /// penumbra UV per unit of depth, y = the reach cap in UV, z = one texel of
    /// depth, w = kMaxPenumbraWorld. All zero while the sun takes the fixed
    /// kernel, and x is what the shader tests: the sun's own figure is never
    /// zero, so zero can only mean off. A local light's ride in its view instead.
    ///
    /// The world cap rides here rather than being read off shadowParams.z,
    /// which is quoted over the selected filter's radius; this path's kernel is
    /// always the Vogel disk.
    glm::vec4 shadowPcss;
    /// One record per cascade: x = the view-space distance it ends at (what the
    /// shader selects on), y = its constant depth bias already in the [0, 1]
    /// depth the shader compares in, z = its normal offset in world units,
    /// w = the world span of its depth range, which is also how wide its ortho
    /// box is, and what the tap-step cap above is divided by to reach a step.
    /// Both biases are scaled CPU-side by that cascade's texel size,
    /// which is why one setting holds across cascades whose texels differ by an
    /// order of magnitude (see CascadeDepthBiasNdc).
    ///
    /// One array of records rather than three arrays of scalars: std140 pads a
    /// float array to a 16-byte stride anyway, so three of them would cost the
    /// same and read as three places to keep in step instead of one. Entries
    /// past the live count are unread.
    std::array<glm::vec4, kMaxShadowCascades> shadowCascade;
    /// World space to each cascade's clip space. Last because it is the only
    /// member whose size is not one lane, and appending keeps every offset
    /// above it fixed.
    std::array<glm::mat4, kMaxShadowCascades> shadowViewProjection;
};

// std140, not std430 — this is a uniform block. The two agree on everything this
// struct contains, because every member is a vec4/uvec4/mat4 lane or an array of
// one, and those take a 16-byte offset and stride under both rules. That is not
// an accident to be preserved by luck: it is why nothing here is a bare float or
// a uint, and the offsets below are what keeps it true.
//
// GLM's default gentypes are 4-aligned, so the C++ side packs these tightly and
// every member still lands on a lane boundary because every member is a whole
// number of lanes wide. Insert anything narrower and the two layouts part
// company silently — which is what these lines exist to prevent.
ASSISI_GPU_LAYOUT(MeshPassFrameConstants);
ASSISI_GPU_FIRST_FIELD(MeshPassFrameConstants, viewProjection);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, view, viewProjection);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, gridDim, view);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, screenSizeNearFar, gridDim);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, lightCounts, screenSizeNearFar);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, cameraPosition, lightCounts);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, clusterScale, cameraPosition);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, indirectSky, clusterScale);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, indirectGround, indirectSky);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, indirectSpecular, indirectGround);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, shadowCounts, indirectSpecular);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, localShadowCounts, shadowCounts);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, shadowParams, localShadowCounts);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, shadowPcss, shadowParams);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, shadowCascade, shadowPcss);
ASSISI_GPU_FIELD_AFTER(MeshPassFrameConstants, shadowViewProjection, shadowCascade);
ASSISI_GPU_NO_TAIL_PADDING(MeshPassFrameConstants, shadowViewProjection);

} // namespace Assisi::Render
