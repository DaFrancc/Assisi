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
        const Assisi::App::ScreenId id = Assisi::App::IdOf(ctx.world, *hud);
        hud->SetContext(Assisi::App::ScreenEntity(gained.entity));
        hud->OnContext({}, Assisi::Mondrian::ContextMoment::Lost, HudOrphaned{.screen = id});
        hud->Show();
        (void)scene.Add(gained.entity, PlayerHud{.screen = id});
    }

    for (const Assisi::App::LocalPlayerLost &lost : ctx.events.Read<Assisi::App::LocalPlayerLost>())
    {
        // A destroyed character has none: HudOrphanSystem has its screen.
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

void HudOrphanSystem(Assisi::App::SystemContext &ctx)
{
    for (const HudOrphaned &orphaned : ctx.events.Read<HudOrphaned>())
    {
        if (Assisi::Mondrian::Screen *const hud = Assisi::App::FindScreen(ctx.world, orphaned.screen))
        {
            Assisi::App::RemoveScreen(ctx.world, *hud);
        }
    }
}

} // namespace Game
