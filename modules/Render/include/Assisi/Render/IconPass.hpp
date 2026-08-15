/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file IconPass.hpp
/// @brief World-space, camera-facing entity icons for the editor.

#include <cstdint>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Render
{

/// @brief World-space edge length of an entity-icon billboard. Exposed so editor
/// picking can size the clickable area to exactly the drawn quad.
inline constexpr float kEntityIconWorldSize = 0.5f;

/// @brief Draws a fixed-size world-space billboard (a camera-facing textured
/// quad) at each supplied position — the editor's marker for entities that have
/// a placement (Transform) but no mesh to draw, e.g. lights or empties.
///
/// The quad is sized in WORLD units and always faces the camera, so perspective
/// and distance change how much screen space it takes (a far icon is small), just
/// like any object in the scene. It is depth-tested against the scene so it is
/// correctly occluded by geometry ("exists in the world"), but does not write
/// depth. The caller decides when to draw it (the editor skips it during play).
class IconPass
{
public:
    /// @param sceneFramebufferInfo  Format/samples of the scene framebuffer the
    ///        billboards composite into (must include the scene depth target).
    /// @param iconAssetPath  Virtual path of the icon image; a magenta placeholder
    ///        is substituted if it cannot be loaded, so the pass still works before
    ///        the art is supplied.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                                  const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                                  const std::string &iconAssetPath);

    /// @brief Rebuild the pipeline for a new scene render-target format (e.g. an
    /// MSAA toggle). Shaders, texture and binding set are reused. No-op (true)
    /// before Initialize().
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr; }

    /// @brief The icon texture, so another pass (e.g. the selection outline) can
    /// mask against the icon's artwork. Null before Initialize().
    [[nodiscard]] nvrhi::ITexture *IconTexture() const { return _icon.NativeTexture(); }

    /// @brief Draw one billboard per world position. @p viewProjection is the
    /// camera's projection*view; @p cameraRight / @p cameraUp are the camera's
    /// world-space basis (so the quads face it). No-op if not initialised or the
    /// list is empty.
    void Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraRight,
              const glm::vec3 &cameraUp, std::span<const glm::vec3> positions);

private:
    [[nodiscard]] bool BuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo);

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingSetHandle _bindingSet;       // the icon texture + sampler
    nvrhi::GraphicsPipelineHandle _pipeline;

    Texture _icon;
};

} // namespace Assisi::Render
