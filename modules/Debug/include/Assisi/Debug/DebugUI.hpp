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

#include <cstddef>

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
    /// textures produced by Render::Texture already are.
    ///
    /// Registrations are cached per texture pointer and reclaimed automatically:
    /// each call marks the texture as used this frame, and BeginFrame frees the
    /// descriptor sets of any texture that hasn't been requested for a few frames.
    /// So the live registration count tracks what's actually on screen (call it
    /// every frame you draw the image) rather than growing for the whole session —
    /// re-requesting a reclaimed texture simply re-registers it.
    ///
    /// @warning Keyed on the raw `ITexture*`. Because reclamation is deferred a few
    /// frames (to stay clear of in-flight GPU work), freeing a texture and
    /// registering a different one at the same address within that window could
    /// still return the stale id. Stop requesting a texture before you free it and
    /// this cannot happen.
    /// @return A valid id, or ImTextureID_Invalid if @p texture is null or exposes
    ///         no VkImageView.
    static ImTextureID GetOrCreateTextureId(nvrhi::ITexture *texture);

    /// @brief Drops the cached ImGui registration for @p texture, if any, and frees
    /// its descriptor set now instead of waiting for the deferred sweep. Call this
    /// right before freeing a texture you have shown, so a different texture later
    /// allocated at the same address can't collide with a stale id (the hazard
    /// GetOrCreateTextureId warns about). No-op if the texture was never registered.
    ///
    /// @warning Frees the descriptor set immediately, so the caller must guarantee
    /// no in-flight frame still references it — call it only after a
    /// device waitForIdle (as AssetCache::ClearThumbnails does), or once the texture
    /// has gone unrequested long enough for the deferred sweep to have run.
    static void ReleaseTexture(nvrhi::ITexture *texture);

    /// @brief Selects which backend the loading spinner plays. `true` → the
    /// animated WebP (editor/loading/Spinner.webp, decoded to per-frame textures);
    /// `false` → the TTF whose consecutive glyphs are the frames. This is the single
    /// switch the header comment in the asset browser refers to: flip it and rebuild.
    /// Both assets load independently at startup and either may be absent (the
    /// browser then falls back to a plain placeholder), so flipping never crashes
    /// even if only one asset is shipped.
    static constexpr bool kUseWebpSpinner = false;

    /// @brief The editor's loading-spinner font, or null if none was shipped. A
    /// dedicated face whose consecutive glyphs are the frames of an animated
    /// loading icon: the caller advances one glyph per tick and loops, so drawing a
    /// frame costs a single textured quad. Kept apart from the text font so its
    /// frame glyphs never collide with real characters. Loaded from
    /// `editor/loading/Spinner.ttf` at startup; absent asset -> null, and
    /// callers fall back to a plain placeholder. Used only when kUseWebpSpinner is false.
    static ImFont *LoadingFont();

    /// @brief Number of decoded frames in the WebP loading spinner, or 0 if none is
    /// loaded (kUseWebpSpinner is false, the asset is absent, or decoding failed).
    /// The caller advances one frame per tick and loops.
    static std::size_t LoadingWebpFrameCount();

    /// @brief ImGui texture id for WebP spinner frame @p index (taken modulo the
    /// frame count, so any counter loops safely). Draw it with ImGui::Image /
    /// ImDrawList::AddImage. Returns ImTextureID_Invalid if no WebP spinner is loaded.
    /// Registers the frame's texture on demand, exactly like GetOrCreateTextureId
    /// (so it participates in the same per-frame reclamation).
    static ImTextureID LoadingWebpFrame(std::size_t index);
};

} // namespace Assisi::Debug
