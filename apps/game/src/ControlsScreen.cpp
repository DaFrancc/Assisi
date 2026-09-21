/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ControlsScreen.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

namespace Game
{

void ControlsScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    // Everything the screen is — its controls, their arguments and its colours
    // — is in the file. One that failed to load logs why and leaves the world
    // running.
    (void)Assisi::App::LoadScreen(ctx.world, *ctx.ui, kControlsScreenPath);
}

void ControlsScreenToggleSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr) // headless host: no UI, no devices
    {
        return;
    }
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::F3))
    {
        return;
    }

    Assisi::Mondrian::Screen *const controls = Assisi::App::FindScreen(ctx.world, kControlsScreenName);
    if (controls == nullptr)
    {
        return; // this level did not ask for one
    }
    ctx.input->ConsumeKey(Assisi::Window::Key::F3);

    if (controls->IsShown())
    {
        controls->Hide();
        return;
    }
    controls->Show();
}

} // namespace Game
