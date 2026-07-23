/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Vulkan/VulkanContext.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>
#include <imgui_internal.h> // ImGui input watchdog: ActiveId/HoveredWindow introspection

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void EditorApp::SetupCamera()
{
    const glm::vec3 camPos{5.f, 5.f, 10.f};
    const glm::vec3 forward = glm::normalize(-camPos);

    _pitch = glm::degrees(glm::asin(forward.y));
    _yaw   = glm::degrees(glm::atan(forward.z, forward.x));

    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
    const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

    _cameraTransform.position = camPos;
    _cameraTransform.rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    // _camera keeps its header default (60 deg FOV, 0.1..200 clip, active).
    RefreshCameraMatrix();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EditorApp::OnStart()
{
    // Load action bindings from game.json
    {
        const auto pathResult = Assisi::Core::AssetSystem::Resolve("game.json");
        if (pathResult)
        {
            if (std::ifstream file(pathResult.value()); file.is_open())
            {
                try
                {
                    const auto json = nlohmann::json::parse(file);
                    if (json.contains("input") && json.at("input").contains("actions"))
                        _actions.LoadFromJson(json.at("input").at("actions"));
                }
                catch (const nlohmann::json::exception &e)
                {
                    Assisi::Core::Log::Warn("Failed to parse input bindings from game.json: {}", e.what());
                }
            }
        }
    }

    auto mainScene = _scenes.Create("Main");
    if (!mainScene)
    {
        // Abort for real: without a scene the per-frame hooks have nothing to
        // run on. RequestClose() here means Run()'s first ShouldClose() check
        // fails and the loop body never executes; the _scene guards in
        // OnFixedUpdate/OnUpdate are defense-in-depth for that same null.
        Assisi::Core::Log::Error("Failed to create the main scene; aborting startup");
        RequestClose();
        return;
    }
    _scene = *mainScene;

    // Editor-only undo/redo. Binds the Main scene (stable for the session — level
    // loads Clear it in place, never swap the object). The rebind hook rebuilds the
    // transient state serialization drops after an apply. See EditHistory.hpp.
    _history.emplace(*_scene, MakeEditRebindHook());

    // Editor reconcile pass: give every asset a `.aast` GUID sidecar and build
    // the GUID→path database. Runs here (not in Application) because it is
    // editor-only — a shipped game consumes a baked index, never scans/writes.
    ReimportAssets();

    SetupCamera();
    SetupScene();
    ScanLevels();

    // Startup level from EditorConfig: open it now so `<editor-exe>
    // levels/Materials.alvl` boots straight into that scene. Resolved through the
    // asset system like every other asset; a missing/typo'd path just warns.
    if (!_editorConfig.startupLevel.empty())
    {
        if (!Assisi::Core::AssetSystem::Exists(_editorConfig.startupLevel))
        {
            Assisi::Core::Log::Warn("Startup level '{}' not found under the asset root; ignoring.", _editorConfig.startupLevel);
        }
        else if (!LoadLevelFromPath(_editorConfig.startupLevel))
        {
            Assisi::Core::Log::Warn("Startup level '{}' could not be loaded.", _editorConfig.startupLevel);
        }
    }

    // --- Systems ---
    _systems.Register(Assisi::App::SystemPhase::Update, "EntityPicking",
                      [this](Assisi::App::SystemContext &) { HandleEntityPicking(); });

    _systems.Register(Assisi::App::SystemPhase::Update, "CameraController",
                      [this](Assisi::App::SystemContext &ctx) { UpdateCamera(ctx.dt); })
        .After("EntityPicking");

    _systems.Register(Assisi::App::SystemPhase::PostUpdate, "ProcessEntitySelection",
                      [this](Assisi::App::SystemContext &ctx)
                      {
                          for (const auto &e : ctx.events.Read<EntitySelectionChangedEvent>())
                          {
                              _selectedEntity = e.entity;
                          }
                      });

    // The game's systems go into their own registry, ticked only while Playing
    // (see the EditorConfig seam contract). Registered once here, gated at the
    // run sites in OnUpdate/OnFixedUpdate.
    if (_editorConfig.registerGameSystems)
    {
        _editorConfig.registerGameSystems(_gameSystems);
        if (_gameSystems.HasRenderSystems())
        {
            Assisi::Core::Log::Warn(
                "EditorApp: the game registered render system(s), which do not run in-editor — "
                "the editor owns rendering. They will run in the standalone game build only.");
        }
    }
}

void EditorApp::ReimportAssets()
{
    const std::expected<std::size_t, Assisi::Core::AssetError> result = _assetDatabase.Rebuild();
    if (!result)
    {
        Assisi::Core::Log::Warn("Asset reimport failed: asset root unavailable.");
        return;
    }
    Assisi::Core::Log::Info("Asset reimport: {} assets indexed (GUID sidecars reconciled).", *result);

    // Wire the (rebuilt) database into serialization's path hint and the asset
    // cache's id↔path translation. The resolvers capture the database by
    // reference, so re-running reimport reuses the same (now freshly rebuilt)
    // database — no need to reinstall, but harmless if we do.
    Assisi::App::InstallAssetResolvers(_assetCache, _assetDatabase);

    // S3/S4: bring glTF materials up to date — explode new meshes, reconcile
    // already-exploded ones against their current source. Any write means new/
    // updated files + manifests, so rebuild afterwards to bring them into the DB
    // (only when something changed — steady state does nothing). The hint
    // resolver is already installed above, so written `.amat` channels carry
    // regenerated path hints (D2).
    if (ReconcileMeshMaterials())
    {
        if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            Assisi::Core::Log::Info("Asset reimport: {} assets indexed after material reconcile.", *rescan);
    }

    // Liveness (D5): a stale mesh currently drawn in the open scene can't wait for
    // the author to click its badge — queue it for an immediate resolution prompt.
    // Non-live stale meshes just keep their badge (resolved on click / next load).
    // At startup the scene has no MeshRenderers yet, so this finds nothing then;
    // it fires on the manual Reimport button with a level loaded.
    _staleResolveQueue.clear();
    if (_scene != nullptr && !_staleMeshes.empty())
    {
        for (auto [entity, mrc] : _scene->Query<Assisi::Runtime::MeshRenderer>())
        {
            const std::optional<std::string> meshPath = _assetDatabase.PathFor(mrc.mesh);
            if (meshPath && _staleMeshes.contains(*meshPath) &&
                std::find(_staleResolveQueue.begin(), _staleResolveQueue.end(), *meshPath) ==
                    _staleResolveQueue.end())
            {
                _staleResolveQueue.push_back(*meshPath);
            }
        }
        if (_staleResolveTarget.empty()) // don't interrupt a prompt already open
        {
            AdvanceStaleQueue();
        }
    }

    // The browser lists by extension and never shows `.aast`, but a reimport may
    // have created sidecars, so force a re-read on next open.
    _assetBrowserDirty = true;
}

bool EditorApp::ReconcileMeshMaterials()
{
    // Case-insensitive glTF-extension test on the virtual path.
    const auto isGltf = [](std::string_view path)
    {
        const auto endsWith = [path](std::string_view suffix)
        {
            if (path.size() < suffix.size())
                return false;
            const std::string_view tail = path.substr(path.size() - suffix.size());
            for (std::size_t i = 0; i < suffix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(tail[i])) != suffix[i])
                    return false;
            return true;
        };
        return endsWith(".gltf") || endsWith(".glb");
    };

    // Texture channels in the written `.amat`s resolve through the database, the
    // same map the asset cache uses — so an exploded material references the same
    // texture GUIDs the mesh would have imported.
    const auto resolveTextureId = [this](std::string_view vpath) -> Assisi::Core::AssetId
    { return _assetDatabase.IdFor(vpath).value_or(Assisi::Core::AssetId{}); };
    // Existing `.amat` files, by their GUID, so the reconciler can load and
    // compare them against the fresh material table.
    const auto resolveMaterialPath = [this](const Assisi::Core::AssetId &id) -> std::string
    { return _assetDatabase.PathFor(id).value_or(std::string{}); };

    _staleMeshes.clear();
    bool changed = false;
    for (const auto &[id, path] : _assetDatabase.Assets())
    {
        if (!isGltf(path))
            continue;

        if (!_assetDatabase.HasManifest(id))
        {
            // First sight: materialize the glTF's materials into `.amat` children.
            const std::expected<std::size_t, Assisi::Geometry::MeshImportError> result =
                Assisi::Geometry::ExplodeGltfMaterials(path, resolveTextureId);
            if (result)
            {
                Assisi::Core::Log::Info("Asset reimport: exploded {} material(s) from '{}'.", *result, path);
                changed = true;
            }
            else
            {
                Assisi::Core::Log::Warn("Asset reimport: could not explode '{}' ({}).", path,
                                        Assisi::Geometry::ToString(result.error()));
            }
            continue;
        }

        // Already exploded: reconcile against the current source (S4/D5).
        const Assisi::Geometry::ReconcileResult result =
            Assisi::Geometry::ReconcileGltfMaterials(path, resolveTextureId, resolveMaterialPath);
        switch (result.outcome)
        {
        case Assisi::Geometry::ReconcileOutcome::UpToDate:
            break;
        case Assisi::Geometry::ReconcileOutcome::Stamped:
            Assisi::Core::Log::Info("Asset reimport: recorded source hash for '{}'.", path);
            break;
        case Assisi::Geometry::ReconcileOutcome::GeometryOnly:
            Assisi::Core::Log::Info("Asset reimport: '{}' source changed but materials are unchanged; refreshed.",
                                    path);
            break;
        case Assisi::Geometry::ReconcileOutcome::AdditiveSlots:
            Assisi::Core::Log::Info("Asset reimport: '{}' gained {} material slot(s); wrote defaults.", path,
                                    result.addedSlots);
            break;
        case Assisi::Geometry::ReconcileOutcome::ConflictStale:
            _staleMeshes.insert(path);
            Assisi::Core::Log::Warn("Asset reimport: '{}' changed in a way that needs manual resolution; left "
                                    "stale (materials untouched).",
                                    path);
            break;
        case Assisi::Geometry::ReconcileOutcome::Failed:
            Assisi::Core::Log::Warn("Asset reimport: could not reconcile '{}'.", path);
            break;
        }
        if (result.changedDisk)
            changed = true;
    }
    return changed;
}

bool EditorApp::IsAssetStale(std::string_view vpath) const
{
    return _staleMeshes.find(std::string{vpath}) != _staleMeshes.end();
}

// The two resolvers the reconcile/diff/regenerate calls need, backed by the
// database — a texture path→GUID and a material GUID→virtual-path.
namespace
{
Assisi::Core::AssetId ResolveTextureIdWith(const Assisi::Core::AssetDatabase &db, std::string_view vpath)
{
    return db.IdFor(vpath).value_or(Assisi::Core::AssetId{});
}
std::string ResolveMaterialPathWith(const Assisi::Core::AssetDatabase &db, const Assisi::Core::AssetId &id)
{
    return db.PathFor(id).value_or(std::string{});
}
} // namespace

void EditorApp::OpenStaleResolution(const std::string &vpath)
{
    _staleResolveTarget = vpath;
    _staleResolveDiff   = Assisi::Geometry::DiffGltfMaterials(
        vpath, [this](std::string_view p) { return ResolveTextureIdWith(_assetDatabase, p); },
        [this](const Assisi::Core::AssetId &id) { return ResolveMaterialPathWith(_assetDatabase, id); });
    _staleResolveRequestOpen = true;
}

void EditorApp::AdvanceStaleQueue()
{
    // Skip any queued mesh that is no longer stale (already resolved this session).
    while (!_staleResolveQueue.empty())
    {
        const std::string next = _staleResolveQueue.front();
        _staleResolveQueue.erase(_staleResolveQueue.begin());
        if (IsAssetStale(next))
        {
            OpenStaleResolution(next);
            return;
        }
    }
    _staleResolveTarget.clear(); // queue drained — modal stays closed
}

void EditorApp::ApplyStaleResolution(bool regenerate)
{
    const std::string target = _staleResolveTarget;
    if (target.empty())
    {
        return;
    }

    bool applied = false;
    if (regenerate)
    {
        const std::optional<std::size_t> slots = Assisi::Geometry::RegenerateGltfMaterials(
            target, [this](std::string_view p) { return ResolveTextureIdWith(_assetDatabase, p); },
            [this](const Assisi::Core::AssetId &id) { return ResolveMaterialPathWith(_assetDatabase, id); });
        applied = slots.has_value();
        if (applied)
        {
            Assisi::Core::Log::Info("Resolved '{}': regenerated {} material slot(s) from source.", target, *slots);
        }
        else
        {
            Assisi::Core::Log::Warn("Resolve '{}': regeneration failed; left stale.", target);
        }
    }
    else
    {
        applied = Assisi::Geometry::AcceptGltfSource(target);
        if (applied)
        {
            Assisi::Core::Log::Info("Resolved '{}': kept existing materials, accepted new source.", target);
        }
        else
        {
            Assisi::Core::Log::Warn("Resolve '{}': could not accept source; left stale.", target);
        }
    }

    if (applied)
    {
        _staleMeshes.erase(target);

        // Regeneration wrote new `.amat`s and rewrote the manifest, so bring the
        // database back in sync and re-resolve any live entity that draws this
        // mesh. Accepting the source touches only the glTF's hash — nothing the
        // database or a resolved entity depends on — so neither is needed there.
        if (regenerate)
        {
            if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            {
                Assisi::Core::Log::Info("Asset reimport: {} assets indexed after regenerate.", *rescan);
            }
            if (_scene != nullptr)
            {
                Assisi::Runtime::ResolveSceneAssets(*_scene, _assetCache, _assetDatabase);
            }
            _assetBrowserDirty = true;
        }
    }

    AdvanceStaleQueue(); // open the next live-stale mesh, or close the modal
}

void EditorApp::DrawStaleResolutionModal()
{
    static constexpr const char *kPopupId = "Resolve material conflict";
    if (_staleResolveRequestOpen)
    {
        ImGui::OpenPopup(kPopupId);
        _staleResolveRequestOpen = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.f, 0.f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }

    ImGui::TextWrapped("The source of '%s' changed in a way that can't be auto-resolved. "
                       "Choose how to reconcile its materials:",
                       _staleResolveTarget.c_str());
    ImGui::Separator();

    if (_staleResolveDiff.valid && !_staleResolveDiff.slots.empty())
    {
        if (ImGui::BeginTable("stale_slots", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 40.f);
            ImGui::TableSetupColumn("Material");
            ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthFixed, 90.f);
            ImGui::TableHeadersRow();
            for (const Assisi::Geometry::SlotDiff &slot : _staleResolveDiff.slots)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", slot.slot);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(slot.name.empty() ? "(unnamed)" : slot.name.c_str());
                ImGui::TableNextColumn();
                const char *label = "unchanged";
                ImVec4      color(0.6f, 0.6f, 0.6f, 1.f);
                switch (slot.change)
                {
                case Assisi::Geometry::SlotChange::Unchanged:
                    break;
                case Assisi::Geometry::SlotChange::Changed:
                    label = "changed";
                    color = ImVec4(0.90f, 0.63f, 0.16f, 1.f); // amber — a conflict
                    break;
                case Assisi::Geometry::SlotChange::Added:
                    label = "added";
                    color = ImVec4(0.45f, 0.78f, 0.45f, 1.f); // green — safe
                    break;
                case Assisi::Geometry::SlotChange::Removed:
                    label = "removed";
                    color = ImVec4(0.86f, 0.36f, 0.36f, 1.f); // red — a conflict
                    break;
                }
                ImGui::TextColored(color, "%s", label);
            }
            ImGui::EndTable();
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.86f, 0.36f, 0.36f, 1.f), "Could not read the current diff.");
    }

    ImGui::Separator();
    ImGui::TextWrapped("Regenerate from source overwrites the materials with the new source, discarding hand-edits "
                       "to changed slots (each material keeps its GUID). Keep my materials accepts the source and "
                       "stops flagging it, leaving every material untouched.");
    ImGui::Spacing();

    if (ImGui::Button("Regenerate from source"))
    {
        ApplyStaleResolution(/*regenerate=*/true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep my materials"))
    {
        ApplyStaleResolution(/*regenerate=*/false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later"))
    {
        AdvanceStaleQueue(); // leave it stale/badged; move to the next live one
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void EditorApp::SetupScene()
{
    auto *vulkanContext = Assisi::Render::RenderSystem::GetVulkanContext();
    if (!vulkanContext)
    {
        return;
    }

    nvrhi::IDevice *device = vulkanContext->GetDevice();

    const auto fbSize = GetWindow().GetFramebufferSize();

    // The asset cache owns the bindless material-texture table (stage D), which
    // the mesh pipeline binds — so initialise it before the scene renderer and
    // thread its layout/table through.
    _assetCache.Initialize(device, &Jobs());

    // Upload the unit collider silhouette meshes into a persistent arena (never
    // reset, so they survive level loads). The editor scales these per collider to
    // outline the collision volume; a capsule reuses the cylinder + sphere.
    _colliderArena.Initialize(device, sizeof(Assisi::Geometry::Vertex));
    _colliderBoxMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitCubeMesh());
    _colliderSphereMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitSphereMesh());
    _colliderCylinderMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitCylinderMesh());

    // The engine's default scene-render path owns lighting + the mesh pipeline.
    // Built against GetSceneFramebufferInfo() rather than the swapchain's own
    // FramebufferInfo so it's already correct if options.json saved an MSAA mode.
    // The editor opts into the overlay passes (selection outline, entity icons,
    // collider wireframes) — a game build leaves them off and never loads
    // assets/editor/**.
    if (!_sceneRenderer.Initialize({.device = device,
                                    .framebufferInfo = GetSceneFramebufferInfo(),
                                    .width = fbSize.Width,
                                    .height = fbSize.Height,
                                    .camera = _camera,
                                    .bindlessLayout = _assetCache.BindlessLayout(),
                                    .bindlessTable = _assetCache.BindlessTable(),
                                    .materialTable = _assetCache.MaterialTableBuffer(),
                                    .enableEditorVisuals = true}))
    {
        RequestClose();
        return;
    }
    // Thumbnails are drawn straight through ImGui, so they must not be sampled as
    // sRGB (which would gamma-decode them and show them too dark) — load linear.
    _thumbnailCache.Initialize(device, &Jobs(), Assisi::Render::ColorSpace::Linear);
    if (std::expected<void, Assisi::Core::AssetError> loaded =
            // Displayed through ImGui, not the mesh shader — load linear so the
            // sampler doesn't gamma-decode it (same reason as the thumbnails).
            _helloTexture.LoadFromAssets(device, "textures/hello.png", Assisi::Render::ColorSpace::Linear);
        !loaded)
    {
        Assisi::Core::Log::Warn("Failed to load textures/hello.png for the ImGui image test.");
    }
}

void EditorApp::OnResize(int32_t width, int32_t height)
{
    _sceneRenderer.Resize(width, height, _camera);
}

void EditorApp::OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (!_sceneRenderer.OnRenderTargetsChanged(framebufferInfo))
    {
        Assisi::Core::Log::Error("Failed to rebuild the mesh pass pipeline after a render-target change.");
    }
}

void EditorApp::OnRender(Assisi::Render::RenderFrame &frame)
{
    if (!_sceneRenderer.IsValid() || !_scene)
    {
        return;
    }

    // Blend physics-driven Transforms between their last two fixed-step poses
    // before the scene's world matrices are propagated in Render(), so bodies
    // move smoothly at the display's refresh rate rather than the physics rate.
    // Only while simulating: paused/stopped, physics must not stomp the Transforms
    // (an inspector edit or the frozen pose is authoritative then).
    if (IsSimulating())
    {
        _physics.InterpolateTransforms(*_scene, GetInterpolationAlpha());
    }

    // Refresh the editor camera's world matrix from its TRS before the view
    // matrix is derived from it; SceneRenderer propagates the game scene it draws.
    RefreshCameraMatrix();
    // The selected entity gets an always-on-top orange selection outline.
    _sceneRenderer.SetHighlightedEntity(_selectedEntity);
    // Editor entity icons show while authoring/paused, but not during live play.
    _sceneRenderer.SetEditorIconsVisible(_playState != PlayState::Playing);
    // Collider wireframes (editor-only) — queued before Render() consumes them.
    SubmitColliderWireframes();
    _sceneRenderer.Render(frame, *_scene, _cameraTransform, _camera);
}

void EditorApp::OnFixedUpdate(float dt)
{
    // Simulation ticks only while the game is running (Run, not Pause/Stop). When
    // it isn't, physics is frozen so the scene only renders — the editor camera
    // and picking still run from OnUpdate, which is not gated here.
    if (!_scene || !IsSimulating())
        return;
    // Game FixedUpdate systems run before the physics step (the Unity/Unreal
    // convention: apply forces this tick, then simulate them).
    _gameSystems.Run(Assisi::App::SystemPhase::FixedUpdate, {*_scene, dt, GetInput(), _actions, GetEvents()});
    _physics.Update(dt);
    // Snapshot the new poses for render interpolation; OnRender blends them.
    _physics.CaptureState();
}

void EditorApp::OnUpdate(float dt)
{
    auto &input = GetInput();
    if (input.IsKeyPressed(Assisi::Window::Key::Escape) && !ImGuiWantsKeyboard())
        RequestClose();

    if (!_scene)
        return;

    // Apply a requested undo/redo here, at the top of the frame: the previous
    // frame's FlushDestroyed has run (so a revived slot is free) and no render
    // command list is open (so scene mutation is safe).
    HandleUndoRedoHotkeys();

    // Delete key removes the selected entity (+ its subtree), undoably. Gated like
    // undo: only when a history is active and no text field owns the keyboard (so
    // Delete edits text there instead). Same safe mutation point as the undo above.
    if (Assisi::Editor::EditHistory *history = ActiveHistory();
        history != nullptr && !ImGui::GetIO().WantTextInput && _selectedEntity != Assisi::ECS::NullEntity &&
        _scene->IsAlive(_selectedEntity) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        DeleteEntity(_selectedEntity);
    }

    // A UI-requested level load is marshalled via Jobs().RunOnMain (see
    // EditorLevels) and applied in Application::Run's DrainMain, which runs just
    // before this — so the scene graph is here, but its meshes/materials stream in
    // asynchronously. Upgrade placeholders in place while loads are in flight.
    Assisi::App::UpgradeStreamingAssets(*_scene, _assetCache, _assetDatabase, _assetsWereLoading);

    _systems.Run(Assisi::App::SystemPhase::Update,    {*_scene, dt, input, _actions, GetEvents()});
    _systems.Run(Assisi::App::SystemPhase::PostUpdate, {*_scene, dt, input, _actions, GetEvents()});

    // Game logic ticks only while Playing — after the editor's own systems, in
    // the game registry's own phase order (see the EditorConfig seam contract).
    if (IsSimulating())
    {
        _gameSystems.Run(Assisi::App::SystemPhase::PreUpdate,  {*_scene, dt, input, _actions, GetEvents()});
        _gameSystems.Run(Assisi::App::SystemPhase::Update,     {*_scene, dt, input, _actions, GetEvents()});
        _gameSystems.Run(Assisi::App::SystemPhase::PostUpdate, {*_scene, dt, input, _actions, GetEvents()});
    }
}

void EditorApp::FlushDeferred()
{
    // End-of-frame: apply entities queued by Scene::Destroy() this frame. Runs
    // after RenderFrame, so a destroyed entity lives out its final frame before
    // its pools are touched — keeping structural changes out of any mid-frame Query.
    if (_scene)
        _scene->FlushDestroyed();
}

// ---------------------------------------------------------------------------
// Undo/redo (editor-only)
// ---------------------------------------------------------------------------

Assisi::Editor::EditHistory::RebindHook EditorApp::MakeEditRebindHook()
{
    return [this](Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, bool present)
    { ApplyEditRebind(entity, id, present); };
}

Assisi::Editor::EditHistory *EditorApp::ActiveHistory()
{
    switch (_playState)
    {
    case PlayState::Editing:
        return _history ? &*_history : nullptr;
    case PlayState::Paused:
        return _pausedHistory ? &*_pausedHistory : nullptr;
    case PlayState::Playing:
        return nullptr; // the simulation owns the scene; edits are neither captured nor undoable
    }
    return nullptr;
}

bool EditorApp::IsSceneDirty()
{
    // Dirty tracks the *editing* history (where saves happen), not a paused scratch.
    return _history.has_value() && _history->CurrentStateToken() != _savedStateToken;
}

std::string EditorApp::EntityDisplayName(Assisi::ECS::Entity entity) const
{
    if (_scene == nullptr || !_scene->IsAlive(entity))
        return {};
    const auto *name = _scene->Get<Assisi::Runtime::Name>(entity);
    if (name == nullptr || name->value.Empty())
        return {};
    char buf[Assisi::Core::kShortStringMax + 1];
    name->value.ToCStr(buf, sizeof(buf));
    return buf;
}

std::string EditorApp::EditLabel(std::string_view action, Assisi::ECS::Entity entity) const
{
    std::string label(action);
    if (_scene == nullptr || !_scene->IsAlive(entity))
        return label; // no entity to attribute it to (shouldn't happen for real edits)

    // Always attribute the edit to an entity: its Name if it has one, otherwise its
    // [index:generation] id, so every history row identifies what it acted on. A
    // plain ASCII " - " separator — the default ImGui font atlas has no em-dash
    // glyph, so "—" would render as "?".
    if (const std::string name = EntityDisplayName(entity); !name.empty())
    {
        label += " - " + name;
    }
    else
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " - Entity [%u:%u]", entity.index, entity.generation);
        label += buf;
    }
    return label;
}

void EditorApp::ApplyEditRebind(Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, bool present)
{
    // Dispatch by component identity to the same transient-rebuild paths the live
    // edits use. Ids resolve once (function-local statics in ComponentIdOf).
    using namespace Assisi;
    static const Core::Reflect::ComponentId kTransform = Core::Reflect::ComponentIdOf<Runtime::Transform>();
    static const Core::Reflect::ComponentId kRigidBodyDesc =
        Core::Reflect::ComponentIdOf<Physics::RigidBodyDescriptor>();
    static const Core::Reflect::ComponentId kMeshRenderer = Core::Reflect::ComponentIdOf<Runtime::MeshRenderer>();

    if (id == kTransform)
    {
        // A restored/edited Transform must drag any physics body to the new pose;
        // Add already re-stamped the change tick, so PropagateTransforms reruns.
        if (present)
        {
            const auto *rbc = _scene->Get<Physics::RigidBody>(entity);
            const auto *tc  = _scene->Get<Runtime::Transform>(entity);
            if (rbc && tc)
                _physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
        }
    }
    else if (id == kRigidBodyDesc)
    {
        if (present)
        {
            // Descriptor came back: rebuild the Jolt body from it (mirrors the
            // component-add path), unless a body somehow already exists.
            const auto *tc   = _scene->Get<Runtime::Transform>(entity);
            const auto *desc = _scene->Get<Physics::RigidBodyDescriptor>(entity);
            if (tc && desc && _scene->Get<Physics::RigidBody>(entity) == nullptr)
                _physics.AddBodyFromDescriptor(*_scene, entity, *tc, *desc);
        }
        else
        {
            // Descriptor removed: tear down its Jolt body + the transient handle
            // (RigidBody is ACOMP(transient), never in the payload).
            if (const auto *rbc = _scene->Get<Physics::RigidBody>(entity))
                _physics.RemoveBody(*rbc);
            _scene->Remove<Physics::RigidBody>(entity);
        }
    }
    else if (id == kMeshRenderer)
    {
        // Rebuild the mesh/material GPU pointers from the restored ids.
        if (present)
            ReresolveEntityAssets(entity);
    }
}

void EditorApp::HandleUndoRedoHotkeys()
{
    // Route to whichever history is live now — the main one while editing, the
    // scratch one while paused, none while playing (physics owns Transforms then).
    // Runs at the top of OnUpdate — a safe point to mutate.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history == nullptr)
        return;

    // Apply a History-panel jump requested last frame (deferred to this safe point).
    // Negative = undo N steps, positive = redo N.
    if (_pendingHistorySteps != 0)
    {
        std::optional<Assisi::ECS::Entity> restored;
        for (int32_t i = _pendingHistorySteps; i < 0; ++i)
            restored = history->Undo();
        for (int32_t i = _pendingHistorySteps; i > 0; --i)
            restored = history->Redo();
        _pendingHistorySteps = 0;
        if (restored.has_value())
            _selectedEntity = *restored;
    }

    const ImGuiIO &io = ImGui::GetIO();
    // Gate on WantTextInput, NOT WantCaptureKeyboard: the latter is true whenever
    // an ImGui window is merely focused (which it is right after any inspector
    // edit), so it would swallow Ctrl-Z for every ImGui-driven edit. WantTextInput
    // is true only while a text field is actively being typed into — exactly when
    // ImGui's own InputText undo should own Ctrl-Z instead (design notes §8.12).
    if (io.WantTextInput || !io.KeyCtrl)
        return;

    // Ctrl-Y or Ctrl-Shift-Z = redo; Ctrl-Z = undo.
    std::optional<Assisi::ECS::Entity> restoredSelection;
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
        restoredSelection = history->Redo();
    else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        restoredSelection = history->Undo();

    if (restoredSelection.has_value())
    {
        // Restore the selection the transaction recorded; a revived entity is valid
        // again, and a destroyed one lands on NullEntity (empty inspector).
        _selectedEntity = *restoredSelection;
    }
}

// ---------------------------------------------------------------------------
// ImGui panels
// ---------------------------------------------------------------------------

void EditorApp::DrawDiagnosticsWindow()
{
    ImGui::Begin("Diagnostics");
    ImGui::Text("FPS: %d", GetFps());
    ImGui::Text("CPU: %.2f ms   GPU: %.2f ms", GetCpuFrameMs(), GetGpuFrameMs());
    ImGui::Separator();

    // Physics collision substeps per fixed step. 1 is a single solve (relies on
    // speculative contacts + per-body CCD, like Unity/Unreal); raising it trades
    // CPU for shallower impact penetration. Watch the CPU ms above as you change it.
    int collisionSteps = _physics.GetCollisionSteps();
    if (ImGui::InputInt("Physics collision steps", &collisionSteps))
        _physics.SetCollisionSteps(collisionSteps);

    ImGui::Separator();
    ImGui::TextDisabled("RMB: look  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Scroll: FOV  |  LMB: select  |  Esc: quit");
    ImGui::TextDisabled("F11: graphics settings");
    ImGui::End();
}

void EditorApp::DrawHistoryWindow()
{
    ImGui::Begin("History");

    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history == nullptr)
    {
        ImGui::TextDisabled(_playState == PlayState::Playing ? "(history is off while playing)"
                                                             : "(no history)");
        ImGui::End();
        return;
    }
    if (_playState == PlayState::Paused)
        ImGui::TextDisabled("Paused — scratch history (cleared on resume/stop)");

    ImGui::BeginDisabled(!history->CanUndo());
    if (ImGui::Button("Undo"))
        _pendingHistorySteps = -1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history->CanRedo());
    if (ImGui::Button("Redo"))
        _pendingHistorySteps = +1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", history->UndoDepth(), history->RedoDepth());
    ImGui::Separator();

    // Photoshop-style linear view: oldest at top, the current state highlighted,
    // undone (future) entries greyed below. Clicking any row jumps there (deferred).
    const std::vector<std::string> undoLabels = history->UndoLabels(); // oldest -> newest
    const std::vector<std::string> redoLabels = history->RedoLabels(); // next-redo -> older

    // Base state (before any retained edit) — current when the undo stack is empty.
    if (ImGui::Selectable("(initial state)", history->UndoDepth() == 0) && history->UndoDepth() > 0)
        _pendingHistorySteps = -static_cast<int32_t>(history->UndoDepth());

    for (std::size_t i = 0; i < undoLabels.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const bool isCurrent = (i + 1 == undoLabels.size());
        if (ImGui::Selectable(undoLabels[i].c_str(), isCurrent) && !isCurrent)
            _pendingHistorySteps = static_cast<int32_t>(i + 1) - static_cast<int32_t>(undoLabels.size()); // undo down to i
        ImGui::PopID();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (std::size_t j = 0; j < redoLabels.size(); ++j)
    {
        ImGui::PushID(static_cast<int>(1000 + j));
        if (ImGui::Selectable(redoLabels[j].c_str()))
            _pendingHistorySteps = static_cast<int32_t>(j + 1); // redo forward to this entry
        ImGui::PopID();
    }
    ImGui::PopStyleColor();

    ImGui::End();
}

void EditorApp::OnImGui()
{
    // Reset the per-frame "an edit widget is being held" accumulator; the gizmo and
    // inspector raise it below, and the end-of-frame capture sweep reads it.
    _captureEditingActive = false;

    // First: resets ImGuizmo's per-frame state and draws the manipulator over the
    // scene (behind the panels below). Also refreshes IsUsingGizmo() for picking.
    DrawTransformGizmo();

    DrawOptionsWindow();
    DrawDiagnosticsWindow();
    DrawGameControlWindow();
    DrawEntityListWindow();
    DrawHistoryWindow();
    DrawLevelsWindow();
    DrawInspector();
    DrawHelloImageWindow();
    DrawAssetBrowser();
    DrawStaleResolutionModal();

    // After every panel has drawn (so each open gesture sees its final value):
    // commit finished drags/typing, drop no-ops, abandon dead-entity gestures.
    // Never mid-panel — the commit only reads the scene + stores JSON, but keeping
    // it at one point keeps the capture model simple. Playing captures nothing.
    if (Assisi::Editor::EditHistory *history = ActiveHistory())
        history->EndFrameSweep(_captureEditingActive);

    // Reflect unsaved changes in the OS window title, but only re-set it when the
    // dirty state actually flips (SetTitle every frame would be wasteful). The
    // base title is the game's own (game.json), not an editor hardcode.
    if (const bool dirty = IsSceneDirty(); dirty != _titleDirtyShown)
    {
        GetWindow().SetTitle(dirty ? GetConfig().title + " *" : GetConfig().title);
        _titleDirtyShown = dirty;
    }

    LogImGuiWedgeDiagnostics();
}

// ---------------------------------------------------------------------------
// ImGui input watchdog
// ---------------------------------------------------------------------------

void EditorApp::LogImGuiWedgeDiagnostics()
{
    // Diagnostic for the "ImGui stops responding until a new window opens"
    // report (2026-07-22): while any widget holds ActiveId, ImGui suppresses
    // hover on every other window — clicks land nowhere, windows can't be
    // dragged, ImGuizmo goes dead — yet the engine's own input (fly camera,
    // F11) keeps working. A widget holding ActiveId for seconds with NO mouse
    // button down and NO text field being edited is wedged, not in use; log
    // enough internal state to identify the widget and what freed it.
    // (Opening a window "fixes" the symptom because FocusWindow() clears a
    // stuck ActiveId — that's the reported F11 behavior.)
    const ImGuiIO      &io = ImGui::GetIO();
    const ImGuiContext &g  = *ImGui::GetCurrentContext();

    const bool anyMouseDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    // Wedge class 1: a widget holds ActiveId with no button down and no text
    // edit — hover is suppressed everywhere, so every window plays dead.
    const bool wedgedActiveId = g.ActiveId != 0 && !anyMouseDown && !io.WantTextInput;
    // Wedge class 2: ImGui has no valid mouse position even though the editor
    // is not fly-looking (mouse capture) and the app has OS focus — the
    // backend lost the cursor, so ImGui ignores all mouse input.
    const bool wedgedMousePos =
        !ImGui::IsMousePosValid() && !GetInput().IsMouseCaptured() && !io.AppFocusLost;
    const bool suspicious = wedgedActiveId || wedgedMousePos;

    if (!suspicious)
    {
        if (_imguiWedgeSeconds >= kImGuiWedgeThreshold)
        {
            Assisi::Core::Log::Warn("ImGui watchdog: wedge cleared after {:.1f}s (ActiveId now 0x{:08X}).",
                                    _imguiWedgeSeconds, g.ActiveId);
        }
        _imguiWedgeSeconds    = 0.f;
        _imguiWedgeNextReport = kImGuiWedgeThreshold;
        return;
    }

    _imguiWedgeSeconds += io.DeltaTime;
    if (_imguiWedgeSeconds < _imguiWedgeNextReport)
        return;
    _imguiWedgeNextReport = _imguiWedgeSeconds + 2.f; // re-report every ~2s while wedged

    const ImGuiWindow *modal = ImGui::GetTopMostPopupModal();
    Assisi::Core::Log::Warn(
        "ImGui watchdog [{}]: ActiveId=0x{:08X} in window '{}' (source {}) held {:.1f}s with no mouse button. "
        "hovered='{}' nav='{}' modal='{}' popups={} wantMouse={} wantKb={} mouse=({:.0f},{:.0f}) captured={}",
        wedgedActiveId ? (wedgedMousePos ? "activeId+mousePos" : "activeId") : "mousePos", g.ActiveId,
        g.ActiveIdWindow ? g.ActiveIdWindow->Name : "<none>", static_cast<int>(g.ActiveIdSource),
        _imguiWedgeSeconds, g.HoveredWindow ? g.HoveredWindow->Name : "<none>",
        g.NavWindow ? g.NavWindow->Name : "<none>", modal ? modal->Name : "<none>", g.OpenPopupStack.Size,
        io.WantCaptureMouse, io.WantCaptureKeyboard, static_cast<double>(io.MousePos.x),
        static_cast<double>(io.MousePos.y), GetInput().IsMouseCaptured());
}

} // namespace Assisi::Editor
