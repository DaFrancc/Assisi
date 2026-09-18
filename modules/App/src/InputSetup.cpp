/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/InputSetup.hpp>

namespace Assisi::App
{

void LoadActionMap(Window::ActionMap &actions, const Window::InputBindings &player)
{
    if (const auto shipped = Window::LoadInputBindings())
    {
        actions.Apply(*shipped);
    }
    actions.Apply(player);
}

} // namespace Assisi::App
