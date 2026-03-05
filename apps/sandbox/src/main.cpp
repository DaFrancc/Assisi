/// @file main.cpp
/// @brief Assisi Sandbox — demonstrates currently implemented systems.
///
/// Systems covered:
///   - Logger: ConsoleSink, FileSink, free functions with std::format style
///   - ECS: SceneRegistry, Scene, Entity creation/destruction, component Add/Get/Has/Remove
///   - Window: WindowContext creation and event loop
///   - Render: RenderSystem initialization (OpenGL backend)

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <cassert>
#include <cstdlib>

/// Example components — must be trivially copyable (SparseSet requirement).
struct Position
{
    float x, y, z;
};

struct Velocity
{
    float x, y, z;
};

int main()
{
    // -------------------------------------------------------------------------
    // Logger
    // -------------------------------------------------------------------------

    // Register sinks on the global logger before any logging takes place.
    // Multiple sinks can be active simultaneously.
    Assisi::Core::GetLogger().AddSink(std::make_shared<Assisi::Core::ConsoleSink>());
    Assisi::Core::GetLogger().AddSink(std::make_shared<Assisi::Core::FileSink>("assisi.log"));

    // Trace/Debug/Info/Warn — compile-time format checking, no location capture.
    Assisi::Core::Log::Trace("Trace: verbose internal detail, {} items", 3);
    Assisi::Core::Log::Debug("Debug: useful during development");
    Assisi::Core::Log::Info("Info:  general status messages");
    Assisi::Core::Log::Warn("Warn:  something unexpected but recoverable");

    // Error/Fatal — file and line are captured automatically at the call site.
    // No need to pass __FILE__ or __LINE__ manually.
    Assisi::Core::Log::Error("Error: something failed");
    Assisi::Core::Log::Fatal("Fatal: unrecoverable, shutting down");

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

    Assisi::Core::Log::Info("ECS: basic assertions passed.");

    // -------------------------------------------------------------------------
    // ECS — Query
    // -------------------------------------------------------------------------

    // Spawn a few entities with different component combinations.
    Assisi::ECS::Scene& query_scene = *scenes.Create("QueryDemo").value();

    Assisi::ECS::Entity a = query_scene.Create();
    Assisi::ECS::Entity b = query_scene.Create();
    Assisi::ECS::Entity c = query_scene.Create();

    query_scene.Add<Position>(a, { 1.f, 0.f, 0.f });
    query_scene.Add<Velocity>(a, { 0.1f, 0.f, 0.f });

    query_scene.Add<Position>(b, { 2.f, 0.f, 0.f });
    query_scene.Add<Velocity>(b, { 0.2f, 0.f, 0.f });

    // Entity c has Position but NO Velocity — should be skipped by the query.
    query_scene.Add<Position>(c, { 3.f, 0.f, 0.f });

    // Query<Position, Velocity> yields only entities a and b.
    // Structured bindings give direct references into the component arrays.
    int count = 0;
    for (auto [entity, pos, vel] : query_scene.Query<Position, Velocity>())
    {
        pos.x += vel.x;
        ++count;
    }

    assert(count == 2);
    assert(query_scene.Get<Position>(a)->x == 1.1f);
    assert(query_scene.Get<Position>(b)->x == 2.2f);
    assert(query_scene.Get<Position>(c)->x == 3.f); // untouched

    Assisi::Core::Log::Info("ECS: query iterated {} entities (expected 2).", count);

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
        Assisi::Core::Log::Error("Failed to create window.");
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
        Assisi::Core::Log::Error("Failed to initialize render system.");
        return EXIT_FAILURE;
    }

    Assisi::Core::Log::Info("Render system initialized.");

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
