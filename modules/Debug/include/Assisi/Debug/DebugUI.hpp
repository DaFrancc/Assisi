/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DebugUI.hpp
/// @brief Thin wrapper around Dear ImGui + GLFW/Vulkan backends.
///
/// Usage:
///   DebugUI::Initialize(window, vulkanContext);   // once
///   // per frame, after the frame's color/depth targets are cleared:
///   DebugUI::BeginFrame(frame);
///   ImGui::Begin("..."); ... ImGui::End();
///   DebugUI::EndFrame(frame);
///   // on shutdown:
///   DebugUI::Shutdown();
///
/// Multi-viewport (floating ImGui windows outside the main window) is
/// deliberately not enabled — it would need per-viewport swapchains managed
/// by the Vulkan backend, out of scope for now. Docking is enabled.
///
/// @note Intentional static service: Dear ImGui is itself a process-global
/// (one implicit ImGui context, one set of backend statics), so wrapping this
/// in an instance would be false encapsulation over shared global state. There
/// is exactly one debug UI per process by construction.

#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <imgui.h>
#include <nvrhi/nvrhi.h>

namespace Assisi::Debug
{

class DebugUI
{
  public:
    /// @brief Initialises ImGui and attaches the GLFW + Vulkan backends.
    static void Initialize(const Window::WindowContext &window, Render::Vulkan::VulkanContext &vulkanContext);

    /// @brief Releases all ImGui resources.
    static void Shutdown();

    /// @brief Starts a new ImGui frame and guarantees the frame's render target
    /// is bound (via NVRHI's own tracked setGraphicsState path — see .cpp), so
    /// there's always something for ImGui to draw into even if the scene draws
    /// nothing this frame. Call before any ImGui:: calls, after clearing
    /// `frame`'s color/depth targets.
    static void BeginFrame(Render::RenderFrame &frame);

    /// @brief Renders the accumulated draw data into `frame`. Call after all
    /// ImGui:: calls for this frame (including the app's OnRender/OnImGui).
    static void EndFrame(Render::RenderFrame &frame);

    /// @brief Registers an NVRHI texture with the ImGui Vulkan backend and returns
    /// a cached ImTextureID for use with ImGui::Image(). Pass the result straight
    /// to ImGui::Image(id, size).
    ///
    /// The texture must sit in the ShaderResource state
    /// (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) whenever ImGui samples it —
    /// textures produced by Render::Texture already are. The registration
    /// (a descriptor set) is cached per texture pointer and released in Shutdown().
    ///
    /// @warning Keyed on the raw `ITexture*`, so an id is valid only while its
    /// texture is alive. Don't register a texture, free it, then register another
    /// (a reused address would return the stale id). Fine for long-lived UI images.
    /// @return A valid id, or ImTextureID_Invalid if @p texture is null or exposes
    ///         no VkImageView.
    static ImTextureID GetOrCreateTextureId(nvrhi::ITexture *texture);
};

} // namespace Assisi::Debug
