/// @file main.cpp
/// @brief Assisi Sandbox — physics demo built on the Application layer.

#include <Assisi/App/Application.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>
#include <Assisi/Render/Shader.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <cstdlib>
#include <tuple>

// ---------------------------------------------------------------------------
// SandboxApp
// ---------------------------------------------------------------------------

class SandboxApp : public Assisi::App::Application
{
  public:
    void OnStart();
    void OnFixedUpdate(float dt);
    void OnUpdate(float dt);
    void OnRender();
    void OnImGui();

  private:
    Assisi::ECS::SceneRegistry    _scenes;
    Assisi::ECS::Scene           *_scene = nullptr;
    Assisi::Physics::PhysicsWorld _physics;

    Assisi::Render::OpenGL::MeshBuffer _cubeMesh;
    Assisi::Render::Shader             _shader;
    Assisi::Runtime::Camera            _camera;
    glm::mat4                          _projection{1.f};

    Assisi::Physics::RigidBodyComponent _cubeRb{};
    glm::quat                           _cornerRot{1.f, 0.f, 0.f, 0.f};
    glm::vec3                           _spawnPos{0.f, 6.f, 0.f};
    int                                 _spawnCount = 0;
};

// ---------------------------------------------------------------------------

void SandboxApp::OnStart()
{
    // Logger examples
    Assisi::Core::Log::Trace("Trace: verbose internal detail, {} items", 3);
    Assisi::Core::Log::Debug("Debug: useful during development");
    Assisi::Core::Log::Info("Info:  general status messages");
    Assisi::Core::Log::Warn("Warn:  something unexpected but recoverable");
    Assisi::Core::Log::Error("Error: something failed");
    Assisi::Core::Log::Fatal("Fatal: unrecoverable, shutting down");

    _scene = _scenes.Create("Main").value();

    _cubeMesh = Assisi::Render::OpenGL::MeshBuffer(Assisi::Render::CreateUnitCubeMesh());

    _shader = Assisi::Render::Shader("shaders/mesh.vert", "shaders/mesh.frag");
    if (!_shader.IsValid())
    {
        Assisi::Core::Log::Error("Failed to load mesh shader.");
        RequestClose();
        return;
    }

    _camera     = Assisi::Runtime::Camera({5.f, 5.f, 10.f}, {0.f, 0.f, 0.f});
    _projection = MakeProjection();

    // Floor — static box
    {
        Assisi::ECS::Entity floor = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            floor, {.position = {0.f, -0.25f, 0.f}, .rotation = {1.f, 0.f, 0.f, 0.f}, .scale = {10.f, 0.5f, 10.f}});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(floor, {.mesh = &_cubeMesh, .textureId = 0u});
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(
            floor, _physics.AddBox({0.f, -0.25f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {5.f, 0.25f, 5.f},
                                   Assisi::Physics::BodyMotion::Static));
    }

    // Dynamic cube — tilted to land on a corner
    _cornerRot = glm::normalize(
        glm::angleAxis(glm::radians(45.f), glm::vec3(0.f, 0.f, 1.f)) *
        glm::angleAxis(glm::radians(45.f), glm::vec3(1.f, 0.f, 0.f)));
    {
        Assisi::ECS::Entity cube = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            cube, {.position = _spawnPos, .rotation = _cornerRot, .scale = {1.f, 1.f, 1.f}});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(cube, {.mesh = &_cubeMesh, .textureId = 0u});
        _cubeRb = _physics.AddBox(_spawnPos, _cornerRot, {0.5f, 0.5f, 0.5f}, Assisi::Physics::BodyMotion::Dynamic);
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(cube, _cubeRb);
    }
}

void SandboxApp::OnFixedUpdate(float dt)
{
    _physics.Update(dt);
    _physics.SyncTransforms(*_scene);
}

void SandboxApp::OnUpdate(float /*dt*/)
{
    auto &input = GetInput();

    if (input.IsMouseButtonPressed(Assisi::Window::MouseButton::Left) &&
        !input.IsMouseCaptured() && !ImGui::GetIO().WantCaptureMouse)
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
            RequestClose();
        }
    }
}

void SandboxApp::OnRender()
{
    _shader.Use();
    _shader.SetVec3("uViewPos",            _camera.WorldPosition());
    _shader.SetVec3("uDirLight.direction", {-0.4f, -1.0f, -0.5f});
    _shader.SetVec3("uDirLight.ambient",   {0.15f, 0.15f, 0.15f});
    _shader.SetVec3("uDirLight.diffuse",   {0.9f,  0.9f,  0.9f });
    _shader.SetVec3("uDirLight.specular",  {0.5f,  0.5f,  0.5f });
    _shader.SetFloat("uShininess",         32.f);

    Assisi::Runtime::DrawScene(*_scene, _camera, _projection, _shader);
}

void SandboxApp::OnImGui()
{
    ImGui::Begin("World");

    ImGui::Text("FPS: %d", GetFps());
    ImGui::Text("Sleep resolution: %.2f ms", GetSleepResolutionMs());
    ImGui::Separator();

    auto [cubePos, cubeRot] = _physics.GetBodyTransform(_cubeRb);
    ImGui::SeparatorText("Dynamic Cube");
    ImGui::Text("Position  %.2f  %.2f  %.2f", cubePos.x, cubePos.y, cubePos.z);
    if (ImGui::Button("Reset Cube"))
    {
        _physics.SetBodyTransform(_cubeRb, _spawnPos, _cornerRot);
    }

    ImGui::SeparatorText("Physics");
    glm::vec3 gravity = _physics.GetGravity();
    if (ImGui::SliderFloat("Gravity Y", &gravity.y, -20.f, 0.f))
    {
        _physics.SetGravity(gravity);
    }

    ImGui::SeparatorText("Spawn");
    if (ImGui::Button("Spawn Cube"))
    {
        const float offsetX = static_cast<float>((_spawnCount % 5) - 2) * 1.5f;
        const float offsetZ = static_cast<float>((_spawnCount / 5 % 5) - 2) * 1.5f;
        const glm::vec3 spawnPos = {offsetX, 8.f, offsetZ};

        Assisi::ECS::Entity newCube = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            newCube, {.position = spawnPos, .rotation = _cornerRot, .scale = {1.f, 1.f, 1.f}});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(
            newCube, {.mesh = &_cubeMesh, .textureId = 0u});
        const auto newRb = _physics.AddBox(spawnPos, _cornerRot, {0.5f, 0.5f, 0.5f},
                                           Assisi::Physics::BodyMotion::Dynamic);
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(newCube, newRb);
        ++_spawnCount;
    }
    ImGui::SameLine();
    ImGui::Text("(%d spawned)", _spawnCount);

    ImGui::SeparatorText("Hint");
    ImGui::TextDisabled("LMB: capture camera  |  Esc: release / quit");

    ImGui::End();
}

// ---------------------------------------------------------------------------

int main()
{
    SandboxApp app;
    app.Run();
    return EXIT_SUCCESS;
}