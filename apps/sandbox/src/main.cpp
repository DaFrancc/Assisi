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

#include <glad/glad.h>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
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
    void OnShutdown() override;
    void OnResize(int width, int height) override;

  private:
    Assisi::ECS::SceneRegistry    _scenes;
    Assisi::ECS::Scene           *_scene = nullptr;
    Assisi::Physics::PhysicsWorld _physics;

    Assisi::Render::OpenGL::MeshBuffer _cubeMesh;
    Assisi::Render::Shader             _shader;
    Assisi::Runtime::Camera            _camera;
    Assisi::Runtime::LightingSystem    _lighting;
    glm::mat4                          _projection{1.f};

    static constexpr float kNearZ = 0.1f;
    static constexpr float kFarZ  = 200.f;

    // Camera control state
    float _yaw         = -116.6f; // initialised in OnStart from camera direction
    float _pitch       =  -24.1f;
    float _fovDegrees  =   60.f;

    static constexpr float kMoveSpeed        = 8.f;   // units/s
    static constexpr float kMouseSensitivity = 0.1f;  // degrees/pixel

    std::vector<unsigned int>           _testTextures; // owned by sandbox, deleted in OnShutdown

    Assisi::Physics::RigidBodyComponent _cubeRb{};
    glm::quat                           _cornerRot{1.f, 0.f, 0.f, 0.f};
    glm::vec3                           _spawnPos{0.f, 6.f, 0.f};
    int                                 _spawnCount = 0;
};

// ---------------------------------------------------------------------------

void SandboxApp::OnStart()
{
    // Logger examples
    // Assisi::Core::Log::Trace("Trace: verbose internal detail, {} items", 3);
    // Assisi::Core::Log::Debug("Debug: useful during development");
    // Assisi::Core::Log::Info("Info:  general status messages");
    // Assisi::Core::Log::Warn("Warn:  something unexpected but recoverable");
    // Assisi::Core::Log::Error("Error: something failed");
    // Assisi::Core::Log::Fatal("Fatal: unrecoverable, shutting down");

    _scene = _scenes.Create("Main").value();

    _cubeMesh = Assisi::Render::OpenGL::MeshBuffer(Assisi::Render::CreateUnitCubeMesh());

    _shader = Assisi::Render::Shader("shaders/mesh.vert", "shaders/mesh.frag");
    if (!_shader.IsValid())
    {
        Assisi::Core::Log::Error("Failed to load mesh shader.");
        RequestClose();
        return;
    }

    _camera = Assisi::Runtime::Camera({5.f, 5.f, 10.f}, {0.f, 0.f, 0.f});

    // Lighting system — must be initialised after the OpenGL context is ready
    {
        const auto size = GetWindow().GetFramebufferSize();
        _projection     = MakeProjection(_fovDegrees, kNearZ, kFarZ);
        if (!_lighting.Initialize(size.Width, size.Height, kNearZ, kFarZ, _projection))
        {
            Assisi::Core::Log::Error("Failed to initialise LightingSystem.");
            RequestClose();
            return;
        }
        _lighting.SetupMeshShader(_shader);
    }

    // Derive initial yaw/pitch from camera direction so mouse control is consistent.
    const glm::vec3 forward = _camera.ForwardDirection();
    _pitch = glm::degrees(glm::asin(forward.y));
    _yaw   = glm::degrees(glm::atan(forward.z, forward.x));

    // Directional light (sun)
    {
        Assisi::ECS::Entity sun = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::DirectionalLightComponent>(
            sun, {.direction = {-0.4f, -1.0f, -0.5f}, .color = {1.f, 1.f, 1.f}, .intensity = 0.5f});
    }

    // Floor — static box
    {
        Assisi::ECS::Entity floor = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            floor, {.position = {0.f, -0.25f, 0.f}, .rotation = {1.f, 0.f, 0.f, 0.f}, .scale = {10.f, 0.5f, 10.f}});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(floor, {.mesh = &_cubeMesh, .albedoTextureId = 0u});
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
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(cube, {.mesh = &_cubeMesh, .albedoTextureId = 0u});
        _cubeRb = _physics.AddBox(_spawnPos, _cornerRot, {0.5f, 0.5f, 0.5f}, Assisi::Physics::BodyMotion::Dynamic);
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(cube, _cubeRb);
    }

    // Helper: create a 1×1 sRGB texture for a given colour / value
    auto makeTex = [&](uint8_t r, uint8_t g, uint8_t b) -> unsigned int
    {
        unsigned int id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        const uint8_t px[4] = {r, g, b, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        _testTextures.push_back(id);
        return id;
    };

    // Albedos
    const unsigned int texGold  = makeTex(255, 200,  50); // warm gold
    const unsigned int texRed   = makeTex(200,  35,  35); // saturated red
    const unsigned int texBlue  = makeTex( 55, 100, 220); // cool blue

    // Roughness values (R channel used; white = 1.0, black = 0.0)
    const unsigned int roughLow  = makeTex(  8,   8,   8); // ~0.03 — near-mirror
    const unsigned int roughHigh = makeTex(245, 245, 245); // ~0.96 — very rough
    const unsigned int roughMed  = makeTex(128, 128, 128); // ~0.50 — semi-rough

    // Metallic values
    const unsigned int metalFull = makeTex(255, 255, 255); // 1.0 — fully metallic
    const unsigned int metalNone = makeTex(  0,   0,   0); // 0.0 — dielectric
    const unsigned int metalMed  = makeTex(128, 128, 128); // 0.5 — partial

    // --- Test cube 1: polished gold metal (high metallic, near-zero roughness) ---
    {
        constexpr glm::vec3 pos{-3.f, 0.5f, -2.f};
        Assisi::ECS::Entity cube = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(cube, {.position = pos});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(
            cube, {.mesh = &_cubeMesh, .albedoTextureId = texGold,
                   .metallicTextureId = metalFull, .roughnessTextureId = roughLow});
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(
            cube, _physics.AddBox(pos, {1.f, 0.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  Assisi::Physics::BodyMotion::Dynamic));
    }

    // --- Test cube 2: rough red plastic (no metallic, high roughness) ---
    {
        constexpr glm::vec3 pos{0.f, 0.5f, -3.f};
        Assisi::ECS::Entity cube = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(cube, {.position = pos});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(
            cube, {.mesh = &_cubeMesh, .albedoTextureId = texRed,
                   .metallicTextureId = metalNone, .roughnessTextureId = roughHigh});
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(
            cube, _physics.AddBox(pos, {1.f, 0.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  Assisi::Physics::BodyMotion::Dynamic));
    }

    // --- Test cube 3: semi-rough blue ceramic (partial metallic, medium roughness) ---
    {
        constexpr glm::vec3 pos{3.f, 0.5f, -2.f};
        Assisi::ECS::Entity cube = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(cube, {.position = pos});
        std::ignore = _scene->Add<Assisi::Runtime::MeshRendererComponent>(
            cube, {.mesh = &_cubeMesh, .albedoTextureId = texBlue,
                   .metallicTextureId = metalMed, .roughnessTextureId = roughMed});
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(
            cube, _physics.AddBox(pos, {1.f, 0.f, 0.f, 0.f}, {0.5f, 0.5f, 0.5f},
                                  Assisi::Physics::BodyMotion::Dynamic));
    }

    // --- Point lights ---

    // White — overhead centre
    {
        Assisi::ECS::Entity light = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            light, {.position = {0.f, 4.f, -1.f}});
        std::ignore = _scene->Add<Assisi::Runtime::PointLightComponent>(
            light, {.color = {1.f, 1.f, 1.f}, .intensity = 150.f, .radius = 30.f});
    }

    // Blue — left side
    {
        Assisi::ECS::Entity light = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            light, {.position = {-5.f, 2.5f, 0.f}});
        std::ignore = _scene->Add<Assisi::Runtime::PointLightComponent>(
            light, {.color = {0.2f, 0.4f, 1.f}, .intensity = 200.f, .radius = 30.f});
    }

    // Red — right side
    {
        Assisi::ECS::Entity light = _scene->Create();
        std::ignore = _scene->Add<Assisi::Runtime::TransformComponent>(
            light, {.position = {5.f, 2.5f, 0.f}});
        std::ignore = _scene->Add<Assisi::Runtime::PointLightComponent>(
            light, {.color = {1.f, 0.1f, 0.1f}, .intensity = 200.f, .radius = 30.f});
    }
}

void SandboxApp::OnResize(int width, int height)
{
    _projection = MakeProjection(_fovDegrees, kNearZ, kFarZ);
    _lighting.Resize(width, height, kNearZ, kFarZ, _projection);
    _lighting.SetupMeshShader(_shader);
}

void SandboxApp::OnFixedUpdate(float dt)
{
    _physics.Update(dt);
    _physics.SyncTransforms(*_scene);
}

void SandboxApp::OnUpdate(float dt)
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

    if (input.IsMouseCaptured())
    {
        // --- Mouse rotation ---
        const glm::vec2 delta = input.MouseDelta();
        _yaw   += delta.x * kMouseSensitivity;
        _pitch -= delta.y * kMouseSensitivity;
        _pitch  = glm::clamp(_pitch, -89.f, 89.f);

        const glm::vec3 forward = {
            glm::cos(glm::radians(_pitch)) * glm::cos(glm::radians(_yaw)),
            glm::sin(glm::radians(_pitch)),
            glm::cos(glm::radians(_pitch)) * glm::sin(glm::radians(_yaw))};

        // --- WASD + Space/Ctrl movement ---
        const glm::vec3 right = _camera.RightDirection();
        glm::vec3 move{0.f};
        if (input.IsKeyDown(Assisi::Window::Key::W))           { move += forward; }
        if (input.IsKeyDown(Assisi::Window::Key::S))           { move -= forward; }
        if (input.IsKeyDown(Assisi::Window::Key::D))           { move += right; }
        if (input.IsKeyDown(Assisi::Window::Key::A))           { move -= right; }
        if (input.IsKeyDown(Assisi::Window::Key::Space))       { move.y += 1.f; }
        if (input.IsKeyDown(Assisi::Window::Key::LeftControl)) { move.y -= 1.f; }

        glm::vec3 pos = _camera.WorldPosition();
        if (glm::length(move) > 0.f)
        {
            pos += glm::normalize(move) * (kMoveSpeed * dt);
        }

        _camera.SetWorldPosition(pos);
        _camera.SetLookAtTarget(pos + forward);
    }

    // --- Scroll to adjust FOV ---
    if (!ImGui::GetIO().WantCaptureMouse)
    {
        const float scroll = input.ScrollDelta();
        if (scroll != 0.f)
        {
            _fovDegrees = glm::clamp(_fovDegrees - (scroll * 5.f), 10.f, 120.f);
            _projection = MakeProjection(_fovDegrees, kNearZ, kFarZ);
        }
    }
}

void SandboxApp::OnRender()
{
    _lighting.Update(*_scene, _camera.ViewMatrix());

    _shader.Use();
    _shader.SetVec3("uViewPos", _camera.WorldPosition());
    _shader.SetVec3("uAmbient", {0.03f, 0.03f, 0.03f});
    _shader.SetInt("uDirLightCount", static_cast<int>(_lighting.DirLightCount()));

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
    ImGui::Text("Position  %.2f  %.2f  %.2f", static_cast<double>(cubePos.x), static_cast<double>(cubePos.y), static_cast<double>(cubePos.z));
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
            newCube, {.mesh = &_cubeMesh, .albedoTextureId = 0u});
        const auto newRb = _physics.AddBox(spawnPos, _cornerRot, {0.5f, 0.5f, 0.5f},
                                           Assisi::Physics::BodyMotion::Dynamic);
        std::ignore = _scene->Add<Assisi::Physics::RigidBodyComponent>(newCube, newRb);
        ++_spawnCount;
    }
    ImGui::SameLine();
    ImGui::Text("(%d spawned)", _spawnCount);

    ImGui::SeparatorText("Hint");
    ImGui::TextDisabled("LMB: capture  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Mouse: look  |  Scroll: FOV  |  Esc: release / quit");

    ImGui::End();
}

void SandboxApp::OnShutdown()
{
    if (!_testTextures.empty())
        glDeleteTextures(static_cast<int>(_testTextures.size()), _testTextures.data());
}

// ---------------------------------------------------------------------------

int main()
{
    SandboxApp app;
    app.Run();
    return EXIT_SUCCESS;
}