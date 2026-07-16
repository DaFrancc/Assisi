/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"
#include "SandboxImGui.hpp"

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <utility>
#include <vector>
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
    // play mode changes (physics settling, spawns, etc.). Unlike a Save()/Load()
    // round-trip, this records each entity's *exact* (index, generation) handle and
    // component JSON, so Stop can revive entities in place (Scene::ReviveAt) rather
    // than renumbering them — which is what keeps the editing undo history's stored
    // handles valid across the play session (edit -> play -> stop -> undo works).
    _playSnapshot.clear();
    {
        Assisi::Runtime::SceneSerializer::ScopedRawEntityContext rawContext(*_scene);
        const auto &registry = Assisi::Core::Reflect::ComponentRegistry::Instance();
        _scene->ForEachEntity(
            [&](Assisi::ECS::Entity entity)
            {
                std::vector<Sandbox::ComponentSnapshot> components;
                for (const auto *meta : registry.SerializableComponents())
                {
                    if (const void *comp = meta->getByEntity(_scene, entity.index, entity.generation))
                        components.push_back({meta->id, meta->serialize(comp)});
                }
                _playSnapshot.push_back({entity, std::move(components)});
            });
    }

    _playState = PlayState::Playing;
    Assisi::Core::Log::Info("Play: started (scene snapshotted, {} entities).", _playSnapshot.size());
}

void SandboxApp::ResumePlay()
{
    if (_playState != PlayState::Paused)
    {
        return;
    }
    // Leaving Paused: the scratch pause-history is discarded (its edits stay in the
    // scene and the simulation carries on, but they were never part of the editing
    // history and their undo does not persist).
    _pausedHistory.reset();
    _playState = PlayState::Playing;
}

void SandboxApp::PausePlay()
{
    if (_playState != PlayState::Playing)
    {
        return;
    }
    // Entering Paused: open a fresh scratch history so edits made while paused are
    // undoable *within the pause*, without ever touching the persistent editing
    // history. Bound to the same scene + rebind hook as the main one.
    _pausedHistory.emplace(*_scene, MakeEditRebindHook());
    _playState = PlayState::Paused;
}

void SandboxApp::StopPlay()
{
    if (_playState == PlayState::Editing || _scene == nullptr)
    {
        return;
    }

    // Discard the scratch pause-history first (whatever the pause let you undo dies
    // with the pause). The editing history is deliberately NOT cleared — the restore
    // below rebuilds entities at their exact pre-play handles, so its stored handles
    // stay valid and the pre-play edits remain undoable.
    _pausedHistory.reset();

    if (!_playSnapshot.empty())
    {
        // Tear down the current (play-state) scene, then rebuild the snapshot at
        // EXACT identity. Destroy+flush (not Scene::Clear) keeps the registry's slot
        // table intact so ReviveAt can restore each entity's original handle; the
        // physics world is wiped and rebuilt wholesale by RebindSceneAssetsAndPhysics,
        // so the play-state Jolt bodies need no per-entity teardown here.
        std::vector<Assisi::ECS::Entity> live;
        _scene->ForEachEntity([&](Assisi::ECS::Entity entity) { live.push_back(entity); });
        for (const Assisi::ECS::Entity entity : live)
            _scene->Destroy(entity);
        _scene->FlushDestroyed();

        {
            Assisi::Runtime::SceneSerializer::ScopedRawEntityContext rawContext(*_scene);
            const auto &registry = Assisi::Core::Reflect::ComponentRegistry::Instance();

            // Phase 1: revive every entity at its exact handle before any component
            // is added, so EntityRef fields (Parent) resolve against live targets.
            for (const PlayEntitySnapshot &snap : _playSnapshot)
                _scene->ReviveAt(snap.handle);

            // Phase 2: restore each entity's components from the captured JSON.
            for (const PlayEntitySnapshot &snap : _playSnapshot)
            {
                for (const Sandbox::ComponentSnapshot &comp : snap.components)
                {
                    if (const auto *meta = registry.ById(comp.id); meta != nullptr && meta->addToScene)
                        meta->addToScene(_scene, snap.handle.index, snap.handle.generation, comp.data);
                }
            }
        }

        _selectedEntity = Assisi::ECS::NullEntity;
        RebindSceneAssetsAndPhysics();
    }

    _playState = PlayState::Editing;
    _playSnapshot.clear();

    Assisi::Core::Log::Info("Play: stopped (scene restored at exact identity).");
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

    // A bare entity — no components, not even a Transform. Not every entity is
    // spatial, so a Transform is opt-in: adding one (via Add Component) places the
    // entity in front of the camera (see AddComponentToSelected). The author
    // builds the entity up from here.
    const Assisi::ECS::Entity entity = _scene->Create();
    _selectedEntity                  = entity;
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
            // Show the entity's Name if it has a non-empty one; otherwise fall back
            // to its [index:generation] id. PushID(index) keeps rows distinct even
            // when two entities share a name.
            char        label[64];
            const auto *nameComp = _scene->Get<Assisi::Runtime::Name>(entity);
            if (nameComp != nullptr && !nameComp->value.Empty())
                nameComp->value.ToCStr(label, sizeof(label));
            else
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
