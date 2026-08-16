/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/GpuMarker.hpp>

#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>

namespace Assisi::Render
{

GpuPassTimerScope::GpuPassTimerScope(const char *name)
{
    // Resolved per scope rather than cached: the context is recreated on a
    // device loss, and a stale pointer here would be recording into freed
    // queries. The lookup is a pointer read.
    Vulkan::VulkanContext *context = RenderSystem::GetVulkanContext();
    if (context == nullptr || !context->IsPassTimingEnabled())
    {
        return;
    }

    context->BeginPassTimer(name);
    _opened = true;
}

GpuPassTimerScope::~GpuPassTimerScope()
{
    if (!_opened)
    {
        return;
    }

    // Deliberately not re-checking IsPassTimingEnabled: the open happened, so
    // the close has to, or the command list carries an unbalanced query. The
    // context latches the enable for a whole frame for the same reason.
    if (Vulkan::VulkanContext *context = RenderSystem::GetVulkanContext())
    {
        context->EndPassTimer();
    }
}

} // namespace Assisi::Render
