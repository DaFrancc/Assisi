/// @file main.cpp
/// @brief Assisi Sandbox — renders textured cubes driven by Jolt physics.
///
/// Demo:
///   - A static floor (box, 10×0.5×10)
///   - A dynamic cube that spawns at (0, 6, 0) and falls under gravity
///   - Both rendered with the mesh.vert / mesh.frag shader (white texture fallback)

#include <glad/glad.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <thread>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Sinks.hpp>
#include <Assisi/Debug/DebugUI.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>
#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Shader.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Window/WindowContext.hpp>

#include <imgui.h>

#include <cstdlib>
#include <tuple>

int main()
{
    // -------------------------------------------------------------------------
    // Logger
    // -------------------------------------------------------------------------

    // Register sinks — multiple can be active simultaneously.
    Assisi::Core::GetLogger().AddSink(std::make_shared<Assisi::Core::ConsoleSink>());
    Assisi::Core::GetLogger().AddSink(std::make_shared<Assisi::Core::FileSink>("assisi.log"));

    // Trace/Debug/Info/Warn — compile-time format checking, no source location capture.
    Assisi::Core::Log::Trace("Trace: verbose internal detail, {} items", 3);
    Assisi::Core::Log::Debug("Debug: useful during development");
    Assisi::Core::Log::Info("Info:  general status messages");
    Assisi::Core::Log::Warn("Warn:  something unexpected but recoverable");

    // Error/Fatal — file and line are captured automatically at the call site.
    Assisi::Core::Log::Error("Error: something failed");
    Assisi::Core::Log::Fatal("Fatal: unrecoverable, shutting down");

    // -------------------------------------------------------------------------
    // Window
    // -------------------------------------------------------------------------

    Assisi::Window::WindowConfiguration config;
    config.Width = 1280;
    config.Height = 720;
    config.Title = "Assisi — Physics Demo";
    config.EnableVSync = true;

    Assisi::Window::WindowContext window(config, nullptr);
    if (!window.IsValid())
    {
        Assisi::Core::Log::Error("Failed to create window.");
        return EXIT_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Render system
    // -------------------------------------------------------------------------

    if (!Assisi::Render::RenderSystem::Initialize(Assisi::Render::Backend::GraphicsBackend::OpenGL, window))
    {
        Assisi::Core::Log::Error("Failed to initialize render system.");
        return EXIT_FAILURE;
    }

    glEnable(GL_DEPTH_TEST);

    // -------------------------------------------------------------------------
    // Debug UI (Dear ImGui)
    // -------------------------------------------------------------------------

    Assisi::Debug::DebugUI::Initialize(window);

    // -------------------------------------------------------------------------
    // Asset system
    // -------------------------------------------------------------------------

    if (auto result = Assisi::Core::AssetSystem::Initialize(); !result)
    {
        Assisi::Core::Log::Error("Failed to initialize asset system.");
        return EXIT_FAILURE;
    }

    // -------------------------------------------------------------------------
    // Shader
    // -------------------------------------------------------------------------

    Assisi::Render::Shader shader("shaders/mesh.vert", "shaders/mesh.frag");
    if (!shader.IsValid())
    {
        Assisi::Core::Log::Error("Failed to load mesh shader.");
        return EXIT_FAILURE;
    }

    // -------------------------------------------------------------------------
    // GPU meshes
    // -------------------------------------------------------------------------

    Assisi::Render::OpenGL::MeshBuffer cubeMesh(Assisi::Render::CreateUnitCubeMesh());

    // -------------------------------------------------------------------------
    // ECS scene
    // -------------------------------------------------------------------------

    Assisi::ECS::SceneRegistry scenes;
    Assisi::ECS::Scene &scene = *scenes.Create("Main").value();

    // -------------------------------------------------------------------------
    // Physics world
    // -------------------------------------------------------------------------

    Assisi::Physics::PhysicsWorld physics;

    // Floor — static box (half extents: 5×0.25×5, centred at y = −0.25)
    {
        Assisi::ECS::Entity floor = scene.Create();

        std::ignore = scene.Add<Assisi::Runtime::TransformComponent>(
            floor, {.position = {0.f, -0.25f, 0.f}, .rotation = {1.f, 0.f, 0.f, 0.f}, .scale = {10.f, 0.5f, 10.f}});

        std::ignore = scene.Add<Assisi::Runtime::MeshRendererComponent>(floor, {.mesh = &cubeMesh, .textureId = 0u});

        std::ignore = scene.Add<Assisi::Physics::RigidBodyComponent>(
            floor, physics.AddBox({0.f, -0.25f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {5.f, 0.25f, 5.f},
                                  Assisi::Physics::BodyMotion::Static));
    }

    // Dynamic cube — spawns at y = 6, tilted so it lands on a corner.
    // RigidBodyComponent and spawn transform stored outside the block for ImGui access.
    constexpr glm::vec3 kCubeSpawnPos = {0.f, 6.f, 0.f};
    const glm::quat cornerRot = glm::normalize(
        glm::angleAxis(glm::radians(45.f), glm::vec3(0.f, 0.f, 1.f)) *
        glm::angleAxis(glm::radians(45.f), glm::vec3(1.f, 0.f, 0.f)));

    Assisi::Physics::RigidBodyComponent cubeRb{};
    {
        Assisi::ECS::Entity cube = scene.Create();

        std::ignore = scene.Add<Assisi::Runtime::TransformComponent>(
            cube, {.position = kCubeSpawnPos, .rotation = cornerRot, .scale = {1.f, 1.f, 1.f}});

        std::ignore = scene.Add<Assisi::Runtime::MeshRendererComponent>(cube, {.mesh = &cubeMesh, .textureId = 0u});

        cubeRb = physics.AddBox(kCubeSpawnPos, cornerRot, {0.5f, 0.5f, 0.5f}, Assisi::Physics::BodyMotion::Dynamic);
        std::ignore = scene.Add<Assisi::Physics::RigidBodyComponent>(cube, cubeRb);
    }

    // -------------------------------------------------------------------------
    // Camera + projection
    // -------------------------------------------------------------------------

    Assisi::Runtime::Camera camera({5.f, 5.f, 10.f}, {0.f, 0.f, 0.f});

    const glm::mat4 projection =
        glm::perspective(glm::radians(60.f), static_cast<float>(config.Width) / static_cast<float>(config.Height),
                         0.1f, 200.f);

    // -------------------------------------------------------------------------
    // Input
    // -------------------------------------------------------------------------

    Assisi::Window::InputContext input(window);

    // -------------------------------------------------------------------------
    // Main loop — fixed physics at 60 Hz, render capped at 144 Hz
    // -------------------------------------------------------------------------

    using Clock = std::chrono::steady_clock;
    using Seconds = std::chrono::duration<double>;

    constexpr double kPhysicsStep = 1.0 / 60.0;
    constexpr double kRenderStep = 1.0 / 144.0;

    /* Disable VSync so the render cap controls frame rate. */
    window.SetVSyncEnabled(false);

    auto prevTime = Clock::now();
    auto nextRenderTime = Clock::now();
    double accumulator = 0.0;
    int spawnCount = 0;

    while (!window.ShouldClose())
    {
        const auto now = Clock::now();
        const double deltaTime = std::min(Seconds(now - prevTime).count(), 0.25);
        prevTime = now;

        /* OS events + input snapshot */
        Assisi::Window::WindowContext::PollEvents();
        input.Poll();

        if (input.IsMouseButtonPressed(Assisi::Window::MouseButton::Left) && !input.IsMouseCaptured() &&
            !ImGui::GetIO().WantCaptureMouse)
        {
            input.SetMouseCaptured(true);
        }

        if (input.IsKeyPressed(Assisi::Window::Key::Escape))
        {
            if (input.IsMouseCaptured())
            {
                input.SetMouseCaptured(false);
            }
            else
            {
                window.RequestClose();
            }
        }

        /* Fixed-rate physics accumulator */
        accumulator += deltaTime;
        while (accumulator >= kPhysicsStep)
        {
            physics.Update(static_cast<float>(kPhysicsStep));
            accumulator -= kPhysicsStep;
        }
        physics.SyncTransforms(scene);

        /* Render only when the render budget allows */
        if (Clock::now() >= nextRenderTime)
        {
            nextRenderTime += std::chrono::duration_cast<Clock::duration>(Seconds(kRenderStep));

            glClearColor(0.15f, 0.15f, 0.18f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            shader.Use();
            shader.SetVec3("uViewPos",            camera.WorldPosition());
            shader.SetVec3("uDirLight.direction", {-0.4f, -1.0f, -0.5f});
            shader.SetVec3("uDirLight.ambient",   {0.15f, 0.15f, 0.15f});
            shader.SetVec3("uDirLight.diffuse",   {0.9f,  0.9f,  0.9f });
            shader.SetVec3("uDirLight.specular",  {0.5f,  0.5f,  0.5f });
            shader.SetFloat("uShininess",         32.f);

            Assisi::Runtime::DrawScene(scene, camera, projection, shader);

            /* --- ImGui panel --- */
            Assisi::Debug::DebugUI::BeginFrame();

            ImGui::Begin("World");

            // Cube state
            auto [cubePos, cubeRot] = physics.GetBodyTransform(cubeRb);
            ImGui::SeparatorText("Dynamic Cube");
            ImGui::Text("Position  %.2f  %.2f  %.2f", cubePos.x, cubePos.y, cubePos.z);
            if (ImGui::Button("Reset Cube"))
            {
                physics.SetBodyTransform(cubeRb, kCubeSpawnPos, cornerRot);
            }

            // Gravity
            ImGui::SeparatorText("Physics");
            glm::vec3 gravity = physics.GetGravity();
            if (ImGui::SliderFloat("Gravity Y", &gravity.y, -20.f, 0.f))
            {
                physics.SetGravity(gravity);
            }

            // Spawn
            ImGui::SeparatorText("Spawn");
            if (ImGui::Button("Spawn Cube"))
            {
                const float offsetX = static_cast<float>(spawnCount % 5 - 2) * 1.5f;
                const float offsetZ = static_cast<float>(spawnCount / 5 % 5 - 2) * 1.5f;
                const glm::vec3 spawnPos = {offsetX, 8.f, offsetZ};

                Assisi::ECS::Entity newCube = scene.Create();
                std::ignore = scene.Add<Assisi::Runtime::TransformComponent>(
                    newCube, {.position = spawnPos, .rotation = cornerRot, .scale = {1.f, 1.f, 1.f}});
                std::ignore = scene.Add<Assisi::Runtime::MeshRendererComponent>(
                    newCube, {.mesh = &cubeMesh, .textureId = 0u});
                const auto newRb = physics.AddBox(spawnPos, cornerRot, {0.5f, 0.5f, 0.5f},
                                                  Assisi::Physics::BodyMotion::Dynamic);
                std::ignore = scene.Add<Assisi::Physics::RigidBodyComponent>(newCube, newRb);
                ++spawnCount;
            }
            ImGui::SameLine();
            ImGui::Text("(%d spawned)", spawnCount);

            ImGui::SeparatorText("Hint");
            ImGui::TextDisabled("LMB: capture camera  |  Esc: release / quit");

            ImGui::End();

            Assisi::Debug::DebugUI::EndFrame();
            /* ------------------- */

            window.SwapBuffers();
        }
    }

    Assisi::Debug::DebugUI::Shutdown();
    return EXIT_SUCCESS;
}
