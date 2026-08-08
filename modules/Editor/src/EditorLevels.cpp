/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/ContentSet.hpp>
#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace Assisi::Editor
{
namespace
{

// The whole file as bytes, or nullopt if it is not there (or cannot be read —
// the two are the same answer to the only caller, which is a save asking what it
// is about to overwrite). Binary, not text: a level is UTF-8 JSON today, and a
// byte-for-byte restore must not depend on that staying true.
std::optional<std::string> ReadWholeFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Puts @p bytes back, reporting whether it worked. Failure is worth a caller's
// attention: it means the file holds contents the author declined.
bool WriteWholeFile(const std::filesystem::path &path, const std::string &bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

} // namespace

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
        // ...and into the file the edited world actually came from, which is the
        // world's own levelPath and never the combo box. The combo picks what Load
        // would open next; it is not touched by startup (LoadLevelFromPath) or by
        // travel, so reading it here wrote the open level's contents over whatever
        // file happened to be selected — and SaveLevelToPath then retargets
        // levelPath to it and marks the editor clean, so nothing warns you.
        const bool named   = IsEditable() && !_world->levelPath.empty();
        const bool canSave = (_playState == PlayState::Editing) && named;
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save", ImVec2(-1.0f, 0.0f)))
            (void)SaveLevelToPath(_world->levelPath);
        ImGui::EndDisabled();
        if (!canSave && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            // A world with no levelPath was never a file — there is nothing to save
            // *back* to, and picking a name is exactly what Save As is.
            ImGui::SetTooltip(_playState != PlayState::Editing
                                  ? "Stop play mode to save (avoids overwriting the level with "
                                    "simulation state)."
                              : IsEditable() ? "This world has never been saved — use Save As to name it."
                                             : "Only the edited world can be saved.");
        }
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
// Blueprints window
// ---------------------------------------------------------------------------

void EditorApp::DrawBlueprintsWindow()
{
    ImGui::Begin("Blueprints");

    // The one sentence that makes the panel legible: a blueprint is not a second
    // kind of thing, it is a file you place instead of copying.
    ImGui::TextWrapped("A blueprint is an ordinary level file you place copies of. Editing the file fixes "
                       "every copy on the next load.");
    ImGui::Separator();

    const bool editable = (_playState == PlayState::Editing) && IsEditable();

    if (ImGui::Button("Refresh"))
        ScanBlueprints();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu file(s)", _blueprintFiles.size());

    // --- Place an instance of an existing file ------------------------------
    ImGui::BeginDisabled(!editable);
    if (_blueprintFiles.empty())
    {
        ImGui::TextDisabled("No .abp or .alvl files found under the asset root.");
    }
    else
    {
        _selectedBlueprint =
            std::clamp(_selectedBlueprint, 0, static_cast<int32_t>(_blueprintFiles.size()) - 1);

        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##blueprint", _blueprintFiles[static_cast<std::size_t>(_selectedBlueprint)].c_str()))
        {
            for (int32_t i = 0; i < static_cast<int32_t>(_blueprintFiles.size()); ++i)
            {
                const bool selected = (i == _selectedBlueprint);
                if (ImGui::Selectable(_blueprintFiles[static_cast<std::size_t>(i)].c_str(), selected))
                    _selectedBlueprint = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Place instance", ImVec2(halfW, 0.f)))
            PlaceBlueprintInstance(_blueprintFiles[static_cast<std::size_t>(_selectedBlueprint)]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds a copy in front of the camera. Undoable.");

        ImGui::SameLine();
        // Deferred: opening loads assets and creates a world, and this runs
        // mid-frame. The safe point in OnUpdate picks it up next frame.
        if (ImGui::Button("Edit", ImVec2(-1.f, 0.f)))
            _pendingBlueprintOpen = _blueprintFiles[static_cast<std::size_t>(_selectedBlueprint)];
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Opens the file in its own world with an editor sun, so you can work on it "
                              "directly. This level stays loaded behind it, and saving brings its copies "
                              "up to date.");
        }
    }
    ImGui::EndDisabled();

    // --- Make one out of what is selected -----------------------------------
    ImGui::Separator();

    const bool haveSelection = !_selection.empty() && _scene != nullptr &&
                               _scene->IsAlive(_selectedEntity) && IsEditable(_selectedEntity);
    // Any member anywhere in the selection, not just the active one: the refusal
    // is about what would be written into the file, and every selected entity is.
    bool selectionIsMember = false;
    if (haveSelection)
    {
        for (const Assisi::ECS::Entity entity : _selection)
            selectionIsMember =
                selectionIsMember || _scene->Has<Assisi::ECS::BlueprintMember>(entity);
    }

    ImGui::BeginDisabled(!editable || !haveSelection || selectionIsMember);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Create from selection").x - ImGui::GetStyle().ItemSpacing.x -
                            ImGui::GetStyle().FramePadding.x * 2.f);
    ImGui::InputTextWithHint("##newblueprint", "name", _newBlueprintName, sizeof(_newBlueprintName));
    ImGui::SameLine();
    if (ImGui::Button("Create from selection") && _newBlueprintName[0] != '\0')
    {
        CreateBlueprintFromSelection(_newBlueprintName);
        _newBlueprintName[0] = '\0';
    }
    ImGui::EndDisabled();

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (selectionIsMember)
        {
            // Nesting is an `instances` entry, not copied entities — copying them
            // would bake the inner blueprint in and stop a fix to it propagating.
            ImGui::SetTooltip("The selection belongs to a blueprint instance. Prune it first, or nest by "
                              "adding an `instances` entry to the file by hand.");
        }
        else if (!haveSelection)
        {
            ImGui::SetTooltip("Select an entity (Ctrl-click for more). Each one and everything parented "
                              "under it becomes the blueprint.");
        }
        else
        {
            ImGui::SetTooltip("Saves the selection and its children as blueprints/<name>.abp, then replaces "
                              "them with an instance of it. Placed at the first selected entity's position "
                              "and rotation; scale stays part of the blueprint. One Ctrl-Z undoes the swap.");
        }
    }

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

void EditorApp::ScanBlueprints()
{
    _blueprintFiles = Assisi::App::ScanContentPaths();
    _selectedBlueprint = 0;
}

void EditorApp::PlaceBlueprintInstance(const std::string &source)
{
    if (_scene == nullptr || _world == nullptr || !IsEditable())
        return;

    // In front of the camera, like every other create gesture here — placing at
    // the origin means hunting for it.
    RefreshCameraMatrix();
    const glm::vec3 forward = glm::normalize(_cameraTransform.rotation * glm::vec3(0.f, 0.f, -1.f));

    Assisi::Runtime::Transform placement;
    placement.position = _cameraTransform.position + forward * 5.f;

    // A name unique in this level, because it is the prefix its members are
    // addressed by: `car_3/wheel_fl` has to name exactly one entity.
    const std::string name = Assisi::Runtime::UniqueInstanceName(
        _world->instances, std::filesystem::path(source).stem().string());

    const Assisi::Runtime::LevelInstance entry{.name      = name,
                                               .source    = source,
                                               .transform = placement,
                                               .overrides = nlohmann::json::object(),
                                               .removed   = {}};

    const auto placed = Assisi::Runtime::SceneSerializer::PlaceInstance(*_scene, _world->instances, entry,
                                                                        /*authored=*/true);
    if (!placed)
    {
        Assisi::Core::Log::Error("Editor: could not place '{}' — see the log above.", source);
        return;
    }

    // Transients the expansion does not build: the members arrived with asset ids
    // and descriptors, and need the same resolve + physics build a level load runs.
    RebuildInstanceTransients(placed->members);

    // One transaction: the record and every member. Undo takes the whole copy back
    // rather than leaving a record with no entities or entities with no record.
    if (Assisi::Editor::EditHistory *history = ActiveHistory())
    {
        Assisi::Editor::Transaction txn;
        txn.label           = "Place " + name;
        txn.selectionBefore = _selectedEntity;
        txn.selectionAfter  = Assisi::ECS::NullEntity;
        txn.cmds.push_back(Assisi::Editor::InstanceDelta{
            .instanceId = placed->instanceId, .before = std::nullopt, .after = *_world->instances.Find(placed->instanceId)});
        for (const Assisi::ECS::Entity member : placed->members)
        {
            if (member != Assisi::ECS::NullEntity)
                txn.cmds.push_back(Assisi::Editor::EntityDelta{member, std::nullopt,
                                                               history->CaptureEntityComponents(member)});
        }
        history->Push(std::move(txn));
    }

    ClearSelection();
    _selectedInstance = placed->instanceId;
}

void EditorApp::CreateBlueprintFromSelection(const std::string &name)
{
    if (_scene == nullptr || _world == nullptr || _selection.empty())
        return;

    // Every selected entity and everything under it, deduplicated: selecting a
    // parent and one of its children is an ordinary thing to do with Ctrl, and
    // writing that child twice would put two members with one name in the file.
    std::vector<Assisi::ECS::Entity> subtree;
    for (const Assisi::ECS::Entity root : _selection)
    {
        if (!_scene->IsAlive(root) || !IsEditable(root))
            continue;
        for (const Assisi::ECS::Entity entity : GatherSubtree(root))
            if (std::find(subtree.begin(), subtree.end(), entity) == subtree.end())
                subtree.push_back(entity);
    }
    if (subtree.empty())
        return;

    // The blueprint is authored around the first selected entity's pose, and the
    // instance is placed back at it — so the swap is invisible on screen, which is
    // what makes it feel like a refactor rather than an edit. The *first*, not the
    // last: with several entities selected the anchor should not depend on the
    // order they were clicked in.
    //
    // Scale is deliberately not part of that pose. A cube scaled to 0.6 and saved
    // as `small_crate` *is* a small crate — cancelling its scale into the
    // placement would leave a unit cube in the file, so the copy standing in front
    // of you looked right and every fresh one came back full size. Where a thing
    // stands and which way it faces is placement; how big it is, is what it is.
    Assisi::Runtime::Transform placement;
    if (const auto *transform = _scene->Get<Assisi::Runtime::Transform>(_selection.front()))
        placement = Assisi::Runtime::AuthoringOrigin(*transform);

    const std::string source   = "blueprints/" + name + ".abp";
    const auto        resolved = Assisi::Core::AssetSystem::Resolve(source);
    if (!resolved)
    {
        Assisi::Core::Log::Error("Editor: cannot resolve a path for '{}'.", source);
        return;
    }

    if (!Assisi::Runtime::SceneSerializer::SaveEntitiesToFile(*_scene, subtree, *resolved, placement))
        return;

    // The file is new, so nothing can have cached a definition for it — but a
    // *previous* file of the same name may be cached, and would be expanded
    // instead of what was just written.
    Assisi::Runtime::InvalidateBlueprint(source);
    ScanBlueprints();

    // Capture the originals before tearing them down: undo has to revive them, and
    // their components must still be alive to serialize.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    Assisi::Editor::Transaction  txn;
    txn.label           = "Create " + name;
    txn.selectionBefore = _selectedEntity;
    txn.selectionAfter  = Assisi::ECS::NullEntity;
    if (history != nullptr)
    {
        for (const Assisi::ECS::Entity entity : subtree)
            txn.cmds.push_back(
                Assisi::Editor::EntityDelta{entity, history->CaptureEntityComponents(entity), std::nullopt});
    }

    for (const Assisi::ECS::Entity entity : subtree)
    {
        if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(entity))
        {
            _physics->RemoveBody(*body);
            _scene->Remove<Assisi::Physics::RigidBody>(entity);
        }
        _scene->Destroy(entity);
    }
    // Now, not at end of frame: the placement below creates entities, and a
    // deferred destroy would let the new members take slots the undo needs back.
    _scene->FlushDestroyed();

    ClearSelection();

    // The same uniqueness the Blueprints panel's "Place instance" applies. The
    // typed name is the *file's*, and two selections saved as `turret.abp` in one
    // level would otherwise place two instances called `turret` — both claiming
    // `turret/…`, so the level would save and never reload (round-7 S17). The
    // file keeps the name that was typed; only the instance is stepped on.
    const std::string instanceName = Assisi::Runtime::UniqueInstanceName(_world->instances, name);

    const Assisi::Runtime::LevelInstance entry{.name      = instanceName,
                                               .source    = source,
                                               .transform = placement,
                                               .overrides = nlohmann::json::object(),
                                               .removed   = {}};

    const auto placed = Assisi::Runtime::SceneSerializer::PlaceInstance(*_scene, _world->instances, entry,
                                                                        /*authored=*/true);
    if (!placed)
    {
        // The entities are already gone. Push what we have so Ctrl-Z brings them
        // back rather than leaving the author with nothing and no way to recover.
        Assisi::Core::Log::Error("Editor: '{}' was written but could not be placed; undo to restore the "
                                 "original entities.",
                                 source);
        if (history != nullptr)
            history->Push(std::move(txn));
        return;
    }

    RebuildInstanceTransients(placed->members);

    if (history != nullptr)
    {
        txn.cmds.push_back(Assisi::Editor::InstanceDelta{
            .instanceId = placed->instanceId, .before = std::nullopt, .after = *_world->instances.Find(placed->instanceId)});
        for (const Assisi::ECS::Entity member : placed->members)
        {
            if (member != Assisi::ECS::NullEntity)
                txn.cmds.push_back(Assisi::Editor::EntityDelta{member, std::nullopt,
                                                               history->CaptureEntityComponents(member)});
        }
        history->Push(std::move(txn));
    }

    _selectedInstance = placed->instanceId;
}

void EditorApp::RebuildInstanceTransients(std::span<const Assisi::ECS::Entity> members)
{
    if (_world == nullptr)
        return;
    RebuildInstanceTransients(*_world, members);
}

void EditorApp::RebuildInstanceTransients(Assisi::App::World                  &world,
                                          std::span<const Assisi::ECS::Entity> members)
{
    for (const Assisi::ECS::Entity member : members)
    {
        if (member == Assisi::ECS::NullEntity)
            continue;
        if (auto *mesh = world.scene.Get<Assisi::Runtime::MeshRenderer>(member))
            Assisi::Runtime::ResolveMeshRendererAssets(*mesh, _assetCache, _assetDatabase);
    }

    // Propagate before building bodies, for the reason App::BuildSceneBodies
    // exists: a parented member is placed from a parent matrix that does not exist
    // until propagation has run over the entities just created.
    world.propagationTick  = Assisi::Runtime::PropagateTransforms(world.scene, world.propagationTick);
    const auto parentWorld = Assisi::App::ParentWorldResolver(world.scene);

    for (const Assisi::ECS::Entity member : members)
    {
        if (member == Assisi::ECS::NullEntity)
            continue;
        const auto *transform  = world.scene.Get<Assisi::Runtime::Transform>(member);
        const auto *descriptor = world.scene.Get<Assisi::Physics::RigidBodyDescriptor>(member);
        if (transform != nullptr && descriptor != nullptr &&
            world.scene.Get<Assisi::Physics::RigidBody>(member) == nullptr)
        {
            world.physics.AddBodyFromDescriptor(world.scene, member, *transform, *descriptor, parentWorld);
        }
    }
}

const nlohmann::json *EditorApp::OverrideClaimFor(Assisi::ECS::Entity entity, const std::string &component) const
{
    if (_scene == nullptr || _world == nullptr)
        return nullptr;

    const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(entity);
    if (tag == nullptr)
        return nullptr;

    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(tag->instanceId);
    if (row == nullptr || !row->overrides.is_object())
        return nullptr;

    const std::shared_ptr<const Assisi::Runtime::BlueprintDefinition> definition =
        Assisi::Runtime::GetBlueprintDefinition(row->source);
    if (definition == nullptr || tag->memberIndex >= definition->members.size())
        return nullptr;

    const auto member = row->overrides.find(definition->members[tag->memberIndex].name);
    if (member == row->overrides.end() || !member->is_object())
        return nullptr;

    const auto claim = member->find(component);
    return claim == member->end() ? nullptr : &*claim;
}

void EditorApp::ResetOverride(Assisi::ECS::Entity entity, const std::string &component,
                              const std::string &field)
{
    if (_scene == nullptr || _world == nullptr || !IsEditable(entity))
        return;

    const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(entity);
    if (tag == nullptr)
        return;

    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(tag->instanceId);
    if (row == nullptr)
        return;

    const std::shared_ptr<const Assisi::Runtime::BlueprintDefinition> definition =
        Assisi::Runtime::GetBlueprintDefinition(row->source);
    if (definition == nullptr || tag->memberIndex >= definition->members.size())
        return;

    const Assisi::Runtime::BlueprintMemberDesc &desc = definition->members[tag->memberIndex];

    // A copy, taken now: `row` points into the table, and RestoreAt below writes
    // through it — so reading `*row` afterwards for the transaction's "before"
    // would hand undo the value it was supposed to undo *to*, and the override
    // would look reverted while the record still claimed it.
    const Assisi::Runtime::BlueprintInstance original = *row;

    Assisi::Runtime::BlueprintInstance updated = original;
    const auto                         member  = updated.overrides.find(desc.name);
    if (member == updated.overrides.end() || !member->is_object() || !member->contains(component))
        return;

    if (field.empty())
    {
        member->erase(component);
    }
    else
    {
        auto &claim = member->at(component);
        if (!claim.is_object() || !claim.contains(field))
            return;
        claim.erase(field);
        // An empty claim is the same as no claim, and leaving one behind would
        // keep marking the component as overridden for a field nobody changed.
        if (claim.empty())
            member->erase(component);
    }
    // What is left of this member's claim on this component, read out *before*
    // the member entry can be erased — an iterator into an erased entry is not
    // something to compare against end() later.
    const nlohmann::json survivingClaim =
        member->contains(component) ? member->at(component) : nlohmann::json{};
    const bool stillClaimed = !survivingClaim.is_null();

    if (member->empty())
        updated.overrides.erase(desc.name);

    Assisi::Editor::EditHistory *history = ActiveHistory();

    const auto *meta = Assisi::Core::Reflect::ComponentRegistry::Instance().Find(component);
    if (meta == nullptr)
        return;

    // Close out any capture gesture still open on this component first.
    //
    // The inspector opens one every frame the component's block is drawn, holding
    // the value as it was *before* this frame. The reset then changes that value,
    // so the end-of-frame sweep would commit a gesture whose "after" is the
    // blueprint's own value — and record it as a fresh override. That is exactly
    // the one-press lag it used to have: the value reset, the claim came straight
    // back, and only a second press (which changed nothing, so the gesture was
    // dropped) made the mark go away. Committing here also keeps a real edit made
    // in the same frame as its own transaction, ahead of the reset that undoes it.
    if (history != nullptr)
        history->CommitGesture(entity, meta->id);

    // Captured before the value moves, so the transaction can put both back.
    std::optional<nlohmann::json> before;
    if (history != nullptr)
        before = history->CaptureComponent(entity, meta->id);

    // Re-apply from the blueprint, plus whatever claim survives. This is a
    // one-member re-expansion: the value falls back exactly as a fresh load would
    // produce it, which is the definition of "un-overridden means un-resolved".
    nlohmann::json resolved = desc.components.contains(component) ? desc.components.at(component)
                                                                 : nlohmann::json::object();
    if (survivingClaim.is_object())
    {
        for (const auto &[key, value] : survivingClaim.items())
            resolved[key] = value;
    }

    _scene->RemoveById(entity, meta->id);
    if (desc.components.contains(component) || stillClaimed)
    {
        nlohmann::json wrapper{{component, resolved}};
        Assisi::Runtime::QualifyReferences(wrapper, original.name.empty() ? "" : original.name + "/");
        meta->addToScene(_scene, entity.index, entity.generation, wrapper.at(component));
    }

    _world->instances.RestoreAt(tag->instanceId, updated);

    if (history != nullptr)
    {
        Assisi::Editor::Transaction txn;
        txn.label           = field.empty() ? "Reset " + component : "Reset " + component + "." + field;
        txn.selectionBefore = _selectedEntity;
        txn.selectionAfter  = _selectedEntity;
        txn.cmds.push_back(Assisi::Editor::ComponentDelta{entity, meta->id, before,
                                                          history->CaptureComponent(entity, meta->id)});
        txn.cmds.push_back(
            Assisi::Editor::InstanceDelta{.instanceId = tag->instanceId, .before = original, .after = updated});
        history->Push(std::move(txn));
    }

    // The component came back from JSON: its transients did not.
    RebuildInstanceTransients(std::span{&entity, 1});
}

void EditorApp::SaveLevel(const std::string &name)
{
    (void)SaveLevelToPath("levels/" + name + ".alvl");
}

bool EditorApp::SaveLevelToPath(const std::string &virtualPath)
{
    if (_scene == nullptr || _world == nullptr)
        return false;

    // A save still waiting on its confirmation owns the previous contents of its
    // file, and Cancel's whole promise is that it can put them back. A second write
    // would leave that backup describing bytes no longer anywhere, so the promise
    // would quietly stop being true — refuse instead, and say which dialog is in the
    // way. Round-7 S18: the old code let the second save through and dropped its
    // re-expansion on the floor without a word.
    if (_pendingSaveConfirm)
    {
        Assisi::Core::Log::Error("SaveLevel: refusing to write '{}' — the save of '{}' is still waiting on "
                                 "an answer. Answer that dialog first.",
                                 virtualPath, _pendingSaveConfirm->virtualPath);
        return false;
    }

    const auto resolved = Assisi::Core::AssetSystem::Resolve(virtualPath);
    if (!resolved)
    {
        Assisi::Core::Log::Error("SaveLevel: cannot resolve path for '{}'", virtualPath);
        return false;
    }

    // Read before overwriting: this is the last moment the previous contents exist,
    // and Cancel is a promise to put them back. One extra read of a file that is
    // about to be rewritten in full, on an explicit gesture — cheap next to
    // serializing the scene, and the only alternative is predicting the cost of a
    // write without doing it.
    std::optional<std::string> previousBytes = ReadWholeFile(*resolved);

    // For the same reason and in the same breath: what the live copies of this file
    // are made of *now*. Gathering it after the write reads the new contents as the
    // old ones whenever the definition cache is cold — which a cancelled save
    // guarantees, since cancelling has to drop what the write cached.
    std::vector<PendingReexpand> reexpandTargets = CollectReexpandTargets(virtualPath);

    const std::string   previousLevelPath = _world->levelPath;
    const bool          blueprint         = InBlueprintMode();
    const std::uint64_t previousSavedToken = blueprint ? _blueprintSavedToken : _savedStateToken;

    // Carry the world's systems back into the file. A Scene does not know them —
    // they are a property of the level — so a save that dropped them would silently
    // strip the field from every level the editor touches.
    const Assisi::Runtime::LevelHeader header{.instances = {}, .systems = _world->systemNames};
    if (!Assisi::Runtime::SceneSerializer::SaveToFile(*_scene, *resolved, header, &_world->instances))
        return false;

    // Save As renames what this world *is* — keep its level identity truthful,
    // since travel and (later) the network level handshake read it.
    _world->levelPath = virtualPath;

    // Record the history position that now matches disk — IsSceneDirty compares
    // against this to drive the title's unsaved-changes marker. Which history that
    // is depends on which world is being edited.
    if (blueprint)
    {
        if (_blueprintHistory)
            _blueprintSavedToken = _blueprintHistory->CurrentStateToken();
    }
    else if (_history)
    {
        _savedStateToken = _history->CurrentStateToken();
    }

    // Stage 5d: every live copy of what was just written catches up, wherever it is
    // resident. A level save normally finds nothing — a file cannot instance itself —
    // so the walk above came back empty and this drops the cache and stops.
    ReexpandInstancesOf(virtualPath, std::move(reexpandTargets));

    // ...unless catching them up would cost undo history, in which case the prompt
    // is up and this save is not finished. Hold everything Cancel needs until it is
    // answered; the copies need nothing held, because ApplyPendingReexpand is the
    // only thing that destroys a member and it has not run.
    if (_pendingReexpandUndoLoss > 0)
    {
        _pendingSaveConfirm = PendingSaveConfirm{.virtualPath           = virtualPath,
                                                 .resolved              = *resolved,
                                                 .previousBytes         = std::move(previousBytes),
                                                 .world                 = _world,
                                                 .previousLevelPath     = previousLevelPath,
                                                 .previousSavedToken    = previousSavedToken,
                                                 .savedTokenIsBlueprint = blueprint};
    }

    return true;
}

void EditorApp::CancelPendingSave()
{
    if (!_pendingSaveConfirm)
        return;
    const PendingSaveConfirm &save = *_pendingSaveConfirm;

    // The file first, because it is the only thing that left the process.
    if (save.previousBytes)
    {
        if (WriteWholeFile(save.resolved, *save.previousBytes))
        {
            Assisi::Core::Log::Info("Editor: save of '{}' cancelled; the file is as it was.",
                                    save.virtualPath);
        }
        else
        {
            // Said loudly, because the author declined and the decline did not take:
            // the file holds contents they refused, and nothing else will say so.
            Assisi::Core::Log::Error("Editor: save of '{}' was cancelled but the previous contents could "
                                     "not be written back — the file holds the NEW version.",
                                     save.virtualPath);
        }
    }
    else
    {
        // There was no file before this save (Save As to a fresh name), so putting
        // things back means there is no file after it either.
        std::error_code ec;
        std::filesystem::remove(save.resolved, ec);
        if (ec)
        {
            Assisi::Core::Log::Error("Editor: save of '{}' was cancelled but the new file could not be "
                                     "removed: {}",
                                     save.virtualPath, ec.message());
        }
        else
        {
            Assisi::Core::Log::Info("Editor: save of '{}' cancelled; the file it created is gone.",
                                    save.virtualPath);
        }
    }

    // The cache was dropped against contents that are no longer on disk — drop it
    // again so the next spawn reads what is actually there now.
    Assisi::Runtime::InvalidateBlueprint(save.virtualPath);

    // Only onto the world the save was for. A world that is no longer the edited one
    // must not have its level path rewritten from under whoever holds it now; the
    // modal makes that unreachable today, and this is what keeps it unreachable if
    // the modal ever stops being one.
    if (save.world == _world && _world != nullptr)
    {
        _world->levelPath = save.previousLevelPath;
        if (save.savedTokenIsBlueprint)
            _blueprintSavedToken = save.previousSavedToken;
        else
            _savedStateToken = save.previousSavedToken;
    }

    // The copies were never touched, so there is nothing to put back about them —
    // and nothing stale either, since the file is once again what they expanded from.
    _pendingReexpand.clear();
    _pendingReexpandRemoved.clear();
    _pendingReexpandUndoLoss = 0;
    _pendingReexpandSource.clear();
    _pendingSaveConfirm.reset();
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
    // **Before anything is torn down.** The load below replaces the scene in
    // place, so a level whose systems cannot be installed has to be refused here
    // or not at all: discovering it afterwards leaves the world holding the new
    // content with none of its behaviour, and the level it replaced is already
    // gone. Checked before ShutdownNetSession for the same reason — a refused
    // load must not have ended the session on the way to refusing.
    if (!Assisi::App::LevelSystemsAreDeclared(virtualPath))
    {
        Assisi::Core::Log::Error("Editor: refusing to open '{}'.", virtualPath);
        return false;
    }

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
    //
    // A failed install fails the load. An unknown system name is a hard error by
    // design, and this is the path a standalone build takes for its startup
    // level — discarding it here is what let a misspelled name open a level that
    // then ran none of its behaviour.
    if (!_worlds.ApplySystems(*_world, header.systems, virtualPath))
    {
        Assisi::Core::Log::Error("Editor: '{}' names a system this build does not declare.", virtualPath);
        return false;
    }

    // A load also ends any in-progress play session: the snapshot describes the
    // old scene, so it must not survive into the new one.
    SetPlayState(PlayState::Editing);
    _playSnapshot.clear();
    ClearSelection();

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
    // And the same for an instance drag caught mid-gesture by the load: its
    // snapshot names entities in the scene that just went away, so committing it
    // would write a transaction against handles that now mean something else.
    _instanceGesture.Abandon();
    _pausedHistory.reset(); // a load ends any play session, scratch history included
    _savedStateToken = 0;   // freshly loaded scene == on disk (empty history, token 0)

    // A load expands every instance from the files as they are now, so nothing this
    // world holds can still be behind one (stage 5e).
    //
    // An unanswered save is dropped rather than cancelled: its file stays as it was
    // written, because a load is not a rejection of it and the world it was asked
    // about is gone either way. Said out loud, since "the dialog vanished" would
    // otherwise be indistinguishable from having answered it.
    if (_pendingSaveConfirm)
    {
        Assisi::Core::Log::Warn("Editor: the load of '{}' left the save of '{}' unanswered — the file "
                                "keeps what was written to it.",
                                virtualPath, _pendingSaveConfirm->virtualPath);
        _pendingSaveConfirm.reset();
    }
    _staleInstanceSources.clear();
    _pendingReexpand.clear();
    _pendingReexpandRemoved.clear();
    _pendingReexpandUndoLoss = 0;
    _pendingReexpandSource.clear();

    return true;
}

} // namespace Assisi::Editor
