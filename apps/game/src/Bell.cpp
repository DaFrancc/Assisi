/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Bell.hpp"

#include "Player/BodyAnimation.hpp"

#include <Assisi/App/AudioSystems.hpp>
#include <Assisi/App/Network.hpp>
#include <Assisi/App/PlayerComponents.hpp>
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
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::B))
    {
        return;
    }
    // Rung by this machine's player, or by nobody in a level with no player.
    Assisi::ECS::Entity ringer = Assisi::ECS::NullEntity;
    for (auto [player, local] : ctx.world.scene.Query<Assisi::App::LocalPlayer>())
    {
        (void)local;
        ringer = player;
    }
    ctx.net.SendIntent(RingBell{.ringer = ringer, .bell = kBellOnB});
}

void RingBellsSystem(Assisi::App::SystemContext &ctx)
{
    if (!ctx.net.IsAuthority())
    {
        return;
    }
    for (const auto &[sender, ring] : ctx.events.Read<Received<RingBell>>())
    {
        ctx.net.Send(BellRang{.ringer = ring.ringer, .bell = ring.bell});
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
        // The gesture is played here, on every machine, from the message that
        // rang the bell: nothing about animation crosses the network.
        if (rang.ringer != Assisi::ECS::NullEntity)
        {
            PlayGesture(ctx.world.scene, rang.ringer);
        }
    }
}

} // namespace Game
