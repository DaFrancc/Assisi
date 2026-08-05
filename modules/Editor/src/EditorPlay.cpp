/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Play state (Run / Pause / Stop)
// ---------------------------------------------------------------------------

void EditorApp::StartPlay(NetIntent intent)
{
    if (_playState != PlayState::Editing || _scene == nullptr)
    {
        return;
    }

    // Play-in-editor hosting sidesteps both gates below, structurally rather
    // than by exception: its clients load a temp snapshot of the scene as it is
    // *right now*, so "the level was never saved" and "the level on disk is not
    // what you are looking at" both stop being true. That is the whole reason
    // PIE needs no equivalent of the cross-machine save prompt.
    const bool playInEditor = intent == NetIntent::Host && _pieClientCount > 0;

    // Two host-side gates, checked before anything is snapshotted so a refusal
    // leaves the editor exactly where it was.
    if (intent == NetIntent::Host && !playInEditor)
    {
        // Clients load the level from disk by path. A level that has never been
        // saved has no path, and a host advertising `None` is a join that fails
        // on the other machine for a reason nobody can act on from there.
        if (HostLevelIdentity().addressing == Assisi::NetSync::LevelAddressing::None)
        {
            _netError = "save the level to host — clients load it from disk, so it has to be there.";
            Assisi::Core::Log::Warn("Editor: refusing to host — {}", _netError);
            return;
        }

        // Unsaved edits are a modal rather than a warning, because the failure
        // they cause is remote and delayed: the client's wall is somewhere else,
        // replicated bodies get corrected against geometry it cannot see, and
        // "objects bouncing off nothing" ten minutes later never gets traced
        // back to an amber label glanced past at host time.
        if (IsSceneDirty() && !_hostIgnoreDirty)
        {
            _hostPromptOpen = true;
            return;
        }
    }
    _hostIgnoreDirty = false;

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

    // The edited world's level identity, so Stop can put it back: a join
    // replaces the play scene with the *host's* level and retargets both.
    _prePlayLevelPath = _world != nullptr ? _world->levelPath : std::string{};
    _prePlaySystems   = _world != nullptr ? _world->systemNames : std::vector<std::string>{};

    SetPlayState(PlayState::Playing);
    _netIntent   = intent;
    _joinPhase   = JoinPhase::None;
    _joinElapsed = 0.f;
    Assisi::Core::Log::Info("Play: started (scene snapshotted, {} entities).", _playSnapshot.size());

    if (intent == NetIntent::Standalone)
        return;

    // What clients are told to load. PIE hands them a snapshot of this scene;
    // a plain host hands them the saved level's virtual path.
    Assisi::NetSync::LevelIdentity hostLevel;
    if (intent == NetIntent::Host)
    {
        if (playInEditor && !WritePieTempLevel(hostLevel))
        {
            _netError = "could not write the temp level for play-in-editor clients.";
            StopPlay();
            return;
        }
        if (!playInEditor)
            hostLevel = HostLevelIdentity();
    }

    // The session binds the scene it replicates by reference at construction,
    // and that scene is now the play scene — the one the editor already treats
    // as disposable.
    _netSession = std::make_unique<Assisi::NetSync::NetSession>(*_scene, _physics);

    // Triggered by hosting or joining, never by a level load: pressing Play alone
    // hashes nothing. Rescanned each time, because the content on disk may have
    // moved on since the last session.
    _contentSetHash.Reset();
    _contentSetHash.Start(Jobs());

    // Cached for the inspector, which renders a game-vetoed component as a
    // disabled checkbox with a reason. Read here rather than per frame: the list
    // is fixed for the life of a session, and the inspector redraws constantly.
    _netVetoedComponentNames = Assisi::NetSync::LoadNeverReplicateFromConfig();

    const auto port = static_cast<std::uint16_t>(_netPort);

    const bool started = intent == NetIntent::Host
                             ? _netSession->Host(port, std::move(hostLevel))
                             // Deferred: the ClientHello waits until this editor
                             // has built the host's level, because a snapshot
                             // applied against a world that does not exist yet
                             // maps NetIds onto whatever is in those slots.
                             : _netSession->Join(_netAddress.data(), port, /*deferHandshake=*/true);
    if (!started)
    {
        // Copy the reason out before dropping the session that holds it.
        _netError = _netSession->LastError();
        _netSession.reset();
        StopPlay();
        return;
    }

    _netError.clear();
    if (intent == NetIntent::Join)
    {
        _joinPhase = JoinPhase::Connecting;
    }
    else if (playInEditor)
    {
        // After Host() succeeded, never before: a client that connects to a
        // port nothing is listening on fails immediately and confusingly.
        SpawnPieClients(_pieClientCount);
    }
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
    // Not while a session is up, either role: pausing a host stops the server
    // ticking under connected clients, and pausing a client stops correction
    // application under a live stream. Neither is a state this design defines,
    // and un-pausing semantics for a networked session are a deferred design
    // rather than something to improvise here.
    if (IsNetSessionActive())
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
        _pausedHistory.emplace(edited->scene, MakeEditRebindHook(), &edited->instances);
    }
    SetPlayState(PlayState::Paused);
}

void EditorApp::StopPlay()
{
    if (_playState == PlayState::Editing || _scene == nullptr)
    {
        return;
    }

    // The session belongs to the play session — both roles, every reason for
    // stopping. First, so a client's mirrors are dropped before the restore
    // rebuilds the editing scene underneath them, and so a host stops
    // replicating a scene that is about to be torn down and rebuilt.
    ShutdownNetSession();
    // Every client this session launched goes with it, and so does the temp
    // level they were loading — "connections do not outlive the level" is the
    // price PIE pays for needing no level-transfer protocol, and a viewer left
    // running against a dead server is the worst version of paying it.
    ShutdownPieClients();
    _netIntent        = NetIntent::Standalone;
    _joinPhase        = JoinPhase::None;
    _pendingJoinBuild = false;
    _pendingStopPlay  = false;

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

        ClearSelection();
        Assisi::App::RebindSceneAssetsAndPhysics(*_scene, _assetCache, _assetDatabase, *_physics);
    }

    // A joined session loaded the *host's* level into this world and retargeted
    // both its identity and its systems. The entities are back; put those back
    // too, or Save would write the editing scene out over the host's filename.
    if (_world != nullptr && (_world->levelPath != _prePlayLevelPath || _world->systemNames != _prePlaySystems))
    {
        _world->levelPath = _prePlayLevelPath;
        (void)_worlds.ApplySystems(*_world, _prePlaySystems, _prePlayLevelPath);
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
    SelectEntity(entity, SelectMode::Replace);

    // Auto-named on create, so nobody has to think about naming until they care
    // (docs/blueprint-system-concept.md §6). It matters more than it looks: an
    // entity's name is what an override and every reference address it by, so an
    // unnamed one gets a placeholder from the serializer at save time — which is
    // stable, but says nothing. A unique-in-scene default keeps the file readable
    // and keeps two entities from racing for the same one.
    {
        std::string candidate;
        for (std::uint32_t suffix = 1;; ++suffix)
        {
            candidate = "Entity_" + std::to_string(suffix);
            bool taken = false;
            _scene->ForEachEntity(
                [&](Assisi::ECS::Entity other)
                {
                    const auto *name = _scene->Get<Assisi::Runtime::Name>(other);
                    taken = taken || (name != nullptr && name->value.View() == candidate);
                });
            if (!taken)
                break;
        }
        (void)_scene->Add<Assisi::Runtime::Name>(entity,
                                                 {Assisi::Core::ShortString{candidate}});
    }

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

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
//
// One list, `_selection`, in click order. `_selectedEntity` is its last element
// — the *active* entity — and is what the inspector, the gizmo and every older
// caller read. Keeping both is not redundancy: with two things selected, "which
// one am I editing" and "what will Delete take" are different questions, and a
// single member could only answer one of them.

void EditorApp::SelectEntity(Assisi::ECS::Entity entity, SelectMode mode)
{
    if (entity == Assisi::ECS::NullEntity)
    {
        // A click on empty space clears, whatever the modifier: a Ctrl-click on
        // nothing toggling nothing is a click that appears to do nothing at all.
        ClearSelection();
        return;
    }

    switch (mode)
    {
    case SelectMode::Replace:
        _selection.assign(1, entity);
        _selectionAnchor = entity;
        break;

    case SelectMode::Toggle:
    {
        const auto it = std::find(_selection.begin(), _selection.end(), entity);
        if (it != _selection.end())
        {
            _selection.erase(it);
            // Deselecting the active entity hands the role to whatever is still
            // selected, rather than leaving the inspector on something no longer
            // in the list.
            _selectedEntity   = _selection.empty() ? Assisi::ECS::NullEntity : _selection.back();
            _selectedInstance = 0;
            if (_selectedEntity != Assisi::ECS::NullEntity && _scene != nullptr)
            {
                if (const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(_selectedEntity))
                    _selectedInstance = tag->instanceId;
            }
            _selectionAnchor = _selectedEntity;
            return;
        }
        _selection.push_back(entity);
        _selectionAnchor = entity;
        break;
    }

    case SelectMode::Range:
    {
        // Resolved against the row order the entity list recorded while drawing.
        // With no anchor, or an anchor that is no longer on screen, there is no
        // range to describe — fall back to a plain pick rather than guessing.
        const auto from = std::find(_entityRowOrder.begin(), _entityRowOrder.end(), _selectionAnchor);
        const auto to   = std::find(_entityRowOrder.begin(), _entityRowOrder.end(), entity);
        if (from == _entityRowOrder.end() || to == _entityRowOrder.end())
        {
            _selection.assign(1, entity);
            _selectionAnchor = entity;
            break;
        }

        // The anchor stays put: shift-clicking further down replaces the range
        // rather than adding a second one, which is what lets a range be grown
        // and shrunk by clicking around.
        const auto first = from <= to ? from : to;
        const auto last  = from <= to ? to : from;
        _selection.assign(first, last + 1);
        // …and the clicked row is the active one, whichever end of the range it
        // is at.
        if (const auto it = std::find(_selection.begin(), _selection.end(), entity); it != _selection.end())
        {
            _selection.erase(it);
            _selection.push_back(entity);
        }
        break;
    }
    }

    _selectedEntity = _selection.empty() ? Assisi::ECS::NullEntity : _selection.back();

    // Selecting a member keeps its instance in view for the inspector's header;
    // selecting a loose entity clears it, so the two modes can never be half on.
    _selectedInstance = 0;
    if (_selectedEntity != Assisi::ECS::NullEntity && _scene != nullptr)
    {
        if (const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(_selectedEntity))
            _selectedInstance = tag->instanceId;
    }
}

void EditorApp::ClearSelection()
{
    _selection.clear();
    _selectedEntity   = Assisi::ECS::NullEntity;
    _selectedInstance = 0;
    _selectionAnchor  = Assisi::ECS::NullEntity;
}

bool EditorApp::IsSelected(Assisi::ECS::Entity entity) const
{
    return std::find(_selection.begin(), _selection.end(), entity) != _selection.end();
}

bool EditorApp::HasSelectedAncestor(Assisi::ECS::Entity entity) const
{
    if (_scene == nullptr)
        return false;

    // Bounded by the selection size rather than by the chain: a cycle in Parent is
    // a corrupt scene, not something to hang on.
    for (std::size_t guard = 0; guard < _selection.size() + 64; ++guard)
    {
        const auto *parent = _scene->Get<Assisi::Runtime::Parent>(entity);
        if (parent == nullptr || parent->parent == Assisi::ECS::NullEntity)
            return false;
        if (IsSelected(parent->parent))
            return true;
        entity = parent->parent;
    }
    return false;
}

void EditorApp::PruneSelection()
{
    if (_scene == nullptr)
        return;

    std::erase_if(_selection, [&](Assisi::ECS::Entity e) { return !_scene->IsAlive(e); });

    // `_selectedEntity` is also written directly — by undo restoring a selection,
    // by picking, by CreateEntity — so reconcile in both directions rather than
    // assuming the list is ahead.
    if (_selectedEntity != Assisi::ECS::NullEntity && !_scene->IsAlive(_selectedEntity))
        _selectedEntity = Assisi::ECS::NullEntity;
    if (_selectedEntity == Assisi::ECS::NullEntity)
        _selection.clear();
    else if (!IsSelected(_selectedEntity))
        _selection.assign(1, _selectedEntity);
    else if (_selection.back() != _selectedEntity)
    {
        std::erase(_selection, _selectedEntity);
        _selection.push_back(_selectedEntity);
    }

    if (_selectionAnchor != Assisi::ECS::NullEntity && !_scene->IsAlive(_selectionAnchor))
        _selectionAnchor = _selectedEntity;
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
    DeleteEntities(std::span{&entity, 1});
}

void EditorApp::DeleteSelection()
{
    // A copy: the delete rewrites `_selection` as it goes.
    const std::vector<Assisi::ECS::Entity> roots = _selection;
    DeleteEntities(roots);
}

void EditorApp::DeleteEntities(std::span<const Assisi::ECS::Entity> roots)
{
    if (_scene == nullptr)
    {
        return;
    }

    // The union of every root's subtree, deduplicated. Two selected entities
    // often stand in the same subtree — a parent and its child, both picked with
    // Ctrl — and capturing that child twice would push two EntityDeltas naming
    // one entity, so undo would revive it, then try to revive it again.
    std::vector<Assisi::ECS::Entity> doomed;
    for (const Assisi::ECS::Entity root : roots)
    {
        // A mirror is not ours to delete: the server would simply send it again,
        // and the round trip would show up as a mysterious flicker rather than as
        // a refusal. (The client *can* destroy one — gameplay runs over the play
        // world — and the apply path survives it; that is a different thing from
        // the editor offering it as an authoring action.)
        if (!_scene->IsAlive(root) || !IsEditable(root))
            continue;

        for (const Assisi::ECS::Entity e : GatherSubtree(root))
            if (std::find(doomed.begin(), doomed.end(), e) == doomed.end())
                doomed.push_back(e);
    }
    if (doomed.empty())
        return;

    // Capture everything as one transaction *before* tearing anything down
    // (components must still be alive to serialize). Undo revives every entity at
    // its exact handle and restores its components (two-phase, so Parent refs
    // resolve); redo re-deletes. One transaction for the whole gesture, so
    // deleting five entities is one Ctrl-Z rather than five.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    Assisi::Editor::Transaction  txn;
    if (history != nullptr)
    {
        const char *what = doomed.size() == 1  ? "Delete Entity"
                           : roots.size() > 1  ? "Delete Entities"
                                               : "Delete Subtree";
        txn.label           = EditLabel(what, doomed.front());
        txn.selectionBefore = _selectedEntity;
        txn.selectionAfter  = Assisi::ECS::NullEntity;
        for (const Assisi::ECS::Entity e : doomed)
            txn.cmds.push_back(Assisi::Editor::EntityDelta{e, history->CaptureEntityComponents(e), std::nullopt});
    }

    // Tear down each entity's Jolt body (RigidBody is transient — never in the
    // snapshot; the undo rebuilds it from RigidBodyDescriptor via the rebind hook),
    // then queue the entity for destruction. Destroy is deferred; the slots free at
    // the frame's FlushDestroyed, ready for a later undo's ReviveAt.
    for (const Assisi::ECS::Entity e : doomed)
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

    // Drop from the selection anything that was deleted. Destroy is deferred, so
    // IsAlive still says yes this frame — the doomed list is the only truthful
    // answer until the flush.
    std::erase_if(_selection,
                  [&](Assisi::ECS::Entity e)
                  { return std::find(doomed.begin(), doomed.end(), e) != doomed.end(); });
    if (std::find(doomed.begin(), doomed.end(), _selectedEntity) != doomed.end())
        _selectedEntity = _selection.empty() ? Assisi::ECS::NullEntity : _selection.back();
    if (std::find(doomed.begin(), doomed.end(), _selectionAnchor) != doomed.end())
        _selectionAnchor = _selectedEntity;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void EditorApp::DrawGameControlWindow()
{
    // What Run does on the network. Host and Join share this one surface
    // deliberately: both halves of the same feature, so "where do I join from?"
    // is answered "the same place you host from". The Network panel is the
    // detail/stats view, not a second place sessions start.
    struct NetModeEntry
    {
        const char  *label;
        NetIntent    intent;
        std::int32_t clients;
    };
    static constexpr std::array<NetModeEntry, 6> kNetModes{{
        {"Standalone", NetIntent::Standalone, 0},
        {"Host", NetIntent::Host, 0},
        {"Host + 1 client", NetIntent::Host, 1},
        {"Host + 2 clients", NetIntent::Host, 2},
        {"Host + 3 clients", NetIntent::Host, 3},
        {"Join…", NetIntent::Join, 0},
    }};
    _playNetSelection = std::clamp(_playNetSelection, 0, static_cast<std::int32_t>(kNetModes.size()) - 1);
    const NetModeEntry &netMode = kNetModes[static_cast<std::size_t>(_playNetSelection)];

    // The one place a play session starts, so the key and the button cannot
    // drift into meaning different things.
    const auto runOrResume = [this, &netMode]
    {
        if (_playState == PlayState::Paused)
        {
            ResumePlay();
            return;
        }
        _pieClientCount = netMode.clients;
        StartPlay(netMode.intent);
    };

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
            runOrResume();
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
    // Every world-structure control below is dead while a session is up. The
    // session binds its scene by reference at construction, so a host-side
    // Travel would either dangle that reference or keep replicating a retired
    // world, and a client-side one detonates the join contract outright. v1
    // disables them; mid-session level change is a deferred renegotiation of
    // the ServerHello level contract, not a v1 casualty.
    const bool networked = IsNetSessionActive();
    const auto netTooltip = [networked](const char *text)
    {
        if (networked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", text);
    };

    // Run starts (from editing) or resumes (from paused); greyed while playing.
    // Pause is live only while playing. Stop is live whenever a session is (playing
    // or paused).
    ImGui::BeginDisabled(!(editing || paused));
    if (ImGui::Button("Run"))
    {
        runOrResume();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.f);
    ImGui::BeginDisabled(!editing);
    if (ImGui::BeginCombo("##netmode", netMode.label))
    {
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(kNetModes.size()); ++i)
        {
            const bool selected = i == _playNetSelection;
            if (ImGui::Selectable(kNetModes[static_cast<std::size_t>(i)].label, selected))
                _playNetSelection = i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("What Run does on the network. \"Host + N\" opens N more windows of this build "
                          "that join automatically — they load a snapshot of the scene as it is now, "
                          "unsaved edits included. Stop closes them.");
    }

    // The endpoint, shown only for Join: an address field that is dead weight in
    // every other mode.
    if (netMode.intent == NetIntent::Join)
    {
        ImGui::SetNextItemWidth(130.f);
        ImGui::BeginDisabled(!editing);
        ImGui::InputText("##joinaddr", _netAddress.data(), _netAddress.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        ImGui::InputInt("Join at", &_netPort, 0, 0);
        _netPort = std::clamp(_netPort, 1, 65535);
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!playing || networked);
    if (ImGui::Button("Pause"))
    {
        PausePlay();
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up: pausing a host stops the server ticking under "
               "connected clients, and pausing a client stops correction application under a live "
               "stream. Stop instead.");

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
    const bool canAddWorld =
        (playing || paused) && !networked && !_levelFiles.empty() && !_pendingWorldLoad.has_value();
    ImGui::BeginDisabled(!canAddWorld);
    if (ImGui::Button("Load as new world") && canAddWorld)
    {
        _pendingWorldLoad = "levels/" + _levelFiles[static_cast<std::size_t>(_selectedLevel)] + ".alvl";
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up — the session replicates one scene, bound by "
               "reference when it started.");
    if (!networked && ImGui::IsItemHovered())
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
    const bool canTravel =
        (playing || paused) && !networked && !_levelFiles.empty() && !_pendingTravel.has_value();
    ImGui::BeginDisabled(!canTravel);
    if (ImGui::Button("Travel here") && canTravel)
    {
        _pendingTravel = "levels/" + _levelFiles[static_cast<std::size_t>(_selectedLevel)] + ".alvl";
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up. A host changing level under connected clients is a "
               "renegotiation of the level the handshake agreed on; that is a deferred design.");
    if (!networked && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("During play only. Changes level to the one selected in the Levels window "
                          "without leaving Play — the world you were in is retired. Stop still "
                          "returns to the level you were editing, with its undo history.");
    }

    // Seamless load (S5): background preload, then an instant swap with the assets
    // already resident (no pop-in). Two steps: 1) "Prepare" starts the load on a
    // worker while this world keeps simulating; 2) once it reads "READY", "Load
    // now" swaps instantly. The selected level is the one chosen in the Levels
    // window.
    ImGui::SeparatorText("Seamless load");

    const bool  loadingInFlight = _worlds.HasPendingLoad();
    const bool  preloadReady    = _worlds.PendingLoadReady();
    const auto  selectedLevel   = _levelFiles.empty()
                                      ? std::string{}
                                      : _levelFiles[static_cast<std::size_t>(_selectedLevel)];

    // Step 1 — Prepare. Disabled once a load is already in flight.
    const bool canPreload = (playing || paused) && !networked && !selectedLevel.empty() && !loadingInFlight &&
                            !_pendingPreload.has_value();
    ImGui::BeginDisabled(!canPreload);
    if (ImGui::Button("Prepare") && canPreload)
    {
        _pendingPreload = "levels/" + selectedLevel + ".alvl";
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up — a seamless swap is still a level change.");
    if (!networked && ImGui::IsItemHovered())
    {
        if (!(playing || paused))
            ImGui::SetTooltip("Start play first — seamless load is a during-play transition.");
        else if (selectedLevel.empty())
            ImGui::SetTooltip("Pick a level in the Levels window first.");
        else
            ImGui::SetTooltip("Start loading '%s' in the background. This world keeps running.",
                              selectedLevel.c_str());
    }

    // Step 2 — Load now. Prominent and enabled ONLY when the preload is fully
    // ready (deserialized AND assets streamed in), so pressing it is a clean swap.
    ImGui::SameLine();
    ImGui::BeginDisabled(!preloadReady || networked);
    if (ImGui::Button("Load now") && preloadReady && !networked)
    {
        _pendingPromote = true; // marshalled: promotion touches GPU state
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up — a seamless swap is still a level change.");
    if (!networked && ImGui::IsItemHovered() && !preloadReady)
    {
        ImGui::SetTooltip(loadingInFlight ? "Enabled once the preload reaches READY (100%%)."
                                          : "Press \"Prepare\" first to start a background load.");
    }

    // Cancel an in-flight/ready preload (throws the loaded world away).
    if (loadingInFlight)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##preload"))
            _worlds.CancelPendingLoad();
    }

    // Status line under the buttons: idle / preparing NN% / READY.
    if (!loadingInFlight)
    {
        ImGui::TextDisabled("Status: idle — pick a level, then Prepare.");
    }
    else if (preloadReady)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "Status: READY — press \"Load now\" (%.*s)",
                           static_cast<int>(_worlds.PendingLoadPath().size()), _worlds.PendingLoadPath().data());
    }
    else
    {
        const int32_t pct = static_cast<int32_t>(_worlds.PendingLoadProgress() * 100.f + 0.5f);
        ImGui::Text("Status: preparing %.*s...  %d%%",
                    static_cast<int>(_worlds.PendingLoadPath().size()), _worlds.PendingLoadPath().data(), pct);
        // A visual bar under the text for good measure.
        ImGui::ProgressBar(_worlds.PendingLoadProgress(), ImVec2(-1.f, 0.f));
    }

    // Destroying the shown world needs a successor to show, and neither role may
    // be dropped on the floor — so only a non-edited world that isn't the only
    // one can go.
    Assisi::App::World *const edited = _worlds.Edited();
    const bool canDestroy = _worlds.Count() > 1 && _world != edited && edited != nullptr && !networked;
    ImGui::SameLine();
    ImGui::BeginDisabled(!canDestroy);
    if (ImGui::Button("Destroy this world") && canDestroy)
    {
        const std::string doomed = _world->name;
        SetActiveWorld(*edited);
        _worlds.Destroy(doomed);
    }
    ImGui::EndDisabled();
    netTooltip("Not while a network session is up — the session holds one of these worlds by reference.");

    // Entity migration (S4): move the selected entity + its subtree into another
    // resident world. This is a debug stand-in for what a game does in code
    // (mark the player/inventory as travelling); it lets you watch a subtree move
    // between two levels by hand.
    const bool haveSelection = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity);
    if (_worlds.Count() > 1 && haveSelection && !networked)
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

    // Delete removes every selected entity and its subtree (undoable). Disabled
    // when nothing is selected. Also on the Delete key (see OnUpdate).
    ImGui::SameLine();
    const bool canDelete = _selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity) &&
                           IsEditable(_selectedEntity);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("-"))
    {
        DeleteSelection();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && canDelete)
    {
        ImGui::SetTooltip(_selection.size() > 1 ? "Delete the selected entities (Del)"
                                                : "Delete the selected entity (Del)");
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

    // The rows this pass draws, in draw order — what a Shift-range walks through.
    // Rebuilt rather than cached: rows come and go with the scene and with which
    // instances are expanded, and a range resolved against a stale order would
    // select entities that are not between the two rows the user clicked.
    _entityRowOrder.clear();
    _pendingRangeTarget = Assisi::ECS::NullEntity;

    // Members are gathered per instance and drawn under a collapsible row instead
    // of loose in the list, because a level of forty cars is two hundred rows of
    // "body", "wheel_fl", "wheel_fl", … otherwise. Built each frame: membership is
    // a query, and a cached list is the stored member list this design refuses.
    std::map<std::uint32_t, std::vector<Assisi::ECS::Entity>> members;
    for (auto [entity, tag] : _scene->Query<Assisi::ECS::BlueprintMember>())
        members[tag.instanceId].push_back(entity);

    const auto drawEntityRow = [&](Assisi::ECS::Entity entity)
    {
            _entityRowOrder.push_back(entity);

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
            // Mirrors are tinted, because "why can't I move this one" should be
            // answerable by looking rather than by clicking.
            const bool mirrored = _scene->Has<Assisi::NetSync::Mirrored>(entity);
            if (mirrored)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f, 0.75f, 1.f, 1.f});

            const bool selected = IsSelected(entity);
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                // Ctrl picks one more (or drops one); Shift takes everything
                // between the last plain pick and here. A range needs the whole
                // row order, and half of it has not been drawn yet — so note the
                // click and resolve it below.
                if (ImGui::GetIO().KeyShift)
                    _pendingRangeTarget = entity;
                else
                    SelectEntity(entity, ImGui::GetIO().KeyCtrl ? SelectMode::Toggle : SelectMode::Replace);

                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    // Defer the focus: it reads the transform and starts an
                    // animation, so keep it out of the scan.
                    focusRequest = entity;
                }
            }
            if (mirrored)
            {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Mirrored — the host owns this entity. Read-only here.");
            }

            // Bring a just-created entity into view (once), centred in the list.
            if (entity == _scrollToEntity)
            {
                ImGui::SetScrollHereY(0.5f);
                _scrollToEntity = Assisi::ECS::NullEntity;
            }
            ImGui::PopID();
    };

    // Loose entities first: an instance is a group and reads better as one block
    // than interleaved with whatever happens to sit between its members in the
    // scan.
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity entity)
        {
            if (!_scene->Has<Assisi::ECS::BlueprintMember>(entity))
                drawEntityRow(entity);
        });

    for (const auto &[instanceId, instanceMembers] : members)
    {
        const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(instanceId);

        char header[128];
        std::snprintf(header, sizeof(header), "%s (%s)",
                      row != nullptr && !row->name.empty() ? row->name.c_str() : "instance",
                      row != nullptr ? row->source.c_str() : "?");

        ImGui::PushID(static_cast<int32_t>(0x8000'0000u | instanceId));

        // The row itself selects the *instance*: the gizmo then moves the whole
        // group and writes its placement, recording no member overrides. Expanding
        // it and clicking a member is the other mode.
        const bool instanceSelected = _selectedInstance == instanceId && _selectedEntity == Assisi::ECS::NullEntity;
        ImGuiTreeNodeFlags flags    = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (instanceSelected)
            flags |= ImGuiTreeNodeFlags_Selected;

        const bool open = ImGui::TreeNodeEx(header, flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            // Instance mode is exclusive: the gizmo moves the group and writes the
            // placement, which is not a thing that composes with a handful of
            // loose entities also being selected.
            ClearSelection();
            _selectedInstance = instanceId;
        }
        if (open)
        {
            for (const Assisi::ECS::Entity member : instanceMembers)
                drawEntityRow(member);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // Now that every row has been drawn, both ends of a Shift-range are known.
    if (_pendingRangeTarget != Assisi::ECS::NullEntity)
    {
        SelectEntity(_pendingRangeTarget, SelectMode::Range);
        _pendingRangeTarget = Assisi::ECS::NullEntity;
    }

    if (focusRequest != Assisi::ECS::NullEntity)
    {
        FocusCameraOn(focusRequest);
    }

    ImGui::End();
}

} // namespace Assisi::Editor
