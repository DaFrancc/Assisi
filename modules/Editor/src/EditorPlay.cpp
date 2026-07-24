/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Play state (Run / Pause / Stop)
// ---------------------------------------------------------------------------

void EditorApp::StartPlay()
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
                std::vector<Assisi::Editor::ComponentSnapshot> components;
                for (const auto *meta : registry.SerializableComponents())
                {
                    if (const void *comp = meta->getByEntity(_scene, entity.index, entity.generation))
                        components.push_back({meta->id, meta->serialize(comp)});
                }
                _playSnapshot.push_back({entity, std::move(components)});
            });
    }

    SetPlayState(PlayState::Playing);
    Assisi::Core::Log::Info("Play: started (scene snapshotted, {} entities).", _playSnapshot.size());
}

void EditorApp::ResumePlay()
{
    if (_playState != PlayState::Paused)
    {
        return;
    }
    // Leaving Paused: the scratch pause-history is discarded (its edits stay in the
    // scene and the simulation carries on, but they were never part of the editing
    // history and their undo does not persist).
    _pausedHistory.reset();
    SetPlayState(PlayState::Playing);
}

void EditorApp::PausePlay()
{
    if (_playState != PlayState::Playing)
    {
        return;
    }
    // Entering Paused: open a fresh scratch history so edits made while paused are
    // undoable *within the pause*, without ever touching the persistent editing
    // history. Bound to the EDITED world's scene, not whichever world is being
    // shown: that is the only one edits may be captured against, and the only one
    // guaranteed to outlive the pause (play-created worlds can be destroyed).
    if (Assisi::App::World *edited = _worlds.Edited())
    {
        _pausedHistory.emplace(edited->scene, MakeEditRebindHook());
    }
    SetPlayState(PlayState::Paused);
}

void EditorApp::StopPlay()
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

    // Everything the session created goes, and the edited world comes back into
    // view — BEFORE the restore below, which works on `_scene` and must therefore
    // already be pointed at the authored level rather than wherever play ended up.
    // A queued "load as new world" from this frame is dropped with it.
    _pendingWorldLoad.reset();
    _pendingTravel.reset();
    _pendingMigrate.reset();
    _pendingPreload.reset();
    _pendingPromote = false;
    DestroyPlayWorlds();

    // Runs unconditionally, including for an empty snapshot: entering Play on an
    // empty scene captures nothing, so gating the teardown on a non-empty snapshot
    // used to let every entity spawned during Play survive into Editing. The revive
    // loops below are already no-ops for an empty snapshot.
    {
        // Tear down the current (play-state) scene, then rebuild the snapshot at
        // EXACT identity. Destroy+flush (not Scene::Clear) keeps the registry's slot
        // table intact so ReviveAt can restore each entity's original handle; the
        // physics world is wiped and rebuilt wholesale by the rebind below, so the
        // play-state Jolt bodies need no per-entity teardown here.
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
                for (const Assisi::Editor::ComponentSnapshot &comp : snap.components)
                {
                    if (const auto *meta = registry.ById(comp.id); meta != nullptr && meta->addToScene)
                        meta->addToScene(_scene, snap.handle.index, snap.handle.generation, comp.data);
                }
            }
        }

        _selectedEntity = Assisi::ECS::NullEntity;
        Assisi::App::RebindSceneAssetsAndPhysics(*_scene, _assetCache, _assetDatabase, *_physics);
    }

    SetPlayState(PlayState::Editing);
    _playSnapshot.clear();

    Assisi::Core::Log::Info("Play: stopped (scene restored at exact identity).");
}

// ---------------------------------------------------------------------------
// Entity creation
// ---------------------------------------------------------------------------

Assisi::ECS::Entity EditorApp::CreateEntity()
{
    if (_scene == nullptr)
    {
        return Assisi::ECS::NullEntity;
    }

    // A bare entity — no components, not even a Transform. Not every entity is
    // spatial, so a Transform is opt-in: adding one (via Add Component) places the
    // entity in front of the camera (see AddComponentToSelected). The author
    // builds the entity up from here.
    const Assisi::ECS::Entity previousSelection = _selectedEntity;
    const Assisi::ECS::Entity entity            = _scene->Create();
    _selectedEntity                             = entity;

    // Capture the creation as one undoable transaction: undo destroys the bare
    // entity, redo revives it at this exact handle. Components added afterwards
    // are their own transactions (so undo peels them off before removing the entity).
    if (Assisi::Editor::EditHistory *history = ActiveHistory())
    {
        Assisi::Editor::Transaction txn;
        txn.label           = EditLabel("Create Entity", entity);
        txn.selectionBefore = previousSelection;
        txn.selectionAfter  = entity;
        txn.cmds.push_back(Assisi::Editor::EntityDelta{entity, std::nullopt, history->CaptureEntityComponents(entity)});
        history->Push(std::move(txn));
    }
    return entity;
}

std::vector<Assisi::ECS::Entity> EditorApp::GatherSubtree(Assisi::ECS::Entity root)
{
    std::vector<Assisi::ECS::Entity> result{root};
    if (_scene == nullptr)
        return result;

    // Breadth-first: for each collected entity, sweep the scene for entities whose
    // Parent points at it (no child index exists, so this scans — fine at editor
    // scale). `result` grows as we go; the index walk visits each new entry.
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const Assisi::ECS::Entity current = result[i];
        _scene->ForEachEntity(
            [&](Assisi::ECS::Entity e)
            {
                const auto *parent = _scene->Get<Assisi::Runtime::Parent>(e);
                if (parent == nullptr || parent->parent != current)
                    return;
                if (std::find(result.begin(), result.end(), e) == result.end())
                    result.push_back(e);
            });
    }
    return result;
}

void EditorApp::DeleteEntity(Assisi::ECS::Entity entity)
{
    if (_scene == nullptr || !_scene->IsAlive(entity))
    {
        return;
    }

    const std::vector<Assisi::ECS::Entity> subtree = GatherSubtree(entity);

    // Capture the whole subtree as one transaction *before* tearing anything down
    // (components must still be alive to serialize). Undo revives every entity at
    // its exact handle and restores its components (two-phase, so Parent refs
    // resolve); redo re-deletes.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    Assisi::Editor::Transaction  txn;
    if (history != nullptr)
    {
        txn.label           = EditLabel(subtree.size() > 1 ? "Delete Subtree" : "Delete Entity", entity);
        txn.selectionBefore = _selectedEntity;
        txn.selectionAfter  = Assisi::ECS::NullEntity;
        for (const Assisi::ECS::Entity e : subtree)
            txn.cmds.push_back(Assisi::Editor::EntityDelta{e, history->CaptureEntityComponents(e), std::nullopt});
    }

    // Tear down each entity's Jolt body (RigidBody is transient — never in the
    // snapshot; the undo rebuilds it from RigidBodyDescriptor via the rebind hook),
    // then queue the entity for destruction. Destroy is deferred; the slots free at
    // the frame's FlushDestroyed, ready for a later undo's ReviveAt.
    for (const Assisi::ECS::Entity e : subtree)
    {
        if (const auto *rbc = _scene->Get<Assisi::Physics::RigidBody>(e))
        {
            _physics->RemoveBody(*rbc);
            _scene->Remove<Assisi::Physics::RigidBody>(e);
        }
        _scene->Destroy(e);
    }

    if (history != nullptr)
        history->Push(std::move(txn));

    // Drop the selection if it was anywhere in the deleted subtree.
    if (std::find(subtree.begin(), subtree.end(), _selectedEntity) != subtree.end())
        _selectedEntity = Assisi::ECS::NullEntity;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void EditorApp::DrawGameControlWindow()
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

    // --- Resident worlds (multi-scene S2) -----------------------------------
    // A debug control, not a shipping feature: it stands in for the game calling
    // WorldManager, so several levels can be brought up and watched side by side
    // from a stock editor. Loading is deferred to the main-thread drain for the
    // same reason level loads are — it touches GPU resources this frame's draws
    // may already reference.
    ImGui::Separator();
    ImGui::Text("Worlds resident: %zu", _worlds.Count());

    // Only during a session. While Editing there is exactly one world — the edited
    // one — which is what keeps Play/Stop's snapshot-and-restore unambiguous, and
    // a second resident level that nothing simulates would have no restore story
    // anyway (docs/multi-scene-design-notes.md §4, S2).
    const bool canAddWorld = (playing || paused) && !_levelFiles.empty() && !_pendingWorldLoad.has_value();
    ImGui::BeginDisabled(!canAddWorld);
    if (ImGui::Button("Load as new world") && canAddWorld)
    {
        _pendingWorldLoad = "levels/" + _levelFiles[static_cast<std::size_t>(_selectedLevel)] + ".alvl";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("During play only. Loads the level selected in the Levels window into a "
                          "SECOND world alongside this one; both simulate, and the Entities panel "
                          "picks which to look at. Stop destroys every world the session created.");
    }

    // Travel: what the running game does when it changes level. Distinct from the
    // Levels window's Load, which changes what you are *editing* — this replaces
    // the world being played and never leaves Play. The edited world goes dormant
    // so Stop still restores it.
    ImGui::SameLine();
    const bool canTravel = (playing || paused) && !_levelFiles.empty() && !_pendingTravel.has_value();
    ImGui::BeginDisabled(!canTravel);
    if (ImGui::Button("Travel here") && canTravel)
    {
        _pendingTravel = "levels/" + _levelFiles[static_cast<std::size_t>(_selectedLevel)] + ".alvl";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("During play only. Changes level to the one selected in the Levels window "
                          "without leaving Play — the world you were in is retired. Stop still "
                          "returns to the level you were editing, with its undo history.");
    }

    // Async travel (S5): a background preload, then an instant swap. This is
    // "Travel here" split into two — the load runs on a worker while this world
    // keeps simulating uninterrupted, and the swap only happens when you press it.
    const bool loadingInFlight = _worlds.HasPendingLoad();
    const bool preloadReady     = _worlds.PendingLoadReady();
    const bool canPreload =
        (playing || paused) && !_levelFiles.empty() && !loadingInFlight && !_pendingPreload.has_value();
    ImGui::BeginDisabled(!canPreload);
    if (ImGui::Button("Preload") && canPreload)
    {
        _pendingPreload = "levels/" + _levelFiles[static_cast<std::size_t>(_selectedLevel)] + ".alvl";
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("During play only. Starts loading the selected level on a worker thread — "
                          "this world keeps simulating with no hitch. Press \"Swap to preloaded\" when "
                          "it is ready for an instant transition.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!loadingInFlight);
    if (ImGui::Button("Swap to preloaded") && loadingInFlight)
    {
        _pendingPromote = true; // marshalled: promotion resolves/frees GPU assets
    }
    ImGui::EndDisabled();

    if (loadingInFlight)
    {
        ImGui::SameLine();
        if (preloadReady)
            ImGui::TextDisabled("ready: %.*s", static_cast<int>(_worlds.PendingLoadPath().size()),
                                _worlds.PendingLoadPath().data());
        else
            ImGui::TextDisabled("loading %.*s...", static_cast<int>(_worlds.PendingLoadPath().size()),
                                _worlds.PendingLoadPath().data());
    }

    // Destroying the shown world needs a successor to show, and neither role may
    // be dropped on the floor — so only a non-edited world that isn't the only
    // one can go.
    Assisi::App::World *const edited = _worlds.Edited();
    const bool canDestroy = _worlds.Count() > 1 && _world != edited && edited != nullptr;
    ImGui::SameLine();
    ImGui::BeginDisabled(!canDestroy);
    if (ImGui::Button("Destroy this world") && canDestroy)
    {
        const std::string doomed = _world->name;
        SetActiveWorld(*edited);
        _worlds.Destroy(doomed);
    }
    ImGui::EndDisabled();

    // Entity migration (S4): move the selected entity + its subtree into another
    // resident world. This is a debug stand-in for what a game does in code
    // (mark the player/inventory as travelling); it lets you watch a subtree move
    // between two levels by hand.
    const bool haveSelection = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity);
    if (_worlds.Count() > 1 && haveSelection)
    {
        ImGui::SeparatorText("Migrate selection");
        _worlds.ForEach(
            [this](Assisi::App::World &target)
            {
                if (&target == _world)
                    return; // can't migrate into the world it's already in
                ImGui::PushID(target.name.c_str());
                if (ImGui::SmallButton(("-> " + target.name).c_str()))
                {
                    _pendingMigrate = target.name;
                }
                ImGui::PopID();
                ImGui::SameLine();
            });
        ImGui::NewLine();
    }

    ImGui::End();
}

void EditorApp::DrawEntityListWindow()
{
    ImGui::Begin("Entities");

    // Which resident world the panels below describe. Only drawn once a second
    // world exists.
    DrawWorldSelector();

    // Every control that mutates the scene is dead while a non-edited world is
    // shown: those are inspect-only, since the undo history and Save bind to the
    // edited world alone.
    ImGui::BeginDisabled(!IsEditable());

    // + adds a new (empty) entity in front of the camera and selects it, and asks
    // the list to scroll to its row below so the new entity comes into view.
    if (ImGui::Button("+"))
    {
        _scrollToEntity = CreateEntity();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Add a new entity");
    }

    // Delete removes the selected entity and its subtree (undoable). Disabled when
    // nothing is selected. Also on the Delete key (see OnUpdate).
    ImGui::SameLine();
    const bool canDelete = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("-"))
    {
        DeleteEntity(_selectedEntity);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canDelete)
    {
        ImGui::SetTooltip("Delete the selected entity (Del)");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("add / delete entity");
    ImGui::EndDisabled();
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
            // Bring a just-created entity into view (once), centred in the list.
            if (entity == _scrollToEntity)
            {
                ImGui::SetScrollHereY(0.5f);
                _scrollToEntity = Assisi::ECS::NullEntity;
            }
            ImGui::PopID();
        });

    if (focusRequest != Assisi::ECS::NullEntity)
    {
        FocusCameraOn(focusRequest);
    }

    ImGui::End();
}

} // namespace Assisi::Editor
