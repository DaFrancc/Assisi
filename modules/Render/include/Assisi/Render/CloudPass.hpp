/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CloudPass.hpp
/// @brief Draws the clouds each frame into the two maps everything else reads:
/// the sky map, which the sky pass and the reflection probe composite over the
/// sky, and the shadow map, which the lit pass dims the directional light by.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/CloudQuality.hpp>
#include <Assisi/Render/Clouds.hpp>
#include <Assisi/Render/ComputeShader.hpp>
#include <Assisi/Render/Fog.hpp>
#include <Assisi/Render/GpuLayout.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief Mirrors cloud_map.comp's constant block.
struct CloudMapConstants
{
    CloudConstants clouds;
    /// The level's haze in front of the clouds, and in its scatter the sky they
    /// are seen against.
    FogConstants fog;
};

ASSISI_GPU_LAYOUT(CloudMapConstants);
ASSISI_GPU_FIRST_FIELD(CloudMapConstants, clouds);
ASSISI_GPU_FIELD_AFTER(CloudMapConstants, fog, clouds);
ASSISI_GPU_NO_TAIL_PADDING(CloudMapConstants, fog);

/// @brief Mirrors cloud_shadow.comp's constant block.
struct CloudShadowMapConstants
{
    CloudConstants clouds;
    /// xy = the square's centre across, camera-relative; z = its width in
    /// metres; w = steps toward the light.
    glm::vec4 area{0.0f};
    /// xyz = unit direction to the light; see CloudShadowLight.
    glm::vec4 light{0.0f};
};

ASSISI_GPU_LAYOUT(CloudShadowMapConstants);
ASSISI_GPU_FIRST_FIELD(CloudShadowMapConstants, clouds);
ASSISI_GPU_FIELD_AFTER(CloudShadowMapConstants, area, clouds);
ASSISI_GPU_FIELD_AFTER(CloudShadowMapConstants, light, area);
ASSISI_GPU_NO_TAIL_PADDING(CloudShadowMapConstants, light);

/// @brief The clouds' compute passes and the textures they fill.
///
/// **Nothing exists until a level asks for clouds.** Initialize builds the
/// pipelines and nothing else; Configure allocates the noise and the two maps,
/// and Release frees them, so a level without clouds, or a player who turned
/// them off, holds no texture of them at all.
///
/// The noise is drawn on the GPU in the first frame after Configure, once, and
/// kept until Release.
class CloudPass
{
public:
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        std::string noiseSpvPath;
        std::string noiseDownsampleSpvPath;
        std::string weatherSpvPath;
        std::string mapSpvPath;
        std::string mapResolveSpvPath;
        std::string shadowSpvPath;
    };

    [[nodiscard]] bool Initialize(const InitParams &params);
    [[nodiscard]] bool IsValid() const { return _map.IsValid(); }

    /// @brief Hold textures for @p quality, reallocating the sky map if its
    /// resolution changed.
    /// @return false if a texture could not be allocated; the pass then holds
    /// nothing.
    [[nodiscard]] bool Configure(const CloudQuality &quality);

    /// @brief Free every texture and binding set.
    void Release();

    /// @brief Record this frame's clouds: the noise first if it has not been
    /// drawn, then the weather map, the shadow map and the sky map.
    /// @pre Configure succeeded.
    void Render(nvrhi::ICommandList *commandList, const CloudMapConstants &map,
                const CloudShadowMapConstants &shadow);

    /// @brief The sky map, or null before Configure.
    [[nodiscard]] nvrhi::ITexture *MapTexture() const { return _mapTexture; }
    /// @brief The shadow map, or null before Configure.
    [[nodiscard]] nvrhi::ITexture *ShadowTexture() const { return _shadowTexture; }
    /// @brief The quality the textures are held for.
    [[nodiscard]] const CloudQuality &Quality() const { return _quality; }

private:
    /// One noise texture and a binding set per mip: the first for drawing it,
    /// the rest for averaging each mip from the one above.
    struct Noise
    {
        nvrhi::TextureHandle texture;
        std::vector<nvrhi::BindingSetHandle> mipSets;
        uint32_t size = 0;
    };

    [[nodiscard]] bool CreateNoise(Noise &noise, uint32_t size, const char *name);
    [[nodiscard]] bool CreateMapBindingSets();
    void DrawNoise(nvrhi::ICommandList *commandList, const Noise &noise, CloudNoiseKind kind);

    nvrhi::IDevice *_device = nullptr;

    ComputeShader _noise;
    ComputeShader _downsample;
    ComputeShader _weather;
    ComputeShader _map;
    ComputeShader _mapResolve;
    ComputeShader _shadow;
    nvrhi::SamplerHandle _noiseSampler;
    nvrhi::BufferHandle _mapConstants;
    nvrhi::BufferHandle _shadowConstants;

    Noise _shape;
    Noise _detail;
    bool _noiseDrawn = false;

    /// The weather map, baked each frame for the shadow and the sky map to read.
    nvrhi::TextureHandle _weatherTexture;
    nvrhi::BindingSetHandle _weatherSet;
    /// What the march writes, before the resolve averages it into the map.
    nvrhi::TextureHandle _marchTexture;
    nvrhi::TextureHandle _mapTexture;
    nvrhi::TextureHandle _shadowTexture;
    nvrhi::BindingSetHandle _mapSet;
    nvrhi::BindingSetHandle _resolveSet;
    nvrhi::BindingSetHandle _shadowSet;
    CloudQuality _quality;
};

} // namespace Assisi::Render
