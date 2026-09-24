/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "PauseMenu.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

namespace Game
{

void PauseMenuScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    // Everything the menu is — its shape, its colours, what its buttons do and
    // the system it needs — is in the file. A screen that failed to load logs
    // why and leaves the world running.
    (void)Assisi::App::LoadScreen(ctx.world, *ctx.ui, kPauseScreenPath);
}

void PauseMenuSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr) // headless host: no UI, no devices
    {
        return;
    }

    Assisi::Mondrian::Screen *const pause = Assisi::App::FindScreen(ctx.world, kPauseScreenPath);
    if (pause == nullptr)
    {
        return; // this level did not ask for one
    }

    if (ctx.input->IsKeyPressed(Assisi::Window::Key::Escape))
    {
        ctx.input->ConsumeKey(Assisi::Window::Key::Escape);
        pause->Show();
    }
}

} // namespace Game
