/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"
#include "SandboxImGui.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// Scene rebuild helpers (shared by level load, the Stop restore, and spawning)
// ---------------------------------------------------------------------------

void SandboxApp::AddPhysicsBody(Assisi::ECS::Entity entity, const Assisi::ECS::Transform &tc,
                                const Assisi::Physics::RigidBodyDescriptor &desc)
{
    const auto motion = desc.isStatic ? Assisi::Physics::BodyMotion::Static
                                      : Assisi::Physics::BodyMotion::Dynamic;
    const Assisi::Physics::PhysicsWorld::ColliderShapeDesc shape{.shape       = desc.shape,
                                                                 .halfExtents = desc.halfExtents,
                                                                 .radius      = desc.radius,
                                                                 .halfHeight  = desc.halfHeight};
    const Assisi::Physics::RigidBody rbc = _physics.AddBody(tc.position, tc.rotation, shape, motion);
    if (desc.enableCCD)
    {
        _physics.SetBodyCCD(rbc, true);
    }
    (void)_scene->Add<Assisi::Physics::RigidBody>(entity, rbc);
}

void SandboxApp::RebindSceneAssetsAndPhysics()
{
    // The scene's entities were replaced wholesale (level load or Stop restore),
    // so every transient pointer and physics body is stale. Rebuild physics from
    // scratch and re-resolve each MeshRenderer's GPU resources from its GUIDs.
    _physics.Clear();

    for (auto [entity, mrc] : _scene->Query<Assisi::Runtime::MeshRenderer>())
    {
        ResolveMeshRendererAssets(mrc);
    }

    for (auto [entity, tc, desc] :
         _scene->Query<Assisi::Runtime::Transform, Assisi::Physics::RigidBodyDescriptor>())
    {
        AddPhysicsBody(entity, tc, desc);
    }
}

// ---------------------------------------------------------------------------
// Play state (Run / Pause / Stop)
// ---------------------------------------------------------------------------

void SandboxApp::StartPlay()
{
    if (_playState != PlayState::Editing || _scene == nullptr)
    {
        return;
    }
    // Snapshot the whole scene so Stop can restore it exactly, discarding whatever
    // play mode changes (physics settling, spawns, etc.).
    _playSnapshot = Assisi::Runtime::SceneSerializer::Save(*_scene).dump();
    _playState    = PlayState::Playing;
    Assisi::Core::Log::Info("Play: started (scene snapshotted).");
}

void SandboxApp::ResumePlay()
{
    if (_playState != PlayState::Paused)
    {
        return;
    }
    _playState = PlayState::Playing;
}

void SandboxApp::PausePlay()
{
    if (_playState != PlayState::Playing)
    {
        return;
    }
    _playState = PlayState::Paused;
}

void SandboxApp::StopPlay()
{
    if (_playState == PlayState::Editing || _scene == nullptr)
    {
        return;
    }

    // Restore the pre-play snapshot. Load() clears the scene and rebuilds fresh
    // entities, so the transient GPU pointers and physics bodies are gone and get
    // rebuilt from the durable components — same as a level load, minus the
    // asset-cache clear: the asset set never changed, so the cached meshes,
    // materials, and their binding sets are all still valid.
    if (!_playSnapshot.empty())
    {
        const nlohmann::json snapshot =
            nlohmann::json::parse(_playSnapshot, nullptr, /*allow_exceptions=*/false);
        if (snapshot.is_discarded())
        {
            Assisi::Core::Log::Error("Play: stop could not parse the scene snapshot; scene left as-is.");
        }
        else
        {
            Assisi::Runtime::SceneSerializer::Load(*_scene, snapshot);
            _selectedEntity = Assisi::ECS::NullEntity;
            RebindSceneAssetsAndPhysics();
        }
    }

    _playState = PlayState::Editing;
    _playSnapshot.clear();
    Assisi::Core::Log::Info("Play: stopped (scene restored).");
}

// ---------------------------------------------------------------------------
// Entity creation
// ---------------------------------------------------------------------------

Assisi::ECS::Entity SandboxApp::CreateEntity()
{
    if (_scene == nullptr)
    {
        return Assisi::ECS::NullEntity;
    }

    // An empty object carrying just a Transform, placed a few units ahead of the
    // camera so it lands in view; the author builds it up via Add Component.
    RefreshCameraMatrix();
    const glm::vec3 forward = glm::normalize(_cameraTransform.rotation * glm::vec3(0.f, 0.f, -1.f));
    constexpr float kSpawnDistance = 5.f;

    const Assisi::ECS::Entity entity = _scene->Create();

    Assisi::ECS::Transform tc;
    tc.position = _cameraTransform.position + forward * kSpawnDistance;
    (void)_scene->Add<Assisi::ECS::Transform>(entity, tc);

    _selectedEntity = entity;
    return entity;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void SandboxApp::DrawGameControlWindow()
{
    // F5 run/resume, F6 pause, F7 stop — handled here so the keys live with the
    // window that owns them (same pattern as F11 in DrawOptionsWindow). Each
    // transition method no-ops unless the current state allows it, so a keypress
    // in the wrong state simply does nothing. Guard on ImGuiWantsKeyboard so the
    // keys don't fire while a text field has focus.
    if (!ImGuiWantsKeyboard())
    {
        Assisi::Window::InputContext &input = GetInput();
        if (input.IsKeyPressed(Assisi::Window::Key::F5))
        {
            if (_playState == PlayState::Paused)
            {
                ResumePlay();
            }
            else
            {
                StartPlay();
            }
        }
        if (input.IsKeyPressed(Assisi::Window::Key::F6))
        {
            PausePlay();
        }
        if (input.IsKeyPressed(Assisi::Window::Key::F7))
        {
            StopPlay();
        }
    }

    ImGui::Begin("Game");

    const bool editing = _playState == PlayState::Editing;
    const bool playing = _playState == PlayState::Playing;
    const bool paused  = _playState == PlayState::Paused;

    // Run starts (from editing) or resumes (from paused); greyed while playing.
    // Pause is live only while playing. Stop is live whenever a session is (playing
    // or paused).
    ImGui::BeginDisabled(!(editing || paused));
    if (ImGui::Button("Run"))
    {
        if (paused)
        {
            ResumePlay();
        }
        else
        {
            StartPlay();
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (ImGui::Button("Pause"))
    {
        PausePlay();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!(playing || paused));
    if (ImGui::Button("Stop"))
    {
        StopPlay();
    }
    ImGui::EndDisabled();

    const char *stateText = "Editing";
    switch (_playState)
    {
    case PlayState::Editing:
        stateText = "Editing";
        break;
    case PlayState::Playing:
        stateText = "Playing";
        break;
    case PlayState::Paused:
        stateText = "Paused";
        break;
    }
    ImGui::Text("State: %s", stateText);
    ImGui::TextDisabled("F5 run  |  F6 pause  |  F7 stop");

    ImGui::End();
}

void SandboxApp::DrawEntityListWindow()
{
    ImGui::Begin("Entities");

    // + adds a new (empty) entity in front of the camera and selects it.
    if (ImGui::Button("+"))
    {
        (void)CreateEntity();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Add a new entity");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("add entity");
    ImGui::Separator();

    // Every alive entity, one selectable row. A single click selects it (the
    // inspector follows _selectedEntity); a double click also flies the camera to
    // frame it. AllowDoubleClick makes Selectable fire on both, so the double
    // click is distinguished by IsMouseDoubleClicked.
    Assisi::ECS::Entity focusRequest = Assisi::ECS::NullEntity;
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity entity)
        {
            char label[48];
            std::snprintf(label, sizeof(label), "Entity [%u:%u]", entity.index, entity.generation);

            ImGui::PushID(static_cast<int32_t>(entity.index));
            const bool selected = (entity == _selectedEntity);
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                _selectedEntity = entity;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    // Defer the focus: it reads the transform and starts an
                    // animation, so keep it out of the ForEachEntity scan.
                    focusRequest = entity;
                }
            }
            ImGui::PopID();
        });

    if (focusRequest != Assisi::ECS::NullEntity)
    {
        FocusCameraOn(focusRequest);
    }

    ImGui::End();
}
