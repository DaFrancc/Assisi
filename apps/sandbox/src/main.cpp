/// @file main.cpp
/// @brief Assisi Sandbox — level viewer built on the Application layer.

#include <Assisi/App/Application.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>
#include <Assisi/Render/Shader.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

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
    void OnResize(int width, int height) override;

  private:
    void ScanLevels();
    void LoadLevel(const std::string &name);
    void SaveLevel(const std::string &name);

    Assisi::ECS::SceneRegistry         _scenes;
    Assisi::ECS::Scene                *_scene = nullptr;
    Assisi::Physics::PhysicsWorld      _physics;

    Assisi::Render::OpenGL::MeshBuffer _cubeMesh;
    Assisi::Render::Shader             _shader;
    Assisi::Runtime::LightingSystem    _lighting;
    glm::mat4                          _projection{1.f};

    // Camera entity lives in its own scene so level loads don't destroy it.
    Assisi::ECS::Scene  _cameraScene;
    Assisi::ECS::Entity _cameraEntity = Assisi::ECS::NullEntity;

    // Camera controller state (Euler angles, authoritative source for orientation).
    float _yaw   = -116.6f;
    float _pitch =  -24.1f;

    static constexpr float kMoveSpeed        = 8.f;   // units/s
    static constexpr float kMouseSensitivity = 0.1f;  // degrees/pixel

    std::vector<std::string> _levelFiles;
    int                      _selectedLevel = 0;
    char                     _saveAsName[128] = {};
};

// ---------------------------------------------------------------------------

void SandboxApp::OnStart()
{
    _scene = _scenes.Create("Main").value();

    _cubeMesh = Assisi::Render::OpenGL::MeshBuffer(Assisi::Render::CreateUnitCubeMesh());

    _shader = Assisi::Render::Shader("shaders/mesh.vert", "shaders/mesh.frag");
    if (!_shader.IsValid())
    {
        Assisi::Core::Log::Error("Failed to load mesh shader.");
        RequestClose();
        return;
    }

    // Create camera entity — lives in a separate scene so level loads don't destroy it.
    {
        const glm::vec3 camPos{5.f, 5.f, 10.f};
        const glm::vec3 forward = glm::normalize(-camPos); // look toward origin

        _pitch = glm::degrees(glm::asin(forward.y));
        _yaw   = glm::degrees(glm::atan(forward.z, forward.x));

        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
        const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

        Assisi::Runtime::TransformComponent camTransform;
        camTransform.position = camPos;
        camTransform.rotation = glm::quat_cast(glm::mat3(right, up, -forward));

        _cameraEntity = _cameraScene.Create();
        (void)_cameraScene.Add<Assisi::Runtime::TransformComponent>(_cameraEntity, camTransform);
        (void)_cameraScene.Add<Assisi::Runtime::CameraComponent>(_cameraEntity,
                                                                  Assisi::Runtime::CameraComponent{60.f, 0.1f, 200.f, true});
    }

    // Lighting system — must be initialised after the OpenGL context is ready.
    {
        const auto *cam = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
        const auto  size = GetWindow().GetFramebufferSize();
        _projection = MakeProjection(cam->fovDegrees, cam->nearZ, cam->farZ);
        if (!_lighting.Initialize(size.Width, size.Height, cam->nearZ, cam->farZ, _projection))
        {
            Assisi::Core::Log::Error("Failed to initialise LightingSystem.");
            RequestClose();
            return;
        }
        _lighting.SetupMeshShader(_shader);
    }

    ScanLevels();
}

void SandboxApp::OnResize(int width, int height)
{
    const auto *cam = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
    _projection = MakeProjection(cam->fovDegrees, cam->nearZ, cam->farZ);
    _lighting.Resize(width, height, cam->nearZ, cam->farZ, _projection);
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
            input.SetMouseCaptured(false);
        else
            RequestClose();
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

        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
        const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

        auto *camTransform = _cameraScene.Get<Assisi::Runtime::TransformComponent>(_cameraEntity);

        // --- WASD + Space/Ctrl movement ---
        glm::vec3 move{0.f};
        if (input.IsKeyDown(Assisi::Window::Key::W))           { move += forward; }
        if (input.IsKeyDown(Assisi::Window::Key::S))           { move -= forward; }
        if (input.IsKeyDown(Assisi::Window::Key::D))           { move += right; }
        if (input.IsKeyDown(Assisi::Window::Key::A))           { move -= right; }
        if (input.IsKeyDown(Assisi::Window::Key::Space))       { move.y += 1.f; }
        if (input.IsKeyDown(Assisi::Window::Key::LeftControl)) { move.y -= 1.f; }

        if (glm::length(move) > 0.f)
            camTransform->position += glm::normalize(move) * (kMoveSpeed * dt);

        camTransform->rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    }

    // --- Scroll to adjust FOV ---
    if (!ImGui::GetIO().WantCaptureMouse)
    {
        const float scroll = input.ScrollDelta();
        if (scroll != 0.f)
        {
            auto *cam = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
            cam->fovDegrees = glm::clamp(cam->fovDegrees - (scroll * 5.f), 10.f, 120.f);
            _projection = MakeProjection(cam->fovDegrees, cam->nearZ, cam->farZ);
        }
    }
}

void SandboxApp::OnRender()
{
    const auto *camTransform = _cameraScene.Get<Assisi::Runtime::TransformComponent>(_cameraEntity);
    const glm::mat4 view = Assisi::Runtime::ViewMatrix(*camTransform);

    _lighting.Update(*_scene, view);

    _shader.Use();
    _shader.SetVec3("uViewPos", camTransform->position);
    _shader.SetVec3("uAmbient", {0.03f, 0.03f, 0.03f});
    _shader.SetInt("uDirLightCount", static_cast<int>(_lighting.DirLightCount()));

    Assisi::Runtime::DrawScene(*_scene, view, _projection, _shader);
}

void SandboxApp::OnImGui()
{
    // ── Diagnostics ─────────────────────────────────────────────────────────
    ImGui::Begin("Diagnostics");
    ImGui::Text("FPS: %d", GetFps());
    ImGui::Text("Sleep resolution: %.2f ms", GetSleepResolutionMs());
    ImGui::Separator();
    ImGui::TextDisabled("LMB: capture  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Mouse: look  |  Scroll: FOV  |  Esc: release / quit");
    ImGui::End();

    // ── Level Loader ────────────────────────────────────────────────────────
    ImGui::Begin("Levels");

    if (ImGui::Button("Refresh"))
        ScanLevels();

    if (_levelFiles.empty())
    {
        ImGui::TextDisabled("No .json files found in assets/levels/");
    }
    else
    {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##level", _levelFiles[_selectedLevel].c_str()))
        {
            for (int i = 0; i < static_cast<int>(_levelFiles.size()); ++i)
            {
                const bool selected = (i == _selectedLevel);
                if (ImGui::Selectable(_levelFiles[i].c_str(), selected))
                    _selectedLevel = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Load", ImVec2(halfW, 0.0f)))
            LoadLevel(_levelFiles[_selectedLevel]);
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(-1.0f, 0.0f)))
            SaveLevel(_levelFiles[_selectedLevel]);
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Save As").x - ImGui::GetStyle().ItemSpacing.x
                            - ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputText("##saveas", _saveAsName, sizeof(_saveAsName));
    ImGui::SameLine();
    if (ImGui::Button("Save As") && _saveAsName[0] != '\0')
    {
        SaveLevel(_saveAsName);
        ScanLevels();
        // Select the newly created file in the dropdown.
        const std::string newName(_saveAsName);
        const auto it = std::find(_levelFiles.begin(), _levelFiles.end(), newName);
        if (it != _levelFiles.end())
            _selectedLevel = static_cast<int>(std::distance(_levelFiles.begin(), it));
    }

    ImGui::End();
}

void SandboxApp::ScanLevels()
{
    _levelFiles.clear();
    const auto resolved = Assisi::Core::AssetSystem::Resolve("levels");
    if (!resolved)
        return;

    for (const auto &entry : std::filesystem::directory_iterator(*resolved))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".alvl")
            _levelFiles.push_back(entry.path().stem().string());
    }
    std::sort(_levelFiles.begin(), _levelFiles.end());
    _selectedLevel = 0;
}

void SandboxApp::SaveLevel(const std::string &name)
{
    const auto resolved = Assisi::Core::AssetSystem::Resolve("levels/" + name + ".alvl");
    if (!resolved)
    {
        Assisi::Core::Log::Error("SaveLevel: cannot resolve path for '{}'", name);
        return;
    }
    Assisi::Runtime::SceneSerializer::SaveToFile(*_scene, *resolved);
}

void SandboxApp::LoadLevel(const std::string &name)
{
    if (!Assisi::Runtime::SceneSerializer::LoadFromFile(*_scene, "levels/" + name + ".alvl"))
        return;

    _physics.Clear();

    // MeshRendererComponent::mesh is transient — re-bind after load.
    for (auto [e, mrc] : _scene->Query<Assisi::Runtime::MeshRendererComponent>())
        mrc.mesh = &_cubeMesh;

    // Create Jolt bodies for entities that have a RigidBodyDescriptor.
    for (auto [e, tc, desc] : _scene->Query<Assisi::Runtime::TransformComponent,
                                             Assisi::Physics::RigidBodyDescriptor>())
    {
        const auto motion = desc.isStatic ? Assisi::Physics::BodyMotion::Static
                                          : Assisi::Physics::BodyMotion::Dynamic;
        (void)_scene->Add<Assisi::Physics::RigidBodyComponent>(
            e, _physics.AddBox(tc.position, tc.rotation, desc.halfExtents, motion));
    }
}

// ---------------------------------------------------------------------------

int main()
{
    SandboxApp app;
    app.Run();
    return EXIT_SUCCESS;
}