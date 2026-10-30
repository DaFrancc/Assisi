/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "TuningScreen.hpp"

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <optional>

ASSISI_REFLECTED_STRUCT(Game::TuningSettings);

namespace Game
{
namespace
{

/// The one set of settings, which outlives every world and the UI.
TuningSettings &Settings()
{
    static TuningSettings settings;
    return settings;
}

} // namespace

void TuningScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    ctx.ui->RegisterSource(Settings());
    // A screen that failed to load logs why and leaves the world running.
    (void)Assisi::App::LoadScreen(ctx.world, *ctx.ui, kTuningScreenPath);
}

void TuningScreenToggleSystem(Assisi::App::SystemContext &ctx)
{
    for (const DifficultyChanged &changed : ctx.events.Read<DifficultyChanged>())
    {
        Assisi::Core::Log::Info("Tuning: difficulty moved to step {}.", changed.value);
    }

    if (ctx.ui == nullptr || ctx.input == nullptr) // headless host: no UI, no devices
    {
        return;
    }
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::F9))
    {
        return;
    }

    Assisi::Mondrian::Screen *const tuning = Assisi::App::FindScreen(ctx.world, kTuningScreenPath);
    if (tuning == nullptr)
    {
        return; // this level did not ask for one
    }
    ctx.input->ConsumeKey(Assisi::Window::Key::F9);

    if (tuning->IsShown())
    {
        tuning->Hide();
        return;
    }
    if (const std::optional<Assisi::App::SceneView> view = Assisi::App::ActiveSceneCamera(ctx.world.scene))
    {
        tuning->SetContext(Assisi::App::ScreenEntity(view->entity));
    }
    tuning->Show();
}

} // namespace Game
