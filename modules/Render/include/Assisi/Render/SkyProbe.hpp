/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyProbe.hpp
/// @brief The one global reflection probe: the sky, captured into a cube and
/// prefiltered so a surface of any roughness can read what it reflects.

#include <Assisi/Render/ComputeShader.hpp>
#include <Assisi/Render/EnvironmentSettings.hpp>
#include <Assisi/Render/SkyProbeInputs.hpp>
#include <Assisi/Render/SpecularIbl.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Render
{

class SkyPass;

/// @brief Bakes the sky into a GGX-prefiltered cube map.
///
/// A bake is two steps in one command list: the sky drawn six times into a
/// capture cube by SkyPass's own shader, and each mip of the prefiltered cube
/// filtered from it by sky_prefilter.comp. The split-sum table the shader
/// scales the result by describes the BRDF rather than a sky, and MeshPass
/// keeps it.
///
/// **Nothing exists until a sky asks for it.** Initialize builds the compute
/// pipeline and nothing else; the cubes arrive with Configure and leave with
/// Release, so a level with no sky, or a renderer with the probe switched off,
/// holds no texture of it at all.
///
/// A bake is kept until the sky changes enough to see (NeedsBake), so a still
/// sky costs one bake and then nothing.
class SkyProbe
{
public:
    /// @brief What the probe did most recently, for the panels.
    struct Stats
    {
        /// Bakes since the probe was last configured.
        uint32_t bakes = 0;
        /// Frames the current bake has been kept for.
        uint32_t ageFrames = 0;
        /// Whether the most recent frame baked.
        bool bakedThisFrame = false;
    };

    /// @brief Builds the prefilter pipeline from @p prefilterSpvPath. Allocates
    /// no texture.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const std::string &prefilterSpvPath);

    [[nodiscard]] bool IsValid() const { return _prefilter.IsValid(); }

    /// @brief Hold targets for @p settings, reallocating if the resolution
    /// differs from what is held. A change that alters what a bake produces
    /// discards the bake.
    /// @return false if a target could not be allocated; the probe then holds
    /// nothing and is not ready.
    [[nodiscard]] bool Configure(const EnvironmentSettings &settings);

    /// @brief Free both cubes and every framebuffer and set over them.
    void Release();

    /// @brief Whether a bake from @p inputs would differ visibly from the one
    /// held: nothing held yet, the clock cut since (@p jumpSerial moved), or
    /// the sky moved past the configured tolerance.
    [[nodiscard]] bool NeedsBake(const SkyProbeInputs &inputs, uint32_t jumpSerial) const;

    /// @brief Record a bake of @p inputs into @p commandList, capturing the sky
    /// through @p sky.
    /// @pre Configure succeeded.
    void Bake(nvrhi::ICommandList *commandList, SkyPass &sky, const SkyProbeInputs &inputs, uint32_t jumpSerial);

    /// @brief Count a frame that kept the bake rather than making one.
    void Keep();

    /// @brief Whether a bake is held, so the textures below mean something.
    [[nodiscard]] bool IsReady() const { return _baked; }

    [[nodiscard]] nvrhi::ITexture *SpecularTexture() const { return _prefiltered; }

    /// @brief The prefiltered cube's last mip, where roughness one is stored.
    [[nodiscard]] float MaxLod() const { return _mipCount > 0u ? static_cast<float>(_mipCount - 1u) : 0.0f; }

    [[nodiscard]] uint32_t Resolution() const { return _resolution; }
    [[nodiscard]] uint32_t MipCount() const { return _mipCount; }
    [[nodiscard]] const Stats &LastStats() const { return _stats; }

private:
    [[nodiscard]] bool AllocateTargets(uint32_t resolution);
    void Capture(nvrhi::ICommandList *commandList, SkyPass &sky, const SkyProbeInputs &inputs);
    void Prefilter(nvrhi::ICommandList *commandList);

    nvrhi::IDevice *_device = nullptr;
    ComputeShader _prefilter;
    nvrhi::SamplerHandle _sourceSampler;

    EnvironmentSettings _settings;
    uint32_t _resolution = 0;
    uint32_t _mipCount = 0;

    // The sky as drawn: one mip, rendered into face by face.
    nvrhi::TextureHandle _capture;
    std::array<nvrhi::FramebufferHandle, kCubeFaceCount> _faceFramebuffers;
    // The sky as reflected: mip m blurred for roughness m / (mips - 1).
    nvrhi::TextureHandle _prefiltered;
    // One per mip of _prefiltered, each writing that mip and reading _capture.
    std::vector<nvrhi::BindingSetHandle> _mipBindingSets;

    bool _baked = false;
    SkyProbeInputs _bakedInputs;
    uint32_t _bakedJumpSerial = 0;
    Stats _stats;
};

} // namespace Assisi::Render
