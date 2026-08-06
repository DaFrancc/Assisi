/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorWorlds.cpp
/// @brief Multiple resident levels in the editor: switching which world is shown,
///        loading a second one alongside the first, and tearing down whatever a
///        play session created. Design: docs/multi-scene-design-notes.md.

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <string>

namespace Assisi::Editor
{

void EditorApp::SetActiveWorld(Assisi::App::World &world)
{
    _worlds.SetActive(world);
    _world   = &world;
    _scene   = &world.scene;
    _physics = &world.physics;

    // Entity handles are scene-local: index 3 in this world is a different entity
    // from index 3 in the last one, and it would pass IsAlive. Anything holding a
    // handle has to be released rather than carried across.
    _selectedEntity   = Assisi::ECS::NullEntity;
    _scrollToEntity   = Assisi::ECS::NullEntity;
    _eyedropperArmed  = false;
    _eyedropperEntity = Assisi::ECS::NullEntity;
    _eyedropperMeta   = nullptr;
    _assetBrowserOpen   = false;
    _assetBrowserEntity = Assisi::ECS::NullEntity;
    _assetBrowserMeta   = nullptr;
    _pendingDeleteComponent = Assisi::Core::Reflect::kInvalidComponentId;
    _pendingDeleteEntity    = Assisi::ECS::NullEntity;
}

bool EditorApp::LoadLevelAsNewWorld(const std::string &virtualPath)
{
    if (!Assisi::Core::AssetSystem::Exists(virtualPath))
    {
        Assisi::Core::Log::Error("Load as new world: '{}' not found under the asset root.", virtualPath);
        return false;
    }

    Assisi::App::World &world = _worlds.Create("Level");

    // Keep the cache: the worlds already resident hold resolved pointers into it,
    // and assets this level shares with them are reused instead of re-uploaded.
    // (The Clear that used to live in the load path becomes a post-travel sweep —
    // docs/multi-scene-design-notes.md §0.)
    Assisi::Runtime::LevelHeader header;
    if (!Assisi::App::LoadLevel(world.scene, virtualPath, _assetCache, _assetDatabase, world.physics,
                                _sceneRenderer, Assisi::App::AssetCacheReset::Keep, &header, &world.instances))
    {
        // Destroy the half-created world rather than leaving an empty resident:
        // it holds no role yet, so this always succeeds.
        Assisi::Core::Log::Error("Load as new world: '{}' failed to load.", virtualPath);
        _worlds.Destroy(world.name);
        return false;
    }

    world.levelPath = virtualPath;
    // Same hard error as every other load: a world that cannot have the systems
    // its level names must not go Active running none of them.
    if (!_worlds.ApplySystems(world, header.systems, virtualPath))
    {
        Assisi::Core::Log::Error("Load as new world: '{}' names a system this build does not declare.",
                                 virtualPath);
        _worlds.Destroy(world.name);
        return false;
    }
    world.state     = Assisi::App::WorldState::Active;
    // A world created during play simulates immediately; one created while editing
    // stays frozen, because nothing outside the edited world has a restore story.
    world.simulate = (_playState == PlayState::Playing);

    SetActiveWorld(world);
    Assisi::Core::Log::Info("World '{}' loaded from '{}' ({} resident).", world.name, virtualPath,
                            _worlds.Count());
    return true;
}

void EditorApp::MigrateSelectionTo(const std::string &targetWorld)
{
    Assisi::App::World *const dst = _worlds.Find(targetWorld);
    if (dst == nullptr || _world == nullptr || dst == _world)
        return;
    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
        return;

    // The selection is a handle into the source world; it is about to be destroyed
    // there, so drop it. (The migrated copy in the destination has a different
    // handle; re-selecting it is the game's/inspector's job, not done here.)
    const Assisi::ECS::Entity moved = _worlds.MigrateEntity(*_world, *dst, _selectedEntity);
    ClearSelection();
    (void)moved;
}

void EditorApp::BeginPreload(const std::string &virtualPath)
{
    if (!Assisi::Core::AssetSystem::Exists(virtualPath))
    {
        Assisi::Core::Log::Error("Preload: '{}' not found under the asset root.", virtualPath);
        return;
    }
    // The load runs on a worker; the current world keeps simulating. Readiness is
    // polled in the Game panel, and the swap is a separate, deliberate step.
    _worlds.BeginLoadLevel(virtualPath);
}

void EditorApp::PromotePreloadedWorld()
{
    if (!_worlds.HasPendingLoad())
        return;

    // As with travel, discard the pause scratch history first: promotion can
    // retire the world it binds. Then swap (instant) and show the arrival.
    _pausedHistory.reset();

    Assisi::App::World *const arrived = _worlds.PromotePendingLoad();
    if (arrived == nullptr)
        return; // promotion logged the failure; nothing changed

    SetActiveWorld(*arrived);

    // Deliberately NO SweepAssetCache() here. The sweep does a full cache Clear +
    // re-import of the survivor from disk — which is exactly the streaming pop-in
    // the preload spent frames avoiding. Seamless travel keeps its pre-loaded
    // assets resident; reclaiming the retired level's GPU memory is the deferred
    // refcounted-eviction job, not something to pay for with a visible re-load
    // right after a swap that was supposed to be instant. (Synchronous "Travel
    // here" still sweeps — it has a loading-screen moment anyway.)
}

bool EditorApp::TravelToLevel(const std::string &virtualPath)
{
    if (!Assisi::Core::AssetSystem::Exists(virtualPath))
    {
        Assisi::Core::Log::Error("Travel: '{}' not found under the asset root; staying put.", virtualPath);
        return false;
    }

    // The pause scratch history binds a scene by reference and travel can destroy
    // the world holding it — discard it first, exactly as resuming does. Without
    // this, pause-then-travel is a use-after-free two clicks deep.
    _pausedHistory.reset();

    Assisi::App::World *const arrived = _worlds.LoadLevel(virtualPath);
    if (arrived == nullptr)
    {
        return false; // WorldManager logged why; play continues where it was
    }

    SetActiveWorld(*arrived);

    // The outgoing world is gone (or dormant), so this is the moment GPU memory
    // can come back. A no-op while anything else is still live.
    _worlds.SweepAssetCache();
    return true;
}

void EditorApp::DestroyPlayWorlds()
{
    Assisi::App::World *edited = _worlds.Edited();
    if (edited == nullptr)
        return;

    const std::size_t destroyed = _worlds.DestroyAllExcept(*edited);
    if (destroyed > 0)
    {
        Assisi::Core::Log::Info("Play: destroyed {} world(s) created during the session.", destroyed);
    }

    // Back to the authored level. SetActiveWorld also drops the selection, which is
    // correct: it was a handle into a world that no longer exists.
    SetActiveWorld(*edited);
    edited->state = Assisi::App::WorldState::Active;
}

void EditorApp::DrawWorldSelector()
{
    // One row, and only once there is a choice to make — a single-world session
    // should look exactly as it did before multi-scene existed.
    if (_worlds.Count() < 2)
        return;

    // Blueprint mode is a mode, not a world you happen to be looking at. Letting the
    // selector step out of it would leave the edited role behind on the blueprint —
    // so the level you switched to would be view-only and the reason would be
    // invisible. Close the blueprint to get back.
    if (InBlueprintMode())
    {
        ImGui::TextDisabled("editing %s", _world->levelPath.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Close the blueprint (Blueprint panel) to go back to the level.");
        ImGui::Separator();
        return;
    }

    ImGui::SetNextItemWidth(-120.f);
    if (ImGui::BeginCombo("##world", _world->name.c_str()))
    {
        _worlds.ForEach(
            [this](Assisi::App::World &world)
            {
                const bool selected = (&world == _world);
                if (ImGui::Selectable(world.name.c_str(), selected) && !selected)
                {
                    SetActiveWorld(world);
                }
                if (ImGui::IsItemHovered() && !world.levelPath.empty())
                {
                    ImGui::SetTooltip("%s", world.levelPath.c_str());
                }
            });
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (IsEditable())
    {
        ImGui::TextDisabled("edited");
    }
    else
    {
        // Say why the panels below are greyed out, rather than letting it look
        // like a bug.
        ImGui::TextDisabled("view only");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Only the edited world (%s) can be changed or saved.",
                              _worlds.Edited() ? _worlds.Edited()->name.c_str() : "none");
        }
    }
    ImGui::Separator();
}

} // namespace Assisi::Editor
