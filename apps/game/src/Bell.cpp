/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Bell.hpp"

#include <Assisi/App/AudioSystems.hpp>
#include <Assisi/App/Network.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

namespace Game
{

namespace
{

/// The bell B rings.
constexpr int32_t kBellOnB = 1;

} // namespace

void AskForBellSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no keyboard
    {
        return;
    }
    if (ctx.input->IsKeyPressed(Assisi::Window::Key::B))
    {
        ctx.net.SendIntent(RingBell{.bell = kBellOnB});
    }
}

void RingBellsSystem(Assisi::App::SystemContext &ctx)
{
    if (!ctx.net.IsAuthority())
    {
        return;
    }
    for (const auto &[sender, ring] : ctx.events.Read<Received<RingBell>>())
    {
        ctx.net.Send(BellRang{.bell = ring.bell});
    }
}

void PlayBellsSystem(Assisi::App::SystemContext &ctx)
{
    for (const auto &[sender, rang] : ctx.events.Read<Received<BellRang>>())
    {
        for (auto [entity, bell] : ctx.world.scene.Query<Bell>())
        {
            if (bell.number == rang.bell)
            {
                ctx.events.Push(Assisi::App::PlaySound{.target = entity});
            }
        }
    }
}

} // namespace Game
