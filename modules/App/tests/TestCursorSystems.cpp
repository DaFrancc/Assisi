/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCursorSystems.cpp
/// @brief The engine's CaptureCursor and CursorToggle, found by the names a
/// level gives them with no game linked: the first takes the mouse once the
/// level has loaded, the second takes it back on a click while it is free, and
/// neither does anything on a host with no input.

#include <doctest/doctest.h>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Physics/CollisionSource.hpp>
#include <Assisi/Window/InputContext.hpp>

using namespace Assisi;
using Window::InputMode;
using Window::KeyAction;
using Window::MouseButton;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// The catalog's system called @p name, which must be there and run in @p phase.
const App::SystemDefinition &Named(std::string_view name, App::SystemPhase phase)
{
    const App::SystemDefinition *found = App::SystemCatalog::Instance().Find(name);
    REQUIRE_MESSAGE(found != nullptr, name);
    CHECK(found->phase == phase);
    CHECK(found->activeWorldOnly);
    REQUIRE(found->run);
    return *found;
}

/// Runs @p system once in a world of its own, with @p input, which may be null.
void RunWith(const App::SystemDefinition &system, Window::InputContext *input)
{
    App::World world{Physics::NoCollisionAssets()};
    Core::EventQueue universal{Core::EventScope::Universal};
    App::SystemContext ctx{world, kStep, 0, input, nullptr, universal, true, nullptr};
    system.run(ctx);
}

} // namespace

TEST_CASE("CaptureCursor: a level that names it takes the mouse once it has loaded, with no game linked")
{
    const App::SystemDefinition &capture = Named("CaptureCursor", App::SystemPhase::Loaded);
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);
    RunWith(capture, &input);
    CHECK(input.GetInputMode() == InputMode::Game);

    // A headless host has no input at all.
    RunWith(capture, nullptr);
}

TEST_CASE("CursorToggle: a click takes the mouse back while it is free, and nothing else does")
{
    const App::SystemDefinition &toggle = Named("CursorToggle", App::SystemPhase::Update);
    Window::InputContext input;
    input.SetInputMode(InputMode::Ui);

    // Free, and nothing clicked: it stays free.
    input.Poll();
    RunWith(toggle, &input);
    CHECK(input.GetInputMode() == InputMode::Ui);

    // A left click takes it back.
    input.OnMouseButton(MouseButton::Left, KeyAction::Press, 0.0);
    input.Poll();
    RunWith(toggle, &input);
    CHECK(input.GetInputMode() == InputMode::Game);

    RunWith(toggle, nullptr);
}
