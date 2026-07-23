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
    if (!Assisi::App::LoadLevel(world.scene, virtualPath, _assetCache, _assetDatabase, world.physics,
                                _sceneRenderer, Assisi::App::AssetCacheReset::Keep))
    {
        // Destroy the half-created world rather than leaving an empty resident:
        // it holds no role yet, so this always succeeds.
        Assisi::Core::Log::Error("Load as new world: '{}' failed to load.", virtualPath);
        _worlds.Destroy(world.name);
        return false;
    }

    world.levelPath = virtualPath;
    world.state     = Assisi::App::WorldState::Active;
    // A world created during play simulates immediately; one created while editing
    // stays frozen, because nothing outside the edited world has a restore story.
    world.simulate = (_playState == PlayState::Playing);

    SetActiveWorld(world);
    Assisi::Core::Log::Info("World '{}' loaded from '{}' ({} resident).", world.name, virtualPath,
                            _worlds.Count());
    return true;
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
