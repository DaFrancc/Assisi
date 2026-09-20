/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "SampleScreenDemo.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Mondrian/SampleScreen.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <memory>

namespace Game
{

void SampleScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    // The engine registered a texture for an image with nothing else to show;
    // the sample's picture is exactly that case.
    Assisi::App::AddScreen(ctx.world, Assisi::Mondrian::AddSampleScreen(*ctx.ui, ctx.ui->GetPlaceholderTexture()), {});
}

void SampleScreenToggleSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr) // headless host: no UI, no devices
    {
        return;
    }
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::F4))
    {
        return;
    }

    Assisi::Mondrian::Screen *const sample = Assisi::App::FindScreen(ctx.world, kSampleScreenName);
    if (sample == nullptr)
    {
        return; // this level did not ask for one
    }
    ctx.input->ConsumeKey(Assisi::Window::Key::F4);

    if (sample->IsShown())
    {
        sample->Hide();
        return;
    }
    sample->Show();
}

} // namespace Game
