/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox — level viewer built on the Application layer.

#include <Assisi/App/Application.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Window/ActionMap.hpp>

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/DefaultMeshes.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/Texture.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// @brief Emitted when the user clicks to select (or deselect) an entity.
/// NullEntity means the click landed on empty space.
AEVENT()
struct EntitySelectionChangedEvent
{
    Assisi::ECS::Entity entity;
};

// ---------------------------------------------------------------------------
// SandboxApp
// ---------------------------------------------------------------------------

class SandboxApp : public Assisi::App::Application
{
  public:
    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnRender(Assisi::Render::RenderFrame &frame) override;
    void OnImGui() override;
    void OnResize(int width, int height) override;
    void OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo) override;

  private:
    // --- Setup ---
    void SetupCamera();
    void SetupScene();

    // --- Per-frame helpers ---
    void HandleEntityPicking();
    void UpdateCamera(float dt);

    // --- ImGui panels ---
    void DrawDiagnosticsWindow();
    void DrawLevelsWindow();
    void DrawInspector();

    // --- Inspector helpers ---
    bool EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta);
    void HandlePhysicsEditing(bool anyFieldEdited);

    /// @brief Writes an eyedropper-picked entity into the armed EntityRef field.
    void ApplyEyedropperPick(Assisi::ECS::Entity picked);

    // --- Level management ---
    void ScanLevels();
    void LoadLevel(const std::string &name);
    void SaveLevel(const std::string &name);

    Assisi::ECS::Entity PickEntity(glm::vec2 mousePos);

    // --- Systems ---
    Assisi::App::SystemRegistry _systems;
    Assisi::Window::ActionMap   _actions;

    // --- State ---
    Assisi::ECS::SceneRegistry         _scenes;
    Assisi::ECS::Scene                *_scene = nullptr;
    Assisi::Physics::PhysicsWorld      _physics;

    // --- Rendering ---
    // The engine's default scene-render path owns lighting + the mesh pipeline;
    // OnRender is a single Render() call.
    Assisi::Runtime::SceneRenderer _sceneRenderer;

    // Resolves each entity's meshPath/albedoPath to shared GPU resources; owns
    // every mesh and texture the scene draws (deduped by path).
    Assisi::Render::AssetCache _assetCache;

    Assisi::ECS::Scene  _cameraScene;
    Assisi::ECS::Entity _cameraEntity = Assisi::ECS::NullEntity;

    // Set by SetupCamera() before first use; these are just safe defaults.
    float _yaw   = 0.f;
    float _pitch = 0.f;

    static constexpr float kMoveSpeed        = 8.f;
    static constexpr float kMouseSensitivity = 0.1f;

    Assisi::ECS::Entity _selectedEntity = Assisi::ECS::NullEntity;
    bool                _wasDragging    = false;

    // Eyedropper: while armed, the next scene entity-pick is written into the
    // captured EntityRef field instead of changing the selection. The target is
    // pinned by (entity, component meta, field offset) rather than a raw pointer,
    // so a pool reallocation between arming and picking can't dangle it.
    bool                                        _eyedropperArmed       = false;
    Assisi::ECS::Entity                         _eyedropperEntity      = Assisi::ECS::NullEntity;
    const Assisi::Core::Reflect::ComponentMeta *_eyedropperMeta        = nullptr;
    std::size_t                                 _eyedropperFieldOffset = 0;

    std::vector<std::string> _levelFiles;
    int                      _selectedLevel = 0;
    char                     _saveAsName[128] = {};
};

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void SandboxApp::SetupCamera()
{
    const glm::vec3 camPos{5.f, 5.f, 10.f};
    const glm::vec3 forward = glm::normalize(-camPos);

    _pitch = glm::degrees(glm::asin(forward.y));
    _yaw   = glm::degrees(glm::atan(forward.z, forward.x));

    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
    const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

    Assisi::Runtime::TransformComponent camTransform;
    camTransform.position = camPos;
    camTransform.rotation = glm::quat_cast(glm::mat3(right, up, -forward));

    _cameraEntity = _cameraScene.Create();
    (void)_cameraScene.Add<Assisi::Runtime::TransformComponent>(_cameraEntity, camTransform);
    (void)_cameraScene.Add<Assisi::Runtime::CameraComponent>(
        _cameraEntity, Assisi::Runtime::CameraComponent{60.f, 0.1f, 200.f, true});
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SandboxApp::OnStart()
{
    // Load action bindings from game.json
    {
        const auto pathResult = Assisi::Core::AssetSystem::Resolve("game.json");
        if (pathResult)
        {
            if (std::ifstream file(pathResult.value()); file.is_open())
            {
                try
                {
                    const auto json = nlohmann::json::parse(file);
                    if (json.contains("input") && json.at("input").contains("actions"))
                        _actions.LoadFromJson(json.at("input").at("actions"));
                }
                catch (const nlohmann::json::exception &e)
                {
                    Assisi::Core::Log::Warn("Failed to parse input bindings from game.json: {}", e.what());
                }
            }
        }
    }

    auto mainScene = _scenes.Create("Main");
    if (!mainScene)
    {
        Assisi::Core::Log::Error("Failed to create the main scene; aborting startup");
        return;
    }
    _scene = *mainScene;

    SetupCamera();
    SetupScene();
    ScanLevels();

    // --- Systems ---
    _systems.Register(Assisi::App::SystemPhase::Update, "EntityPicking",
                      [this](Assisi::App::SystemContext &) { HandleEntityPicking(); });

    _systems.Register(Assisi::App::SystemPhase::Update, "CameraController",
                      [this](Assisi::App::SystemContext &ctx) { UpdateCamera(ctx.dt); })
        .After("EntityPicking");

    _systems.Register(Assisi::App::SystemPhase::PostUpdate, "ProcessEntitySelection",
                      [this](Assisi::App::SystemContext &ctx)
                      {
                          for (const auto &e : ctx.events.Read<EntitySelectionChangedEvent>())
                          {
                              _selectedEntity = e.entity;
                          }
                      });
}

void SandboxApp::SetupScene()
{
    auto *vulkanContext = Assisi::Render::RenderSystem::GetVulkanContext();
    if (!vulkanContext)
    {
        return;
    }

    nvrhi::IDevice *device = vulkanContext->GetDevice();

    const auto fbSize = GetWindow().GetFramebufferSize();
    const auto *cam = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);

    // The engine's default scene-render path owns lighting + the mesh pipeline.
    // Built against GetSceneFramebufferInfo() rather than the swapchain's own
    // FramebufferInfo so it's already correct if options.json saved an MSAA mode.
    if (!_sceneRenderer.Initialize(device, GetSceneFramebufferInfo(), fbSize.Width, fbSize.Height, *cam))
    {
        RequestClose();
        return;
    }

    _assetCache.Initialize(device);
}

void SandboxApp::OnResize(int width, int height)
{
    const auto *cam = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
    if (cam == nullptr)
    {
        return;
    }
    _sceneRenderer.Resize(width, height, *cam);
}

void SandboxApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer.OnRenderTargetsChanged(framebufferInfo))
    {
        Assisi::Core::Log::Error("Failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void SandboxApp::OnRender(Assisi::Render::RenderFrame &frame)
{
    if (!_sceneRenderer.IsValid() || !_scene)
    {
        return;
    }

    // The camera lives in its own scene, so propagate it here; SceneRenderer
    // propagates the game scene it draws.
    Assisi::Runtime::PropagateTransforms(_cameraScene);
    const auto *camTransform = _cameraScene.Get<Assisi::Runtime::TransformComponent>(_cameraEntity);
    const auto *cam          = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);

    _sceneRenderer.Render(frame, *_scene, *camTransform, *cam);
}

void SandboxApp::OnFixedUpdate(float dt)
{
    _physics.Update(dt);
    _physics.SyncTransforms(*_scene);
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

namespace
{
// The ImGui context may not exist yet (before DebugUI initializes, or when the
// debug UI is disabled) — calling ImGui::GetIO() without a context asserts, so
// gate every query on GetCurrentContext() first.
bool ImGuiWantsMouse()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiWantsKeyboard()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}
} // namespace

void SandboxApp::HandleEntityPicking()
{
    auto &input = GetInput();
    if (_actions.IsActionPressed("Select", input) &&
        !input.IsMouseCaptured() && !ImGuiWantsMouse())
    {
        const Assisi::ECS::Entity picked = PickEntity(input.MousePosition());

        // An armed eyedropper consumes the click to fill its EntityRef field
        // rather than moving the selection.
        if (_eyedropperArmed)
        {
            ApplyEyedropperPick(picked);
            _eyedropperArmed = false;
            _eyedropperMeta  = nullptr;
        }
        else
        {
            GetEvents().Push(EntitySelectionChangedEvent{picked});
        }
    }
}

void SandboxApp::ApplyEyedropperPick(Assisi::ECS::Entity picked)
{
    if (!_eyedropperMeta || !_scene || !_scene->IsAlive(_eyedropperEntity))
        return;

    // The component pointer may have moved since the field was armed (the pool
    // can reallocate), so re-resolve it now and write straight into the
    // reflected offset.
    const void *ptr =
        _eyedropperMeta->getByEntity(_scene, _eyedropperEntity.index, _eyedropperEntity.generation);
    if (!ptr)
        return;

    auto *field = reinterpret_cast<Assisi::ECS::Entity *>(
        const_cast<char *>(static_cast<const char *>(ptr)) + _eyedropperFieldOffset);
    *field = picked;
}

void SandboxApp::UpdateCamera(float dt)
{
    auto      &input          = GetInput();
    const bool imguiWantsMouse = ImGuiWantsMouse();

    if (_actions.IsActionPressed("LookMode", input) && !imguiWantsMouse)
        input.SetMouseCaptured(true);
    if (_actions.IsActionReleased("LookMode", input))
        input.SetMouseCaptured(false);

    if (input.IsMouseCaptured())
    {
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

        auto *camTransform =
            _cameraScene.Get<Assisi::Runtime::TransformComponent>(_cameraEntity);

        glm::vec3 move{0.f};
        if (_actions.IsActionDown("MoveForward",  input)) { move += forward; }
        if (_actions.IsActionDown("MoveBackward", input)) { move -= forward; }
        if (_actions.IsActionDown("MoveRight",    input)) { move += right; }
        if (_actions.IsActionDown("MoveLeft",     input)) { move -= right; }
        if (_actions.IsActionDown("MoveUp",       input)) { move.y += 1.f; }
        if (_actions.IsActionDown("MoveDown",     input)) { move.y -= 1.f; }

        if (glm::length(move) > 0.f)
            camTransform->position += glm::normalize(move) * (kMoveSpeed * dt);

        camTransform->rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    }

    if (!imguiWantsMouse)
    {
        const float scroll = input.ScrollDelta();
        if (scroll != 0.f)
        {
            auto *cam       = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
            cam->fovDegrees = glm::clamp(cam->fovDegrees - (scroll * 5.f), 10.f, 120.f);
        }
    }
}

void SandboxApp::OnUpdate(float dt)
{
    auto &input = GetInput();
    if (input.IsKeyPressed(Assisi::Window::Key::Escape) && !ImGuiWantsKeyboard())
        RequestClose();

    _systems.Run(Assisi::App::SystemPhase::Update,    {*_scene, dt, input, _actions, GetEvents()});
    _systems.Run(Assisi::App::SystemPhase::PostUpdate, {*_scene, dt, input, _actions, GetEvents()});
}

// ---------------------------------------------------------------------------
// ImGui panels
// ---------------------------------------------------------------------------

void SandboxApp::DrawDiagnosticsWindow()
{
    ImGui::Begin("Diagnostics");
    ImGui::Text("FPS: %d", GetFps());
    ImGui::Separator();
    ImGui::TextDisabled("RMB: look  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Scroll: FOV  |  LMB: select  |  Esc: quit");
    ImGui::TextDisabled("F12: graphics settings");
    ImGui::End();
}

void SandboxApp::DrawLevelsWindow()
{
    ImGui::Begin("Levels");

    if (ImGui::Button("Refresh"))
        ScanLevels();

    if (_levelFiles.empty())
    {
        ImGui::TextDisabled("No .alvl files found in assets/levels/");
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

        const float halfW =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
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
        const std::string newName(_saveAsName);
        const auto        it = std::find(_levelFiles.begin(), _levelFiles.end(), newName);
        if (it != _levelFiles.end())
            _selectedLevel = static_cast<int>(std::distance(_levelFiles.begin(), it));
    }

    ImGui::End();
}

void SandboxApp::OnImGui()
{
    DrawDiagnosticsWindow();
    DrawLevelsWindow();
    DrawInspector();
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

bool SandboxApp::EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta)
{
    using namespace Assisi::Core::Reflect;

    bool anyEditable   = false;
    bool anyFieldEdited = false;

    for (const auto &field : meta.fields)
    {
        if (field.transient)
            continue;
        anyEditable = true;

        void *fp = static_cast<char *>(mut) + field.offset;
        ImGui::PushID(field.name.c_str());

        bool edited = false;
        switch (field.type)
        {
        case FieldType::Float:
            edited = ImGui::DragFloat(field.name.c_str(), static_cast<float *>(fp), 0.01f);
            break;
        case FieldType::Double:
            edited = ImGui::InputDouble(field.name.c_str(), static_cast<double *>(fp));
            break;
        case FieldType::Int:
        case FieldType::Int32:
            edited = ImGui::DragInt(field.name.c_str(), static_cast<int *>(fp));
            break;
        case FieldType::UInt32:
            edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_U32, fp, 1.f);
            break;
        case FieldType::Bool:
            edited = ImGui::Checkbox(field.name.c_str(), static_cast<bool *>(fp));
            break;
        case FieldType::Vec2:
            edited = ImGui::DragFloat2(field.name.c_str(), static_cast<float *>(fp), 0.01f);
            break;
        case FieldType::Vec3:
            edited = ImGui::DragFloat3(field.name.c_str(), static_cast<float *>(fp), 0.01f);
            break;
        case FieldType::Vec4:
            edited = ImGui::DragFloat4(field.name.c_str(), static_cast<float *>(fp), 0.01f);
            break;
        case FieldType::Quat:
        {
            auto     *quat  = static_cast<glm::quat *>(fp);
            glm::vec3 euler = glm::degrees(glm::eulerAngles(*quat));
            if (ImGui::DragFloat3(field.name.c_str(), &euler.x, 0.5f))
            {
                *quat  = glm::normalize(glm::quat(glm::radians(euler)));
                edited = true;
            }
            break;
        }
        case FieldType::AssetPath:
        {
            auto *ap = static_cast<Assisi::Core::AssetPath *>(fp);
            char  buf[Assisi::Core::kAssetPathMax + 1];
            ap->ToCStr(buf, sizeof(buf));
            if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
            {
                ap->Assign(buf);
                edited = true;
            }
            break;
        }
        case FieldType::EntityRef:
        {
            auto      *ref   = static_cast<Assisi::ECS::Entity *>(fp);
            const bool empty = (*ref == Assisi::ECS::NullEntity);

            char preview[32];
            if (empty)
                std::snprintf(preview, sizeof(preview), "(none)");
            else if (!_scene->IsAlive(*ref))
                std::snprintf(preview, sizeof(preview), "[%u:%u] (dangling)", ref->index, ref->generation);
            else
                std::snprintf(preview, sizeof(preview), "Entity [%u:%u]", ref->index, ref->generation);

            const bool  armedForThis = _eyedropperArmed && _eyedropperMeta == &meta &&
                                      _eyedropperFieldOffset == field.offset;
            const char *pickLabel    = armedForThis ? "Cancel" : "Pick";

            /* Eyedropper button first so it can't be pushed off the window's right
               edge by the combo's trailing label; the combo takes the rest. */
            if (ImGui::Button(pickLabel))
            {
                if (armedForThis)
                {
                    _eyedropperArmed = false;
                    _eyedropperMeta  = nullptr;
                }
                else
                {
                    _eyedropperArmed       = true;
                    _eyedropperEntity      = _selectedEntity;
                    _eyedropperMeta        = &meta;
                    _eyedropperFieldOffset = field.offset;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(armedForThis ? "Picking… left-click an entity in the scene (or click to cancel)"
                                               : "Eyedropper: then left-click an entity in the scene");

            ImGui::SameLine();
            if (ImGui::BeginCombo(field.name.c_str(), preview))
            {
                if (ImGui::Selectable("(none)", empty))
                {
                    *ref   = Assisi::ECS::NullEntity;
                    edited = true;
                }
                _scene->ForEachEntity(
                    [&](Assisi::ECS::Entity e)
                    {
                        char label[32];
                        std::snprintf(label, sizeof(label), "Entity [%u:%u]", e.index, e.generation);
                        const bool selected = (e == *ref);
                        if (ImGui::Selectable(label, selected))
                        {
                            *ref   = e;
                            edited = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    });
                ImGui::EndCombo();
            }
            break;
        }
        default:
            ImGui::TextDisabled("%s: [unsupported type]", field.name.c_str());
            break;
        }

        anyFieldEdited |= edited;
        ImGui::PopID();
    }

    if (!anyEditable)
        ImGui::TextDisabled("(runtime-only)");

    return anyFieldEdited;
}

void SandboxApp::HandlePhysicsEditing(bool anyFieldEdited)
{
    if (anyFieldEdited)
    {
        const auto *tc   = _scene->Get<Assisi::Runtime::TransformComponent>(_selectedEntity);
        const auto *rbc  = _scene->Get<Assisi::Physics::RigidBodyComponent>(_selectedEntity);
        const auto *desc = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
        if (tc && rbc)
            _physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
        if (rbc && desc)
        {
            _physics.ReshapeBox(*rbc, desc->halfExtents);
            _physics.SetBodyCCD(*rbc, desc->enableCCD);
        }
    }

    // IsAnyItemActive() is global — it fires for a drag/edit in *any* window, so
    // touching the AA combo or the Save-As field would otherwise freeze the
    // selected body to Static. Scope it to the Inspector: this runs inside the
    // Inspector's Begin/End, so IsWindowFocused (current window) is true only
    // while an Inspector widget is the one being manipulated.
    const bool nowDragging =
        ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (nowDragging != _wasDragging)
    {
        const auto *rbc =
            (_selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity))
                ? _scene->Get<Assisi::Physics::RigidBodyComponent>(_selectedEntity)
                : nullptr;

        if (rbc)
        {
            if (nowDragging)
            {
                _physics.SetBodyMotionType(*rbc, Assisi::Physics::BodyMotion::Static);
            }
            else
            {
                const auto *desc     = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
                const bool  isStatic = desc && desc->isStatic;
                _physics.SetBodyMotionType(*rbc, isStatic ? Assisi::Physics::BodyMotion::Static
                                                          : Assisi::Physics::BodyMotion::Dynamic);
                if (!isStatic)
                {
                    const auto *tc = _scene->Get<Assisi::Runtime::TransformComponent>(_selectedEntity);
                    if (tc)
                        _physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
                }
            }
        }
    }
    _wasDragging = nowDragging;
}

void SandboxApp::DrawInspector()
{
    using namespace Assisi::Core::Reflect;

    ImGui::Begin("Inspector");

    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        ImGui::TextDisabled("No entity selected.");
        ImGui::TextDisabled("Left-click an object in the scene.");
        ImGui::End();
        return;
    }

    ImGui::Text("Entity [%u:%u]", _selectedEntity.index, _selectedEntity.generation);
    ImGui::Separator();

    bool anyFieldEdited = false;

    for (const auto &meta : ComponentRegistry::Instance().All())
    {
        const void *compPtr =
            meta.getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation);

        if (!compPtr)
            continue;

        if (!ImGui::CollapsingHeader(meta.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::PushID(meta.name.c_str());
        anyFieldEdited |= EditComponentFields(const_cast<void *>(compPtr), meta);
        ImGui::PopID();
    }

    HandlePhysicsEditing(anyFieldEdited);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Level management
// ---------------------------------------------------------------------------

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

    _selectedEntity = Assisi::ECS::NullEntity;
    _physics.Clear();

    // New asset set: drop the old cache and evict the mesh pass's binding sets
    // (they key on raw texture pointers we're about to free) before re-resolving.
    _assetCache.Clear();
    _sceneRenderer.InvalidateAssetBindings();

    for (auto [e, mrc] : _scene->Query<Assisi::Runtime::MeshRendererComponent>())
    {
        mrc.mesh          = _assetCache.ResolveMesh(mrc.meshPath);
        mrc.albedoTexture = _assetCache.ResolveTexture(mrc.albedoPath);
    }

    for (auto [e, tc, desc] : _scene->Query<Assisi::Runtime::TransformComponent,
                                             Assisi::Physics::RigidBodyDescriptor>())
    {
        const auto motion = desc.isStatic ? Assisi::Physics::BodyMotion::Static
                                          : Assisi::Physics::BodyMotion::Dynamic;
        const auto rbc    = _physics.AddBox(tc.position, tc.rotation, desc.halfExtents, motion);
        if (desc.enableCCD)
            _physics.SetBodyCCD(rbc, true);
        (void)_scene->Add<Assisi::Physics::RigidBodyComponent>(e, rbc);
    }
}

// ---------------------------------------------------------------------------
// Ray picking
// ---------------------------------------------------------------------------

namespace
{

bool RayOBBIntersect(glm::vec3 origin, glm::vec3 dir, const glm::mat4 &model, float &tOut)
{
    const glm::mat4 inv   = glm::inverse(model);
    const glm::vec3 lOrig = glm::vec3(inv * glm::vec4(origin, 1.f));
    const glm::vec3 lDir  = glm::vec3(inv * glm::vec4(dir, 0.f));

    float tMin = -std::numeric_limits<float>::max();
    float tMax =  std::numeric_limits<float>::max();

    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(lDir[i]) < 1e-8f)
        {
            if (lOrig[i] < -0.5f || lOrig[i] > 0.5f)
                return false;
        }
        else
        {
            float t1 = (-0.5f - lOrig[i]) / lDir[i];
            float t2 = ( 0.5f - lOrig[i]) / lDir[i];
            if (t1 > t2)
                std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return false;
        }
    }

    if (tMax < 0.f)
        return false;

    tOut = tMin > 0.f ? tMin : tMax;
    return true;
}

} // namespace

Assisi::ECS::Entity SandboxApp::PickEntity(glm::vec2 mousePos)
{
    if (!_scene)
        return Assisi::ECS::NullEntity;

    const auto     *camTransform = _cameraScene.Get<Assisi::Runtime::TransformComponent>(_cameraEntity);
    const auto     *cam          = _cameraScene.Get<Assisi::Runtime::CameraComponent>(_cameraEntity);
    const glm::mat4 view         = Assisi::Runtime::ViewMatrix(*camTransform);
    const auto      fbSize       = GetWindow().GetFramebufferSize();
    const float     w            = static_cast<float>(fbSize.Width);
    const float     h            = static_cast<float>(fbSize.Height);
    if (w <= 0.f || h <= 0.f) // minimized/zero-size framebuffer — no valid ray
        return Assisi::ECS::NullEntity;
    const glm::mat4 projection   = Assisi::Runtime::ProjectionMatrix(*cam, w / h);

    const float     ndcX    = (2.f * mousePos.x / w) - 1.f;
    const float     ndcY    = 1.f - (2.f * mousePos.y / h);
    glm::vec4       viewDir = glm::inverse(projection) * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    viewDir.z = -1.f;
    viewDir.w =  0.f;
    const glm::vec3 rayDir    = glm::normalize(glm::vec3(glm::inverse(view) * viewDir));
    const glm::vec3 rayOrigin = camTransform->position;

    float               closestT = std::numeric_limits<float>::max();
    Assisi::ECS::Entity result   = Assisi::ECS::NullEntity;

    for (auto [e, tc] : _scene->Query<Assisi::Runtime::TransformComponent>())
    {
        float t = 0.f;
        if (RayOBBIntersect(rayOrigin, rayDir, tc.worldMatrix, t) && t < closestT)
        {
            closestT = t;
            result   = e;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------

int main()
{
    SandboxApp app;
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}