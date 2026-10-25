/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/WorldFrame.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>

namespace Assisi::App
{

void BeginWorldFrame(const SystemContext &ctx)
{
    ctx.world.systems.Run(SystemPhase::PreUpdate, ctx);
}

} // namespace Assisi::App
