/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/RenderSystem.hpp>

#include <Assisi/Core/Logger.hpp>

// Only this translation unit needs glslang — keeps it out of RenderSystem.hpp's
// public interface (and every module that transitively includes it).
#include <glslang/Public/ShaderLang.h>

namespace Assisi::Render
{

bool RenderSystem::Initialize(const Assisi::Window::WindowContext &window)
{
    if (!window.IsValid())
    {
        Assisi::Core::Log::Error("RenderSystem: Window is not valid.");
        return false;
    }

    glslang::InitializeProcess();

    s_vulkanContext = Vulkan::VulkanContext::Create(window);
    return s_vulkanContext != nullptr;
}

void RenderSystem::Shutdown()
{
    s_vulkanContext.reset();
    glslang::FinalizeProcess();
}

} // namespace Assisi::Render
