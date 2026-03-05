/// @file main.cpp
/// @brief Assisi Sandbox — demonstrates currently implemented systems.
///
/// Systems covered:
///   - ECS: SceneRegistry, Scene, Entity creation/destruction, component Add/Get/Has/Remove
///   - Window: WindowContext creation and event loop
///   - Render: RenderSystem initialization (OpenGL backend)

#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>

/// Example component — must be trivially copyable (SparseSet requirement).
struct Position
{
    float x, y, z;
};

int main()
{
    // -------------------------------------------------------------------------
    // ECS
    // -------------------------------------------------------------------------

    // SceneRegistry manages named scenes. You can have as many as you like.
    Assisi::ECS::SceneRegistry scenes;

    // Create() returns std::expected<Scene*, SceneError>.
    // It fails with SceneError::NameAlreadyTaken if the name is already in use.
    auto levelResult = scenes.Create("Level1");
    assert(levelResult.has_value() && "Failed to create Level1.");
    Assisi::ECS::Scene& level = *levelResult.value();

    // SetActive() returns std::expected<void, SceneError>.
    // It fails with SceneError::NotFound if no scene with that name exists.
    auto activeResult = scenes.SetActive("Level1");
    assert(activeResult.has_value() && "Failed to set Level1 as active.");

    // You can work through the reference returned by Create() ...
    auto e = level.Create();

    // ... or retrieve the active scene via Active() or Get().
    assert(scenes.Active()->IsAlive(e));

    // Add<T> lazily creates a component pool for T on first use.
    // Returns std::expected<T*, SparseSetError> — fails if the entity already
    // has a component of this type.
    auto addResult = level.Add<Position>(e, { 1.f, 2.f, 3.f });
    assert(addResult.has_value() && "Failed to add Position.");

    // Has<T> checks whether the entity currently has a component of type T.
    assert(level.Has<Position>(e));

    // Get<T> returns a T* — nullptr if the entity doesn't have the component.
    if (Position* pos = level.Get<Position>(e))
    {
        pos->x = 10.f;
        assert(pos->x == 10.f);
    }

    // Remove<T> removes just one component while keeping the entity alive.
    level.Remove<Position>(e);
    assert(!level.Has<Position>(e));
    assert(level.IsAlive(e));

    // Destroy removes the entity and automatically strips it from every
    // registered component pool — no manual per-component cleanup needed.
    auto readdResult = level.Add<Position>(e, { 0.f, 0.f, 0.f });
    assert(readdResult.has_value());
    level.Destroy(e);
    assert(!level.IsAlive(e));
    assert(!level.Has<Position>(e));

    std::cout << "ECS: all assertions passed.\n";

    // -------------------------------------------------------------------------
    // Window
    // -------------------------------------------------------------------------

    Assisi::Window::WindowConfiguration config;
    config.Width        = 1280;
    config.Height       = 720;
    config.Title        = "Assisi Sandbox";
    config.EnableVSync  = true;

    // WindowContext initialises GLFW and creates the OS window.
    // The second argument is an optional shared GlfwLibrary; passing nullptr
    // lets WindowContext manage its own GLFW lifetime.
    Assisi::Window::WindowContext window(config, nullptr);

    if (!window.IsValid())
    {
        std::cerr << "Failed to create window.\n";
        return EXIT_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Render
    // -------------------------------------------------------------------------

    // RenderSystem::Initialize selects the graphics backend at runtime.
    // Only OpenGL is implemented so far.
    if (!Assisi::Render::RenderSystem::Initialize(
            Assisi::Render::Backend::GraphicsBackend::OpenGL, window))
    {
        std::cerr << "Failed to initialize render system.\n";
        return EXIT_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Main loop
    // -------------------------------------------------------------------------

    while (!window.ShouldClose())
    {
        window.PollEvents();
        window.SwapBuffers();
    }

    return EXIT_SUCCESS;
}
