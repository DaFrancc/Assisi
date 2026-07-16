/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// Levels window
// ---------------------------------------------------------------------------

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
        // Marshal the actual load to the next frame's main-thread drain via the job
        // system — never load from OnImGui, which runs mid-frame after the scene
        // draws that bind the asset cache's bindless table are already recorded
        // (LoadLevel frees/rebuilds that table). _pendingLevelLoad marks the load
        // in-flight so the button disables (reads as "loading") and a second click
        // can't queue a duplicate; the marshalled task clears it once done.
        ImGui::BeginDisabled(_pendingLevelLoad.has_value());
        if (ImGui::Button("Load", ImVec2(halfW, 0.0f)))
        {
            _pendingLevelLoad = _levelFiles[_selectedLevel];
            Jobs().RunOnMain([this, name = *_pendingLevelLoad] {
                LoadLevel(name);
                _pendingLevelLoad.reset();
            });
        }
        ImGui::EndDisabled();
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

// ---------------------------------------------------------------------------
// Level management
// ---------------------------------------------------------------------------

void SandboxApp::ScanLevels()
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
    // The Levels UI works in bare stems (from ScanLevels); the on-disk layout is
    // levels/<name>.alvl. LoadLevelFromPath does the real work by virtual path, so
    // the command-line loader (which already has a vpath) shares it.
    LoadLevelFromPath("levels/" + name + ".alvl");
}

bool SandboxApp::LoadLevelFromPath(const std::string &virtualPath)
{
    if (!Assisi::Runtime::SceneSerializer::LoadFromFile(*_scene, virtualPath))
        return false;

    // A load also ends any in-progress play session: the snapshot describes the
    // old scene, so it must not survive into the new one.
    _playState = PlayState::Editing;
    _playSnapshot.clear();
    _selectedEntity = Assisi::ECS::NullEntity;

    // New asset set: drop the old cache and evict the mesh pass's binding sets
    // (they key on raw texture pointers we're about to free) before re-resolving.
    _assetCache.Clear();
    _sceneRenderer.InvalidateAssetBindings();

    RebindSceneAssetsAndPhysics();
    return true;
}
