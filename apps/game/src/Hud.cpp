/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Hud.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/Mondrian/Screen.hpp>

namespace Game
{

void HudSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    Assisi::ECS::Scene &scene = ctx.world.scene;

    for (const Assisi::App::LocalPlayerGained &gained : ctx.events.Read<Assisi::App::LocalPlayerGained>())
    {
        Assisi::Mondrian::Screen *const hud = Assisi::App::LoadScreen(ctx.world, *ctx.ui, kHudScreenPath);
        if (hud == nullptr)
        {
            continue; // logged by LoadScreen
        }
        hud->SetContext(Assisi::App::ScreenEntity(gained.entity));
        hud->Show();
        (void)scene.Add(gained.entity, PlayerHud{.screen = Assisi::App::IdOf(ctx.world, *hud)});
    }

    for (const Assisi::App::LocalPlayerLost &lost : ctx.events.Read<Assisi::App::LocalPlayerLost>())
    {
        const PlayerHud *const owned = scene.Get<PlayerHud>(lost.entity);
        if (owned == nullptr)
        {
            continue;
        }
        if (Assisi::Mondrian::Screen *const hud = Assisi::App::FindScreen(ctx.world, owned->screen))
        {
            Assisi::App::RemoveScreen(ctx.world, *hud);
        }
        (void)scene.Remove<PlayerHud>(lost.entity);
    }
}

} // namespace Game
