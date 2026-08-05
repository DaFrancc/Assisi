/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Levels window
// ---------------------------------------------------------------------------

void EditorApp::DrawLevelsWindow()
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
        if (ImGui::BeginCombo("##level", _levelFiles[static_cast<std::size_t>(_selectedLevel)].c_str()))
        {
            for (int32_t i = 0; i < static_cast<int32_t>(_levelFiles.size()); ++i)
            {
                const bool selected = (i == _selectedLevel);
                if (ImGui::Selectable(_levelFiles[static_cast<std::size_t>(i)].c_str(), selected))
                    _selectedLevel = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const float halfW =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        // Marshal the actual load to the next frame's main-thread drain via the job
        // system — never load from OnImGui, which runs mid-frame after the scene
        // draws that bind the asset cache's bindless table are already recorded
        // (LoadLevel frees/rebuilds that table). _pendingLevelLoad marks the load
        // in-flight so the button disables (reads as "loading") and a second click
        // can't queue a duplicate; the marshalled task clears it once done.
        // Open Level is an *editing* gesture — it changes which level you are
        // working on — so it is dead during a session. (It used to silently force
        // the session back to Editing, which was worse: a click meant to load
        // something ended your play without saying so.) The play-time equivalent
        // is the Game panel's Travel, which changes level without leaving Play.
        const bool canOpen = (_playState == PlayState::Editing) && !_pendingLevelLoad.has_value();
        ImGui::BeginDisabled(!canOpen);
        if (ImGui::Button("Load", ImVec2(halfW, 0.0f)) && canOpen)
        {
            _pendingLevelLoad = _levelFiles[static_cast<std::size_t>(_selectedLevel)];
            Jobs().RunOnMain([this, name = *_pendingLevelLoad] {
                LoadLevel(name);
                _pendingLevelLoad.reset();
            });
        }
        ImGui::EndDisabled();
        if (_playState != PlayState::Editing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Stop play mode to open another level for editing "
                              "(the Game panel's Travel changes level during play).");
        ImGui::SameLine();
        // Save is only allowed while editing: during Play/Pause `*_scene` is the
        // *simulated* scene (settled physics, spawned/deleted entities), so saving
        // then would overwrite the level file with simulation state — and corrupt
        // dirty tracking after Stop restores the pre-play scene.
        // ...and only into the edited world: another resident world is someone
        // else's level, and the dirty token tracks the edited history alone.
        const bool canSave = (_playState == PlayState::Editing) && IsEditable();
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save", ImVec2(-1.0f, 0.0f)))
            SaveLevel(_levelFiles[static_cast<std::size_t>(_selectedLevel)]);
        ImGui::EndDisabled();
        if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Stop play mode to save (avoids overwriting the level with simulation state).");
    }

    ImGui::Separator();
    const bool canSaveAs = (_playState == PlayState::Editing) && IsEditable();
    ImGui::BeginDisabled(!canSaveAs);
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
            _selectedLevel = static_cast<int32_t>(std::distance(_levelFiles.begin(), it));
    }
    ImGui::EndDisabled();
    if (!canSaveAs && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop play mode to save.");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Level management
// ---------------------------------------------------------------------------

void EditorApp::ScanLevels()
{
    _levelFiles.clear();
    const auto resolved = Assisi::Core::AssetSystem::Resolve("levels");
    if (!resolved)
        return;

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(*resolved))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".alvl")
                _levelFiles.push_back(entry.path().stem().string());
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        Assisi::Core::Log::Warn("ScanLevels: cannot list levels directory '{}': {}", resolved->string(),
                                e.what());
    }
    std::sort(_levelFiles.begin(), _levelFiles.end());
    _selectedLevel = 0;
}

void EditorApp::SaveLevel(const std::string &name)
{
    const auto resolved = Assisi::Core::AssetSystem::Resolve("levels/" + name + ".alvl");
    if (!resolved)
    {
        Assisi::Core::Log::Error("SaveLevel: cannot resolve path for '{}'", name);
        return;
    }
    // Carry the world's profile back into the file. A Scene does not know it —
    // it is a property of the level — so a save that dropped it would silently
    // strip the field from every level the editor touches.
    const Assisi::Runtime::LevelHeader header{.instances = {}, .profile = _world->profile};
    if (Assisi::Runtime::SceneSerializer::SaveToFile(*_scene, *resolved, header, &_world->instances))
    {
        // Save As renames what this world *is* — keep its level identity truthful,
        // since travel and (later) the network level handshake read it.
        _world->levelPath = "levels/" + name + ".alvl";

        // Record the history position that now matches disk — IsSceneDirty compares
        // against this to drive the title's unsaved-changes marker.
        if (_history)
            _savedStateToken = _history->CurrentStateToken();
    }
}

void EditorApp::LoadLevel(const std::string &name)
{
    // The Levels UI works in bare stems (from ScanLevels); the on-disk layout is
    // levels/<name>.alvl. LoadLevelFromPath does the real work by virtual path, so
    // the command-line loader (which already has a vpath) shares it.
    LoadLevelFromPath("levels/" + name + ".alvl");
}

bool EditorApp::LoadLevelFromPath(const std::string &virtualPath)
{
    // A networked session replicates *this* scene, and a load replaces every
    // entity in it. Continuing to host across that would mean silently
    // despawning the entire world on every connected client and respawning a
    // different one; joining a host and then loading a level locally would mean
    // fighting the host over the same scene. Both are the wrong outcome, and
    // neither is something the protocol should have to express — so end the
    // session first and let the player start a new one.
    ShutdownNetSession();

    // The engine does the whole load: deserialize, drop the old asset set, evict
    // the renderer's cached bindings, re-resolve assets and rebuild physics. What
    // remains below is purely editor bookkeeping about the OLD scene. (We are at a
    // safe point — level loads are marshalled to the main-thread drain, never run
    // from OnImGui; see the Load button in DrawLevelsWindow.)
    Assisi::Runtime::LevelHeader header;
    if (!Assisi::App::LoadLevel(*_scene, virtualPath, _assetCache, _assetDatabase, *_physics, _sceneRenderer,
                                Assisi::App::AssetCacheReset::ClearFirst, &header, &_world->instances))
        return false;

    // Open Level reuses the edited world, clearing its scene in place rather than
    // creating a second one. That is what keeps the undo history's Scene& binding
    // (and every panel's) valid for the whole session — see
    // docs/multi-scene-design-notes.md §2. Only the world's level identity changes.
    _world->levelPath = virtualPath;

    // ...and its systems, which belong to the level that is now in it. This is the
    // re-target case ApplyProfile's Clear exists for: the world already holds the
    // previous level's profile.
    _worlds.ApplyProfile(*_world, header.profile);

    // A load also ends any in-progress play session: the snapshot describes the
    // old scene, so it must not survive into the new one.
    SetPlayState(PlayState::Editing);
    _playSnapshot.clear();
    _selectedEntity = Assisi::ECS::NullEntity;

    // Same aliasing hazard as the history below, and the same reason: an armed
    // eyedropper or an open asset-browser dialog holds an entity handle from the
    // OLD scene, which after the dense rebuild can resolve to a live but entirely
    // different entity — and pass IsAlive — so the pick would silently write into
    // the wrong entity's field. Disarm both.
    _eyedropperArmed  = false;
    _eyedropperEntity = Assisi::ECS::NullEntity;
    _eyedropperMeta   = nullptr;
    _assetBrowserOpen   = false;
    _assetBrowserEntity = Assisi::ECS::NullEntity;
    _assetBrowserMeta   = nullptr;

    // A fresh scene rebuilds entity identity densely from {0,0}: every handle the
    // undo stacks hold now dangles (and could alias a different entity), so wipe
    // the history. (Load's Scene::Clear already reset the registry.)
    if (_history)
        _history->Clear();
    _pausedHistory.reset(); // a load ends any play session, scratch history included
    _savedStateToken = 0;   // freshly loaded scene == on disk (empty history, token 0)

    return true;
}

} // namespace Assisi::Editor
