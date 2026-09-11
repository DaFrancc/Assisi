/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SsaoPass.hpp
/// @brief Screen-space ambient occlusion: from the depth prepass's depth to one
/// visible fraction per pixel, blurred, for mesh.frag's indirect term.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/SsaoSettings.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Assisi::Render
{

/// @brief Three fullscreen steps over single-sampled targets: the hemisphere
/// test (ssao.frag) and the two axes of a depth-aware blur (ssao_blur.frag),
/// both reading SceneDistancePass's distance per pixel.
///
/// **Nothing exists until a frame asks for it.** Initialize builds the pipelines
/// and nothing else; the targets arrive with Configure and leave with Release,
/// so a renderer with occlusion switched off holds no texture of it at all.
class SsaoPass
{
public:
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Shared fullscreen-triangle vertex stage.
        std::string vertexShaderSpvPath;
        std::string occlusionShaderSpvPath;
        std::string blurShaderSpvPath;
    };

    /// @brief Loads the shaders and builds every pipeline. Allocates no target.
    [[nodiscard]] bool Initialize(const InitParams &params);

    [[nodiscard]] bool IsValid() const { return _occlusionPipeline != nullptr; }

    /// @brief Hold targets of @p width x @p height reading @p distance — the
    /// scene's distance per pixel, from SceneDistancePass — rebuilding only what a
    /// changed size or a changed input invalidates.
    /// @return false if a target could not be allocated; the pass then holds
    /// nothing.
    [[nodiscard]] bool Configure(uint32_t width, uint32_t height, nvrhi::ITexture *distance);

    /// @brief Free every target and every set over them.
    void Release();

    /// @brief Everything one run reads besides the scene's distance.
    struct Frame
    {
        glm::mat4 projection{1.f};
        float farZ = 0.f;
        SsaoSettings settings;
    };

    /// @brief Record the three steps into @p commandList. The result is in
    /// OcclusionTexture() when it returns.
    /// @pre Configure succeeded, and the distance it was given holds this frame's.
    void Render(nvrhi::ICommandList *commandList, const Frame &frame) const;

    /// @brief The blurred visible fraction, one per pixel; null while nothing
    /// is held. Its handle survives every Render, and changes only with a
    /// Configure that reallocates.
    [[nodiscard]] nvrhi::ITexture *OcclusionTexture() const { return _occlusion; }

private:
    enum class Step : uint32_t
    {
        Occlusion,
        BlurHorizontal,
        BlurVertical,
        Count
    };
    static constexpr uint32_t kStepCount = static_cast<uint32_t>(Step::Count);

    [[nodiscard]] bool CreateTargets();
    void CreateBindingSets();
    void RunStep(nvrhi::ICommandList *commandList, Step step) const;

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::BindingLayoutHandle _singleInputLayout;
    nvrhi::BindingLayoutHandle _blurLayout;
    nvrhi::GraphicsPipelineHandle _occlusionPipeline;
    nvrhi::GraphicsPipelineHandle _blurPipeline;
    nvrhi::BufferHandle _constants;

    uint32_t _width = 0;
    uint32_t _height = 0;
    // SceneDistancePass's target. Not owned.
    nvrhi::ITexture *_distance = nullptr;

    // The raw occlusion, then the half-blurred one. The vertical pass writes
    // back into _occlusion, so the texture mesh.frag reads is one handle for the
    // life of a configuration.
    nvrhi::TextureHandle _occlusion;
    nvrhi::TextureHandle _blurScratch;
    nvrhi::FramebufferHandle _occlusionFramebuffer;
    nvrhi::FramebufferHandle _blurScratchFramebuffer;

    std::array<nvrhi::BindingSetHandle, kStepCount> _bindingSets;

    // The kernel for the last sample count rendered with.
    mutable std::vector<glm::vec3> _kernel;
};

} // namespace Assisi::Render
