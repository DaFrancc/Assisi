/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorColliderBlueprint.cpp
/// @brief A model's collision in the asset browser: what it carries, on hover,
///        and "Make collider blueprint", which breaks it into child entities in
///        a blueprint beside the model.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Editor/ColliderBlueprint.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Naming.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assisi::Editor
{

namespace
{

constexpr const char *kReplacePromptId = "Replace collider blueprint?";

/// The model's file name without its folder or extension: the blueprint's root
/// is named after it.
std::string ModelStem(const std::string &modelPath)
{
    return std::filesystem::path(modelPath).stem().string();
}

} // namespace

void EditorApp::DrawModelActionsMenu(const std::string &vpath)
{
    // Writing a blueprint is authoring, which a restricted viewer never does.
    if (IsRestrictedViewer() || !ImGui::BeginPopupContextItem("##modelactions"))
    {
        return;
    }
    // Offered, but not taken, for a model already one collider: a blueprint of
    // it would gain nothing, and the tooltip says why.
    const bool breakable = CollisionInfoOf(vpath).breakable;
    if (ImGui::MenuItem("Make collider blueprint", nullptr, false, breakable))
    {
        RequestColliderBlueprint(vpath);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (breakable)
        {
            ImGui::SetTooltip("Writes %s: the model with one child entity per collision piece.",
                              ColliderBlueprintPath(vpath).c_str());
        }
        else
        {
            ImGui::SetTooltip("The model's collision is a single collider already; there is nothing to break apart.");
        }
    }
    if (ImGui::MenuItem("Extract animations"))
    {
        ExtractAnimations(vpath);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Writes each of the model's animations beside it as a clip file of its own.");
    }
    ImGui::Separator();
    DrawUseAsItems(vpath);
    ImGui::EndPopup();
}

void EditorApp::DrawColliderBlueprintButton(Assisi::Core::AssetId model)
{
    // A nil id, or a built-in like the fallback cube, names no file to break.
    const std::optional<std::string> path = model.IsNil() ? std::nullopt : _assetDatabase.PathFor(model);
    if (!path || !_assetDatabase.IdFor(*path).has_value())
    {
        return;
    }
    const bool breakable = CollisionInfoOf(*path).breakable;
    ImGui::BeginDisabled(!breakable || IsRestrictedViewer());
    if (ImGui::Button("Make collider blueprint"))
    {
        RequestColliderBlueprint(*path);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (breakable)
        {
            ImGui::SetTooltip("Writes %s: the model with one child entity per collision piece.",
                              ColliderBlueprintPath(*path).c_str());
        }
        else
        {
            ImGui::SetTooltip("%s\nA single collider already; there is nothing to break apart.",
                              CollisionInfoOf(*path).summary.c_str());
        }
    }
}

const ModelCollisionInfo &EditorApp::CollisionInfoOf(const std::string &vpath)
{
    using Infos = std::unordered_map<std::string, ModelCollisionInfo>;
    Infos::iterator found = _assetBrowserCollision.find(vpath);
    if (found != _assetBrowserCollision.end())
    {
        return found->second;
    }
    ModelCollisionInfo info{.summary = "Collision: the model cannot be read.", .breakable = false};
    if (const std::optional<Assisi::Core::AssetId> id = _assetDatabase.IdFor(vpath))
    {
        if (const std::optional<Assisi::Geometry::CollisionModel> model = _collisionSource.Load(*id))
        {
            info.summary = CollisionSummary(model->collision);
            info.breakable = CanMakeColliderBlueprint(model->collision);
        }
    }
    return _assetBrowserCollision.emplace(vpath, std::move(info)).first->second;
}

void EditorApp::RequestColliderBlueprint(const std::string &modelPath)
{
    const std::expected<std::filesystem::path, Assisi::Core::AssetError> target =
        Assisi::Core::AssetSystem::Resolve(ColliderBlueprintPath(modelPath));
    std::error_code error;
    if (target && std::filesystem::exists(*target, error))
    {
        _colliderBlueprintPrompt = modelPath;
        _colliderBlueprintPromptOpen = true;
        return;
    }
    WriteColliderBlueprint(modelPath);
}

void EditorApp::DrawColliderBlueprintPrompt()
{
    if (_colliderBlueprintPromptOpen)
    {
        ImGui::OpenPopup(kReplacePromptId);
        _colliderBlueprintPromptOpen = false;
    }
    if (!ImGui::BeginPopupModal(kReplacePromptId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::Text("%s already exists.", ColliderBlueprintPath(_colliderBlueprintPrompt).c_str());
    ImGui::TextUnformatted("Replace it with the model's collision as it is now?");
    if (ImGui::Button("Replace"))
    {
        WriteColliderBlueprint(_colliderBlueprintPrompt);
        _colliderBlueprintPrompt.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        _colliderBlueprintPrompt.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::WriteColliderBlueprint(const std::string &modelPath)
{
    const std::optional<Assisi::Core::AssetId> model = _assetDatabase.IdFor(modelPath);
    if (!model)
    {
        Assisi::Core::Log::Warn("Collider blueprint: '{}' has no id yet; reimport first.", modelPath);
        return;
    }
    const std::optional<Assisi::Geometry::CollisionModel> collision = _collisionSource.Load(*model);
    if (!collision)
    {
        return;
    }
    // The menu only offers models of several pieces; this is the backstop.
    if (!CanMakeColliderBlueprint(collision->collision))
    {
        Assisi::Core::Log::Warn("Collider blueprint: '{}' is a single collider already; nothing is written.",
                                modelPath);
        return;
    }
    const std::string source = ColliderBlueprintPath(modelPath);
    const std::expected<std::filesystem::path, Assisi::Core::AssetError> target =
        Assisi::Core::AssetSystem::Resolve(source);
    if (!target)
    {
        Assisi::Core::Log::Error("Collider blueprint: cannot resolve a path for '{}'.", source);
        return;
    }

    // Built in a scene of its own, so the open level is not touched.
    Assisi::ECS::Scene scratch;
    const Assisi::ECS::Entity root = scratch.Create();
    (void)scratch.Add(root, Assisi::ECS::Transform{});
    Assisi::Runtime::MeshRenderer renderer;
    renderer.mesh = *model;
    (void)scratch.Add(root, renderer);
    (void)Assisi::Runtime::GiveEntityName(scratch, root, ModelStem(modelPath));

    const std::vector<ColliderPart> parts = ColliderBlueprintParts(*model, collision->collision);
    std::vector<Assisi::ECS::Entity> members{root};
    for (const ColliderPart &part : parts)
    {
        const Assisi::ECS::Entity child = scratch.Create();
        (void)scratch.Add(child, Assisi::ECS::Transform{.position = part.position, .rotation = part.rotation});
        (void)scratch.Add(child, Assisi::ECS::Parent{.parent = root});
        (void)scratch.Add(child, part.collider);
        (void)Assisi::Runtime::GiveEntityName(scratch, child, part.name);
        members.push_back(child);
    }

    if (!Assisi::Runtime::SceneSerializer::SaveEntitiesToFile(scratch, members, *target, Assisi::ECS::Transform{}))
    {
        Assisi::Core::Log::Error("Collider blueprint: could not write '{}'.", source);
        return;
    }
    Assisi::Core::Log::Info("Collider blueprint: wrote '{}' with {} piece(s).", source, parts.size());

    // A previous file of the same name may still be cached, and would be
    // expanded instead of this one; a new one needs its id.
    Assisi::Runtime::InvalidateBlueprint(source);
    ReimportAssets();
    ScanBlueprints();
}

} // namespace Assisi::Editor
