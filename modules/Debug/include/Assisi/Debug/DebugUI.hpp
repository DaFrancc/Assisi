#pragma once

/// @file DebugUI.hpp
/// @brief Thin wrapper around Dear ImGui + GLFW/OpenGL3 backends.
///
/// Usage:
///   DebugUI::Initialize(window);            // once, after OpenGL context is current
///   // per frame:
///   DebugUI::BeginFrame();
///   ImGui::Begin("..."); ... ImGui::End();
///   DebugUI::EndFrame();
///   // on shutdown:
///   DebugUI::Shutdown();

#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Debug
{

class DebugUI
{
  public:
    /// @brief Initialises ImGui and attaches the GLFW + OpenGL3 backends.
    /// Must be called after the OpenGL context is current.
    static void Initialize(const Window::WindowContext &window);

    /// @brief Releases all ImGui resources.
    static void Shutdown();

    /// @brief Starts a new ImGui frame. Call before any ImGui:: calls.
    static void BeginFrame();

    /// @brief Renders the accumulated draw data. Call after all ImGui:: calls,
    ///        before window.SwapBuffers().
    static void EndFrame();
};

} // namespace Assisi::Debug