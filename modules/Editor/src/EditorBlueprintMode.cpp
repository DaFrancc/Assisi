/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorBlueprintMode.cpp
/// @brief Editing a blueprint in its own world, with its own light.
///
/// A blueprint is an ordinary level file, so
/// editing one is opening it as a level — no second format, no second loader. What
/// this file adds is the *mode*: the level you came from stays resident behind it,
/// the blueprint world takes the edited role and its own undo stack, and the panels
/// that have nothing to do with authoring one piece of content go away.
///
/// The level staying resident is load-bearing. Saving a blueprint brings that
/// level's live copies of it up to date in place (see "Re-expansion on save"
/// below), and it cannot do that to a world it just unloaded.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Runtime/EditorOnly.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace Assisi::Editor
{
namespace
{

/// Where the editor's sun starts: a touch warm, over the author's left shoulder,
/// well above the horizon. Angled rather than straight down the camera so a shape
/// keeps a lit side and a dark one — a head-on light makes every model look flat,
/// which is the failure this rig exists to avoid.
constexpr float kSunIntensity = 1.5f;
constexpr float kSunAzimuth   = -35.f; ///< Degrees, 0 = along +Z.
constexpr float kSunElevation = 45.f;  ///< Degrees above the horizon.

/// The direction a sun at (@p azimuth, @p elevation) shines *along* — which is what
/// DirectionalLight::direction holds, pointing away from the light.
glm::vec3 SunDirection(float azimuthDegrees, float elevationDegrees)
{
    const float az = glm::radians(azimuthDegrees);
    const float el = glm::radians(elevationDegrees);
    const glm::vec3 toLight{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    return -glm::normalize(toLight);
}

/// The (azimuth, elevation) a direction came from — the inverse of SunDirection, so
/// the sliders show where the sun actually is after somebody dragged its Transform
/// or typed into the inspector.
std::pair<float, float> SunAngles(const glm::vec3 &direction)
{
    const glm::vec3 toLight = glm::normalize(-direction);
    return {glm::degrees(std::atan2(toLight.x, toLight.z)),
            glm::degrees(std::asin(std::clamp(toLight.y, -1.f, 1.f)))};
}

} // namespace

bool EditorApp::InBlueprintMode() const
{
    return _blueprintWorld != nullptr && _world == _blueprintWorld;
}

Assisi::ECS::Entity EditorApp::BlueprintSunEntity() const
{
    if (_blueprintWorld == nullptr)
        return Assisi::ECS::NullEntity;

    // A query rather than a stored handle, for the same reason instance membership
    // is: the sun is an ordinary entity and the author may delete it. A handle
    // would go stale silently and the panel would edit a slot something else owns.
    for (auto [entity, light, tag] :
         _blueprintWorld->scene.Query<Assisi::Runtime::DirectionalLight, Assisi::Runtime::EditorOnly>())
    {
        (void)light;
        (void)tag;
        return entity;
    }
    return Assisi::ECS::NullEntity;
}

void EditorApp::AddBlueprintEditorRig(Assisi::App::World &world)
{
    const Assisi::ECS::Entity sun = world.scene.Create();

    // EditorOnly is what keeps this sun out of the file. Without it the first save
    // of crate.abp writes a sun into the crate, and every instance of that crate
    // placed in a level brings its own along — see Runtime::EditorOnly.
    (void)world.scene.Add(sun, Assisi::Runtime::EditorOnly{});
    (void)Assisi::Runtime::GiveEntityName(world.scene, sun, "Editor Sun");
    (void)world.scene.Add(sun, Assisi::Runtime::DirectionalLight{
            .direction = SunDirection(kSunAzimuth, kSunElevation),
            .color     = {1.f, 0.98f, 0.94f},
            .intensity = kSunIntensity});
}

void EditorApp::OpenBlueprintForEditing(const std::string &source)
{
    // Play owns the scene, and blueprint mode is an editing gesture. The panel
    // greys its button out too; this is the guard that means it.
    if (_playState != PlayState::Editing)
        return;

    // Already open: show it rather than standing up a second one. Two worlds of one
    // blueprint would both claim to be the file and only one could save.
    if (_blueprintWorld != nullptr)
    {
        if (_blueprintWorld->levelPath != source)
        {
            Assisi::Core::Log::Warn("Blueprint editor: '{}' is already open; close it before opening '{}'.",
                                    _blueprintWorld->levelPath, source);
        }
        SetActiveWorld(*_blueprintWorld);
        return;
    }

    if (!Assisi::Core::AssetSystem::Exists(source))
    {
        Assisi::Core::Log::Error("Blueprint editor: '{}' not found under the asset root.", source);
        return;
    }

    Assisi::App::World &world = _worlds.Create("Blueprint");

    // Keep the asset cache: the level behind us holds resolved pointers into it, and
    // a blueprint shares most of its meshes and materials with the level that places
    // it — re-uploading them to look at one would be the expensive way round.
    Assisi::Runtime::LevelHeader header;
    const Assisi::Runtime::LevelResult loaded =
        Assisi::App::LoadLevel(world, source, {_assetCache, _assetDatabase, _sceneRenderer},
                               {.reset = Assisi::App::AssetCacheReset::Keep, .header = &header});
    if (!loaded)
    {
        // Same as opening a level as a new world: the scene this may have emptied is
        // the throwaway one created two lines up, and it is destroyed here.
        Assisi::Core::Log::Error("Blueprint editor: '{}' failed to load — {}.", source,
                                 Assisi::Runtime::Describe(loaded.error()));
        _worlds.Destroy(world.name);
        return;
    }

    world.levelPath = source;
    // A blueprint world never simulates, so a missing system changes nothing you
    // could see here — but it means the file names something this build cannot
    // provide, which the author should hear about before saving over it.
    if (!_worlds.ApplySystems(world, header.systems, source))
        Assisi::Core::Log::Error("Blueprint mode: '{}' names a system this build does not declare.", source);
    world.state = Assisi::App::WorldState::Active;
    // Never simulate: a running world settles its bodies into a pose the file would
    // then remember.
    world.simulate = false;

    AddBlueprintEditorRig(world);

    _blueprintReturnWorld = _world != nullptr ? _world->name : std::string{};
    _blueprintWorld       = &world;

    // The role moves, which is what makes the panels editable here. The level keeps
    // its history in _history, untouched, and gets the role back on close.
    _worlds.SetEdited(world);
    SetActiveWorld(world);

    ClearSelection();
    _blueprintHistory.emplace(world.scene, MakeEditRebindHook(), &world.instances);
    _blueprintSavedToken = 0; // freshly loaded == what is on disk

    Assisi::Core::Log::Info("Blueprint editor: editing '{}' (world '{}').", source, world.name);
}

void EditorApp::CloseBlueprintEditor()
{
    if (_blueprintWorld == nullptr)
        return;

    const std::string worldName = _blueprintWorld->name;
    const std::string path      = _blueprintWorld->levelPath;

    // Drop the history *first*: it binds the scene by reference and holds entity
    // handles into it, and the world is about to be destroyed under both.
    _blueprintHistory.reset();
    _blueprintSavedToken = 0;
    ClearSelection();

    // Same aliasing hazard a level load guards against, and the same fix: an armed
    // eyedropper or an open browser dialog holds a handle from the world being
    // destroyed, which would resolve into a live but entirely different entity.
    _eyedropperArmed    = false;
    _eyedropperEntity   = Assisi::ECS::NullEntity;
    _eyedropperMeta     = nullptr;
    _assetBrowserOpen   = false;
    _assetBrowserEntity = Assisi::ECS::NullEntity;
    _assetBrowserMeta   = nullptr;

    _blueprintWorld = nullptr;

    // Both roles have to leave the world before it can be destroyed — Destroy
    // refuses otherwise, precisely so a dangling role is an error and not a crash.
    Assisi::App::World *const back = _worlds.Find(_blueprintReturnWorld);
    if (back != nullptr)
    {
        _worlds.SetEdited(*back);
        SetActiveWorld(*back);
    }
    else
    {
        Assisi::Core::Log::Error("Blueprint editor: the world '{}' to return to is gone; leaving '{}' "
                                 "resident rather than destroying the only world.",
                                 _blueprintReturnWorld, worldName);
        return;
    }
    _blueprintReturnWorld.clear();

    (void)_worlds.Destroy(worldName);
    Assisi::Core::Log::Info("Blueprint editor: closed '{}'.", path);
}

void EditorApp::DrawBlueprintEditorWindow()
{
    if (!InBlueprintMode())
        return;

    ImGui::Begin("Blueprint");

    ImGui::TextUnformatted(_blueprintWorld->levelPath.c_str());
    if (IsSceneDirty())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "*");
    }
    ImGui::TextWrapped("Saving writes this file and brings every live copy of it up to date.");

    ImGui::Separator();

    const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Save", ImVec2(halfW, 0.f)))
        SaveLevelToPath(_blueprintWorld->levelPath);
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(-1.f, 0.f)))
        _pendingBlueprintClose = true;
    if (ImGui::IsItemHovered() && IsSceneDirty())
        ImGui::SetTooltip("There are unsaved changes. Closing discards them.");

    // --- The rig ------------------------------------------------------------
    ImGui::Separator();
    ImGui::SeparatorText("Lighting");
    ImGui::TextDisabled("Editor-only. None of this is written to the file.");

    const Assisi::ECS::Entity sun = BlueprintSunEntity();
    if (sun == Assisi::ECS::NullEntity)
    {
        ImGui::TextDisabled("No editor sun in this world.");
        if (ImGui::Button("Add one", ImVec2(-1.f, 0.f)))
            AddBlueprintEditorRig(*_blueprintWorld);
    }
    else if (auto *light = _blueprintWorld->scene.GetMut<Assisi::Runtime::DirectionalLight>(sun))
    {
        auto [azimuth, elevation] = SunAngles(light->direction);
        bool aimed                = false;
        aimed |= ImGui::SliderFloat("Azimuth", &azimuth, -180.f, 180.f, "%.0f°");
        aimed |= ImGui::SliderFloat("Elevation", &elevation, -89.f, 89.f, "%.0f°");
        if (aimed)
            light->direction = SunDirection(azimuth, elevation);

        ImGui::ColorEdit3("Sun colour", &light->color.x, ImGuiColorEditFlags_Float);
        ImGui::DragFloat("Sun intensity", &light->intensity, 0.02f, 0.f, 20.f, "%.2f");
    }

    ImGui::Spacing();
    ImGui::ColorEdit3("Ambient colour", &_blueprintAmbientColor.x, ImGuiColorEditFlags_Float);
    // Defaults low, and worth leaving low: ambient is unshadowed by construction, so
    // near 1 it washes out every cue the sun is here to give.
    ImGui::SliderFloat("Ambient", &_blueprintAmbient, 0.f, 1.f, "%.2f");

    if (ImGui::SmallButton("Reset lighting"))
    {
        _blueprintAmbientColor = glm::vec3(1.f);
        _blueprintAmbient      = 0.25f;
        if (auto *light = sun != Assisi::ECS::NullEntity
                              ? _blueprintWorld->scene.GetMut<Assisi::Runtime::DirectionalLight>(sun)
                              : nullptr)
        {
            light->direction = SunDirection(kSunAzimuth, kSunElevation);
            light->color     = {1.f, 0.98f, 0.94f};
            light->intensity = kSunIntensity;
        }
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// The instance root's billboard
// ---------------------------------------------------------------------------

void EditorApp::SubmitInstanceIcons()
{
    if (_scene == nullptr || _world == nullptr)
        return;

    // Placements come from the table, the only place they exist: an instance root is
    // a row, not an entity. Rebuilt per frame rather than cached — instances are
    // placed and deleted by the same gestures that would have to invalidate a cache,
    // and there are tens of them, not thousands.
    std::vector<glm::vec3> positions;
    const auto rows = _world->instances.All();
    positions.reserve(rows.size());
    for (const auto &[id, row] : rows)
    {
        (void)id;
        positions.push_back(row->transform.position);
    }
    _sceneRenderer.SubmitEditorIcons(positions);

    if (_selectedInstance.IsValid())
    {
        if (const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(_selectedInstance))
            _sceneRenderer.SubmitIconOutline(row->transform.position);
    }
}

// ---------------------------------------------------------------------------
// Re-expansion on save
// ---------------------------------------------------------------------------

void EditorApp::MarkInstancesStale(const std::string &source)
{
    if (std::find(_staleInstanceSources.begin(), _staleInstanceSources.end(), source) ==
        _staleInstanceSources.end())
    {
        _staleInstanceSources.push_back(source);
    }
    Assisi::Core::Log::Warn("Blueprint '{}': live copies left out of date with the file. Hosting is "
                            "refused until they catch up or the level is reloaded.",
                            source);
}

std::vector<EditorApp::PendingReexpand> EditorApp::CollectReexpandTargets(const std::string &source)
{
    // Everything the edit reaches, gathered **before the file is written** — the
    // answer lives in the definition as it stands now, and a member's name is the
    // only thing connecting a live tag to its replacement.
    //
    // Before the *write*, not merely before the invalidation. `GetBlueprintDefinition`
    // re-parses from disk whenever the cache is cold, and a cancelled save leaves it
    // cold (Cancel drops the entry it wrote), so running this afterwards can read the
    // **new** file as the "previous" definition, find nothing removed, and save
    // without asking. Ask while the old contents are still the contents.
    std::vector<PendingReexpand> collected;
    std::vector<std::string>     skipped;

    _worlds.ForEach(
        [&](Assisi::App::World &world)
        {
            // A simulating world is stepping bodies right now; replacing them
            // under the step is a visible pop at best, with no undo to fall back
            // on. Named in the log rather than silently passed over.
            if (world.simulate)
            {
                for (const auto &[id, row] : world.instances.All())
                {
                    (void)id;
                    const Assisi::Runtime::BlueprintResult definition =
                        Assisi::Runtime::GetBlueprintDefinition(row->source);
                    if (!definition)
                        continue;
                    if (row->source == source ||
                        std::find((*definition)->closure.begin(), (*definition)->closure.end(), source) !=
                        (*definition)->closure.end())
                    {
                        skipped.push_back(world.name);
                        break;
                    }
                }
                return;
            }

            for (const auto &[id, row] : world.instances.All())
            {
                const Assisi::Runtime::BlueprintResult definition =
                    Assisi::Runtime::GetBlueprintDefinition(row->source);
                if (!definition)
                    continue;

                // By closure, not by path: a lot's flattened member list contains
                // the car's members, so editing the car changes the lot as well.
                const bool touched =
                    row->source == source || std::find((*definition)->closure.begin(),
                                                       (*definition)->closure.end(),
                                                       source) != (*definition)->closure.end();
                if (!touched)
                    continue;

                PendingReexpand pending;
                pending.world      = &world;
                pending.instanceId = id;
                pending.previousMemberNames.reserve((*definition)->members.size());
                for (const auto &member : (*definition)->members)
                    pending.previousMemberNames.push_back(member.name);

                // The entities behind those names, resolved here and not later: a
                // member tag holds an index into *this* definition, so this is the
                // last moment an index can be turned into the right entity — see
                // PendingReexpand::previousMemberEntities. Walked by tag rather than
                // by Runtime::FindMember per name — one query for the whole instance
                // instead of one per member, and no name lookup at all.
                pending.previousMemberEntities.assign((*definition)->members.size(),
                                                      Assisi::ECS::NullEntity);
                for (auto [entity, tag] : world.scene.Query<Assisi::ECS::BlueprintMember>())
                {
                    if (tag.instanceId == id &&
                        tag.memberIndex < pending.previousMemberEntities.size())
                    {
                        pending.previousMemberEntities[tag.memberIndex] = entity;
                    }
                }
                collected.push_back(std::move(pending));
            }
        });

    for (const std::string &name : skipped)
        Assisi::Core::Log::Info("Blueprint '{}': world '{}' skipped (simulating).", source, name);

    return collected;
}

void EditorApp::ReexpandInstancesOf(const std::string &source, std::vector<PendingReexpand> collected)
{
    // The cache goes regardless of whether anything was live: the next spawn must
    // read what was just written, not what was cached before it.
    Assisi::Runtime::InvalidateBlueprint(source);

    if (collected.empty())
        return;

    // One collection at a time: the prompt has one set of buttons, so a second save
    // cannot ask. **This guard must stay below the invalidation above.** Returning
    // ahead of it leaves the new file on disk while `GetBlueprintDefinition` still
    // hands out the contents from before it, so every later spawn builds the old
    // thing and nothing says so.
    //
    // Below it, the only thing left to skip is the live copies' catch-up: the "Leave
    // them" answer, given on the author's behalf because there was no way to ask. The
    // write stands, the cache is honest, the copies are recorded as stale, and hosting
    // stays refused until they catch up or the level is reloaded.
    if (!_pendingReexpand.empty())
    {
        MarkInstancesStale(source);
        return;
    }

    // What each instance loses, from the name diff. Only answerable here: the new
    // definitions exist and nothing has been touched yet.
    //
    // **Grouped by world, never flattened.** A handle is (slot, generation) with no
    // scene identity in it and every Scene numbers from {0,0}, so a doomed handle
    // from one resident world compares equal to a live, unrelated entity in
    // another — and this loop deliberately spans every resident world.
    std::vector<std::pair<Assisi::App::World *, std::vector<Assisi::ECS::Entity>>> doomedByWorld;
    const auto doomedFor = [&doomedByWorld](Assisi::App::World *world) -> std::vector<Assisi::ECS::Entity> &
                           {
                               for (auto &[owner, list] : doomedByWorld)
                               {
                                   if (owner == world)
                                       return list;
                               }
                               return doomedByWorld.emplace_back(world, std::vector<Assisi::ECS::Entity>{}).second;
                           };

    for (const PendingReexpand &pending : collected)
    {
        const Assisi::Runtime::BlueprintInstance *row = pending.world->instances.Find(pending.instanceId);
        if (row == nullptr)
            continue;
        const Assisi::Runtime::BlueprintResult definition =
            Assisi::Runtime::GetBlueprintDefinition(row->source);
        if (!definition)
            continue; // the file broke; ReexpandInstance will refuse it and say so

        for (std::size_t i = 0; i < pending.previousMemberNames.size(); ++i)
        {
            const std::string &name = pending.previousMemberNames[i];
            if ((*definition)->IndexOf(name).has_value())
                continue;

            // From the capture, never from a fresh lookup: `definition` is the file
            // as it now is and `name` is by construction something it no longer
            // declares, so any name-based resolution can only fail.
            const Assisi::ECS::Entity member = pending.previousMemberEntities[i];
            if (member == Assisi::ECS::NullEntity)
                continue;
            doomedFor(pending.world).push_back(member);

            if (std::find(_pendingReexpandRemoved.begin(), _pendingReexpandRemoved.end(), name) ==
                _pendingReexpandRemoved.end())
            {
                _pendingReexpandRemoved.push_back(name);
            }
        }
    }

    _pendingReexpand       = std::move(collected);
    _pendingReexpandSource = source;

    // How much history the truncation costs. **Every stack, not `ActiveHistory()`:**
    // a blueprint-mode save destroys members in the *level* worlds while the active
    // history is the blueprint world's, and `CountForgettable` returns 0 for a scene
    // it is not bound to — so asking only the active one makes the count always 0,
    // the prompt below unreachable, and `ForgetEntities` decline on the same test,
    // leaving the level's stack naming entities that have just been destroyed.
    //
    // Each history self-filters by scene, so the pairing below is a full cross
    // product on purpose: only the matching terms contribute.
    _pendingReexpandUndoLoss = 0;
    for (Assisi::Editor::EditHistory *history : AllHistories())
    {
        for (const auto &[world, doomed] : doomedByWorld)
            _pendingReexpandUndoLoss += history->CountForgettable(world->scene, doomed);
    }

    // Logged even when nothing is at stake — the common case, since most edits only
    // change values. "No dialog appeared" and "the dialog is broken" look identical
    // from the outside otherwise.
    Assisi::Core::Log::Info("Blueprint '{}': {} member(s) removed from {} live cop(y/ies), {} undo step(s) "
                            "at stake.",
                            source, _pendingReexpandRemoved.size(), _pendingReexpand.size(),
                            _pendingReexpandUndoLoss);

    if (_pendingReexpandUndoLoss == 0)
        ApplyPendingReexpand();

    // Otherwise the save is left waiting: SaveLevelToPath sees the nonzero loss,
    // raises `_pendingSaveConfirm` and holds everything Cancel needs, and
    // DrawSaveConfirmModal keeps the prompt up until it is answered.
}

void EditorApp::ApplyPendingReexpand()
{
    if (_pendingReexpand.empty())
        return;

    // Grouped by world for the same reason the doomed list is — see ReexpandInstancesOf.
    std::vector<std::pair<Assisi::App::World *, std::vector<Assisi::ECS::Entity>>> destroyedByWorld;
    const auto destroyedFor =
        [&destroyedByWorld](Assisi::App::World *world) -> std::vector<Assisi::ECS::Entity> &
        {
            for (auto &[owner, list] : destroyedByWorld)
            {
                if (owner == world)
                    return list;
            }
            return destroyedByWorld.emplace_back(world, std::vector<Assisi::ECS::Entity>{}).second;
        };

    std::size_t instancesUpdated = 0;

    for (const PendingReexpand &pending : _pendingReexpand)
    {
        Assisi::App::World &world = *pending.world;

        // ReexpandInstance's precondition: engine-side state it cannot see has to
        // come off first, or the strip leaves a Jolt body wired to a component about
        // to be rewritten from a file.
        for (const Assisi::ECS::Entity member :
             Assisi::Runtime::MembersOf(world.scene, pending.instanceId))
        {
            if (const auto *body = world.scene.Get<Assisi::Physics::RigidBody>(member))
            {
                world.physics.RemoveBody(*body);
                world.scene.Remove<Assisi::Physics::RigidBody>(member);
            }
        }

        const auto result = Assisi::Runtime::SceneSerializer::ReexpandInstance(
            world.scene, world.instances, pending.instanceId, pending.previousMemberNames);
        if (!result)
            continue; // already logged, and nothing was changed

        ++instancesUpdated;
        if (!result->destroyed.empty())
        {
            std::vector<Assisi::ECS::Entity> &list = destroyedFor(pending.world);
            list.insert(list.end(), result->destroyed.begin(), result->destroyed.end());
        }
        RebuildInstanceTransients(world, result->members);
    }

    // Now, not at end of frame: a deferred destroy would let the next entity created
    // take a slot the history still believes is occupied.
    for (const PendingReexpand &pending : _pendingReexpand)
        pending.world->scene.FlushDestroyed();

    if (!destroyedByWorld.empty())
    {
        // Every stack, for the reason ReexpandInstancesOf spells out: the members
        // that went are not necessarily in the world whose history is active, and a
        // transaction still naming a destroyed entity holds a handle the next dense
        // rebuild can hand to something else entirely.
        {
            std::size_t dropped = 0;
            for (Assisi::Editor::EditHistory *history : AllHistories())
            {
                for (const auto &[world, destroyed] : destroyedByWorld)
                    dropped += history->ForgetEntities(world->scene, destroyed);
            }

            if (dropped > 0)
            {
                Assisi::Core::Log::Info("Blueprint '{}': {} undo step(s) dropped — they named members the "
                                        "edit removed.",
                                        _pendingReexpandSource, dropped);
            }
        }
        PruneSelection();
    }

    Assisi::Core::Log::Info("Blueprint '{}': {} instance(s) brought up to date.", _pendingReexpandSource,
                            instancesUpdated);

    // The copies have caught up, so this file is no longer stale.
    std::erase(_staleInstanceSources, _pendingReexpandSource);

    _pendingReexpand.clear();
    _pendingReexpandRemoved.clear();
    _pendingReexpandUndoLoss = 0;
    _pendingReexpandSource.clear();
}

void EditorApp::DrawSaveConfirmModal()
{
    if (!_pendingSaveConfirm)
        return;

    // Titled for what it costs, not for what happened. A title like "Blueprint saved"
    // reads as a receipt, and a receipt gets dismissed unread — here that means
    // throwing away undo history by reflex.
    constexpr const char *kTitle = "Saving will discard undo history";
    if (!ImGui::IsPopupOpen(kTitle))
        ImGui::OpenPopup(kTitle);

    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // The cost first, in the warning colour: it is the only part of this that cannot
    // be taken back, and the rest of the dialog is context for it.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.2f, 1.f));
    ImGui::TextWrapped("%zu undo step%s will be discarded and cannot be recovered.",
                       _pendingReexpandUndoLoss, _pendingReexpandUndoLoss == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    std::string removed;
    for (const std::string &name : _pendingReexpandRemoved)
        removed += (removed.empty() ? "" : ", ") + name;
    ImGui::TextWrapped("Saving '%s' removes %s from its %zu live cop%s.", _pendingReexpandSource.c_str(),
                       removed.c_str(), _pendingReexpand.size(),
                       _pendingReexpand.size() == 1 ? "y" : "ies");
    ImGui::Spacing();
    ImGui::TextWrapped("Those undo steps name the entities being removed, so they cannot be replayed once "
                       "the removal happens. Redo goes with them.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Cancel puts the file back exactly as it was and changes nothing else.");
    ImGui::Spacing();

    if (ImGui::Button("Save and discard the history", ImVec2(220.f, 0.f)))
    {
        ApplyPendingReexpand();
        _pendingSaveConfirm.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    // The middle answer, and what the stale-instance record exists for: the write
    // stands and the copies stay behind it, which is legal, recorded, and refuses
    // hosting until resolved. Distinct from Cancel on purpose — one is "save it, I
    // will deal with the copies later", the other "I did not want any of this".
    if (ImGui::Button("Save, leave copies", ImVec2(160.f, 0.f)))
    {
        MarkInstancesStale(_pendingReexpandSource);

        _pendingReexpand.clear();
        _pendingReexpandRemoved.clear();
        _pendingReexpandUndoLoss = 0;
        _pendingReexpandSource.clear();
        _pendingSaveConfirm.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.f, 0.f)))
    {
        CancelPendingSave();
        ImGui::CloseCurrentPopup();
    }
    // Default focus stays on Cancel, so Enter lands on the answer that loses nothing.
    // The first button is the one this dialog exists to slow down; it must not also
    // be the one a keypress aimed at the previous screen happens to hit.
    ImGui::SetItemDefaultFocus();

    ImGui::EndPopup();
}

} // namespace Assisi::Editor
