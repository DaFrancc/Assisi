/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Chiara/Profile.hpp>
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
#include <Assisi/NetSync/NetComponents.hpp>
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

    // Before any session can exist: the quantization is inside the handshake
    // hash, so it has to be settled before the first hello is written. The
    // smoothing is not — it is purely local — but it reads from the same file
    // and there is no reason to defer it.
    Assisi::NetSync::LoadQuantizationFromConfig();
    Assisi::NetSync::LoadSmoothingFromConfig();

    // What the manager needs to turn a level file into a running world when the
    // game travels. Installed once; captured by pointer, and all three outlive it.
    _worlds.SetServices({.cache    = &_assetCache,
                         .database = &_assetDatabase,
                         .renderer = &_sceneRenderer,
                         .jobs     = &Jobs()});

    // The game's systems become the **default profile**: every world the editor
    // builds gets its own instance of them, which is what keeps a system's
    // cross-frame state from advancing N× across resident worlds
    // (docs/world-system-binding-design-notes.md §3). The seam itself is
    // unchanged — it still hands out a SystemRegistry — so a game that knows
    // nothing about profiles keeps working.
    //
    // Registered before the first Create() below, because that world is given the
    // default profile the moment it exists.
    if (_editorConfig.registerGameSystems)
    {
        _worlds.RegisterProfile("Default",
                                [this](Assisi::App::World &world)
                                {
                                    _editorConfig.registerGameSystems(world.systems);

                                    // Once, not once per world: what this warns about is
                                    // how the game registered, not anything about a world.
                                    if (!_warnedGameRenderSystems && world.systems.HasRenderSystems())
                                    {
                                        _warnedGameRenderSystems = true;
                                        Assisi::Core::Log::Warn(
                                            "EditorApp: the game registered render system(s), which "
                                            "do not run in-editor — the editor owns rendering. They "
                                            "will run in the standalone game build only.");
                                    }
                                });
        _worlds.SetDefaultProfile("Default");
    }

    // Any further named profiles the game has, for levels that want a different
    // set than the default. After the default is registered, so a game can point
    // SetDefaultProfile at one of its own instead.
    if (_editorConfig.registerProfiles)
    {
        _editorConfig.registerProfiles(_worlds);
    }

    // The editor's starting world. It holds both roles: active (rendered,
    // input-driven) and edited (saved, dirtied, undone into). Opening a level
    // clears this world's scene in place rather than creating another one, which
    // is what keeps the history binding below valid for the whole session.
    _world = &_worlds.Create("Main");
    _worlds.SetActive(*_world);
    _worlds.SetEdited(*_world);
    _world->state    = Assisi::App::WorldState::Active;
    _world->simulate = false; // starts Editing; SetPlayState owns this from here on
    _scene           = &_world->scene;
    _physics         = &_world->physics;

    // Create() deliberately installs nothing, so a world built in memory (rather
    // than loaded from a level naming its profile) is given the default here. A
    // startup level opened below re-applies whatever that level asks for.
    _worlds.ApplyProfile(*_world, /*name=*/"");

    // Editor-only undo/redo. Binds the edited world's scene (stable for the
    // session — level loads Clear it in place, never swap the object). The rebind
    // hook rebuilds the transient state serialization drops after an apply. See
    // EditHistory.hpp.
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

    // A play-in-editor client: enter Play as a joiner straight away. Nothing
    // about this process is otherwise special — which is the point, since a
    // viewer that took a different code path would stop being a test of the
    // path it exists to test. The world it ends up showing is the host's, built
    // from the handshake (BuildJoinedWorld); whatever level was loaded above is
    // discarded on the way, and Stop brings it back.
    if (!_editorConfig.autoJoinEndpoint.empty())
    {
        std::string   address = "127.0.0.1";
        std::uint16_t port    = static_cast<std::uint16_t>(_netPort);
        const std::string_view endpoint = _editorConfig.autoJoinEndpoint;
        if (const std::size_t colon = endpoint.rfind(':'); colon != std::string_view::npos)
        {
            address = std::string(endpoint.substr(0, colon));
            const std::string portText(endpoint.substr(colon + 1));
            if (const int32_t parsed = std::atoi(portText.c_str()); parsed > 0 && parsed <= 65535)
                port = static_cast<std::uint16_t>(parsed);
        }
        else
        {
            address = std::string(endpoint);
        }

        std::snprintf(_netAddress.data(), _netAddress.size(), "%s", address.c_str());
        _netPort = static_cast<int32_t>(port);
        StartPlay(NetIntent::Join);
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

}

void EditorApp::ReimportAssets()
{
    // A restricted viewer shares one asset tree with the editor that spawned it,
    // and two processes minting ids into the same directory is a race whose
    // loser silently gets a different id for the same file. It indexes what is
    // already there and writes nothing.
    const Assisi::Core::RebuildMode mode = IsRestrictedViewer() ? Assisi::Core::RebuildMode::ReadOnly
                                                                : Assisi::Core::RebuildMode::Reconcile;
    const std::expected<std::size_t, Assisi::Core::AssetError> result = _assetDatabase.Rebuild(mode);
    if (!result)
    {
        Assisi::Core::Log::Warn("Asset reimport failed: asset root unavailable.");
        return;
    }
    Assisi::Core::Log::Info("Asset reimport: {} assets indexed ({}).", *result,
                            IsRestrictedViewer() ? "read-only scan" : "GUID sidecars reconciled");

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
    // ...and never in a restricted viewer, which is the writing half this whole
    // pass exists to do.
    if (!IsRestrictedViewer() && ReconcileMeshMaterials())
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
                                    .enableEditorVisuals = _editorConfig.enableEditorVisuals}))
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
        // Runs at display rate over every physics-driven Transform, so it belongs
        // in the render breakdown rather than being lumped in with physics.
        ASSISI_PROFILE_SCOPE("physics-interpolate");
        _physics->InterpolateTransforms(*_scene, GetInterpolationAlpha());
    }

    // Immediately after the writeback and before Render() propagates world
    // matrices: a bodied mirror's visual offset is *added on top of* the pose
    // the writeback just wrote, so this cannot run before it. A no-op unless
    // this editor is a connected client.
    {
        ASSISI_PROFILE_SCOPE("net-smooth-view");
        SmoothNetView();
    }

    // Refresh the editor camera's world matrix from its TRS before the view
    // matrix is derived from it; SceneRenderer propagates the game scene it draws.
    RefreshCameraMatrix();
    // Editor overlays (selection outline, entity icons, collider wireframes),
    // all gated on the F11 "Editor overlays" checkbox so the view can be
    // decluttered without touching how the scene itself renders. (Whether the
    // overlay *passes* even exist is a separate Initialize-time decision —
    // EditorConfig::enableEditorVisuals.)
    _sceneRenderer.SetHighlightedEntity(_showEditorOverlays ? _selectedEntity : Assisi::ECS::NullEntity);
    // Entity icons show while authoring/paused, but not during live play.
    _sceneRenderer.SetEditorIconsVisible(_showEditorOverlays && _playState != PlayState::Playing);
    if (_showEditorOverlays)
    {
        // Collider wireframes — queued before Render() consumes them. This is the
        // build half; `overlay-lines` inside Render() is the draw half.
        ASSISI_PROFILE_SCOPE("collider-wireframes");
        SubmitColliderWireframes();
    }
    // The propagation bookmark comes from the world, not the renderer: one
    // renderer will serve several worlds once more than one is resident.
    _sceneRenderer.Render(frame, *_scene, _cameraTransform, _camera, _world->propagationTick);
}

void EditorApp::OnFixedUpdate(float dt)
{
    if (!_scene)
        return;
    // The pump runs unconditionally even though a session now only exists inside
    // a play session (docs/replication-plan-v4.md §3.6). The two facts are not in
    // tension: `IsSimulating()` is false while Paused and during the frames a
    // join spends building its world, and both are states where the wire must
    // still be read — a join that stops polling never receives the handshake it
    // is waiting for, and a session whose host vanished must notice. Gating this
    // on IsSimulating() would deadlock the join and silently strand the other.
    //
    // Poll first: a command that arrived for this tick should be applied on this
    // tick, not the next one. It also advances the join state machine, which is
    // why it takes dt.
    PollNetSession(dt);

    // **In the editor, the play state is a flat switch: while it is not Playing,
    // nothing steps anywhere.** Not "the world you are looking at is frozen" —
    // Pause means the whole session is frozen, logic and physics alike, in every
    // resident world. Per-world `simulate` then selects among the worlds of a
    // *running* session; it never overrides the session being stopped, which is
    // what it used to do for any world other than the viewed one.
    //
    // A block rather than an early return, because the session tick below has to
    // run in either state — see PollNetSession's note above.
    if (IsSimulating())
    {
        // Every simulated world steps, and each runs its OWN FixedUpdate systems
        // immediately before its own physics — the Unity/Unreal convention (apply
        // forces this tick, then simulate them), now held per world rather than only
        // for the active one. Worlds step sequentially, which is what lets them share
        // one Jolt thread pool.
        //
        // Only Active worlds step: Dormant means "resident and inspectable, but not
        // stepped" (WorldState), and a stale `simulate` must not be able to break
        // that. Resuming while viewing the dormant edited world used to do exactly
        // that and step its physics.
        _worlds.ForEach(
            [this, dt](Assisi::App::World &world)
            {
                if (world.state != Assisi::App::WorldState::Active || !world.simulate)
                    return;

                world.systems.Run(Assisi::App::SystemPhase::FixedUpdate,
                                  {world, dt, GetSimTick(), &GetInput(), &_actions, GetEvents(),
                                   /*isActiveWorld=*/&world == _worlds.Active(), &_worlds});

                {
                    // Jolt's whole step, including its internal job dispatch. Everything
                    // under `fixed-update` that isn't a named ECS system is this.
                    ASSISI_PROFILE_SCOPE("physics-step");
                    world.physics.Update(dt);
                }
                {
                    // Snapshot the new poses for render interpolation; OnRender blends them.
                    // Linear in the body count, and separable from the solve — worth its own
                    // slice so a big scene says which of the two grew.
                    ASSISI_PROFILE_SCOPE("physics-capture");
                    world.physics.CaptureState();
                }
            });
    }

    // Between the step and the snapshot. A mirrored body woken by a contact the
    // server never had — client poses differ by whatever the last correction has
    // not yet removed, and Jolt wakes by island — has to be put back before
    // anything reads it, and before this frame's render writeback picks it up.
    if (_netSession)
        _netSession->AfterPhysicsStep();

    // Last: a snapshot describes the world at the *end* of the tick it is
    // stamped with, so it has to be built after everything that moves it.
    TickNetSession();
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
        _scene->IsAlive(_selectedEntity) && IsEditable(_selectedEntity) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        DeleteEntity(_selectedEntity);
    }

    // A joining client builds the host's level here, for the same reason: the
    // load frees the old asset set and re-resolves, and the fixed step that
    // noticed the handshake runs mid-frame. A failure inside sets _pendingStopPlay,
    // which the next block picks up on this same frame.
    if (_pendingJoinBuild)
    {
        _pendingJoinBuild = false;
        BuildJoinedWorld();
    }
    if (_pendingStopPlay)
    {
        _pendingStopPlay = false;
        StopPlay();
    }

    // A world requested from the Game panel is created here, at the frame's safe
    // point — the same reason level loads are marshalled: it resolves assets and
    // touches GPU resources this frame's draws may already reference.
    if (_pendingWorldLoad)
    {
        const std::string request = *_pendingWorldLoad;
        _pendingWorldLoad.reset();
        LoadLevelAsNewWorld(request);
    }
    if (_pendingTravel)
    {
        const std::string request = *_pendingTravel;
        _pendingTravel.reset();
        TravelToLevel(request);
    }

    // A travel a game system asked for last frame. Applied here rather than where
    // it was requested: systems run inside the walk over the resident worlds, and
    // travelling from there would invalidate that walk and could free the world
    // the system is running in. This is the safe point — before the frame's draws
    // are recorded, so freeing the outgoing world's GPU assets is legal.
    if (_worlds.HasTravelRequest())
    {
        if (Assisi::App::World *const arrived = _worlds.ProcessTravelRequest())
        {
            SetActiveWorld(*arrived);
            _worlds.SweepAssetCache();
        }
    }
    if (_pendingMigrate)
    {
        const std::string target = *_pendingMigrate;
        _pendingMigrate.reset();
        MigrateSelectionTo(target);
    }
    if (_pendingPreload)
    {
        const std::string request = *_pendingPreload;
        _pendingPreload.reset();
        BeginPreload(request);
    }
    if (_pendingPromote)
    {
        _pendingPromote = false;
        PromotePreloadedWorld();
    }

    // Advance a background preload: once its worker has deserialized, this resolves
    // and streams the new world's assets (main-thread GPU work) while it stays
    // hidden, so "ready" means meshes/materials are actually resident. Safe point:
    // it touches the asset cache.
    _worlds.PumpPendingLoad();

    // Drain decoded-and-waiting asset uploads into the GPU under a per-frame budget,
    // batched into one submit (P0/P0b). The async decode continuations that ran in
    // DrainMain above only enqueued these; the GPU work (the writeTexture staging
    // memcpy, arena writes, material rows) happens here, at the frame safe point
    // before RenderFrame, so it's visible to this frame's draws. Gentle tier while a
    // seamless preload streams behind the world you're playing; generous for a
    // foreground load (nothing to protect). A no-op when the queue is empty.
    if (_worlds.HasPendingLoad())
        _assetCache.PumpPublishes(/*timeBudgetMs=*/2.0, /*byteBudget=*/16ull << 20);
    else
        _assetCache.PumpPublishes(/*timeBudgetMs=*/10.0, /*byteBudget=*/128ull << 20);

    // A UI-requested level load is marshalled via Jobs().RunOnMain (see
    // EditorLevels) and applied in Application::Run's DrainMain, which runs just
    // before this — so the scene graph is here, but its meshes/materials stream in
    // asynchronously. Upgrade placeholders in place while loads are in flight.
    // Every resident world streams: a world you are not currently looking at would
    // otherwise keep its billboard placeholders forever. A world still Loading in
    // the background is skipped — a worker owns its scene until promotion.
    _worlds.ForEach([this](Assisi::App::World &world)
                    {
                        if (world.state == Assisi::App::WorldState::Loading)
                            return;
                        Assisi::App::UpgradeStreamingAssets(world.scene, _assetCache, _assetDatabase,
                                                            world.streamingPending);
                    });

    // A mirror arrives carrying authored asset ids and null resolved pointers,
    // and nothing else in this loop knows to look at it: UpgradeStreamingAssets
    // above only runs while the *cache* has loads in flight, which a spawn
    // arriving over the wire is not. This was v2's third integration gap — the
    // world replicated correctly and drew nothing. The client's structure
    // revision is the signal; resolving is idempotent, so acting on it late
    // costs a frame of billboard and never correctness.
    if (_netSession != nullptr && _netSession->Client() != nullptr)
    {
        if (const std::uint64_t revision = _netSession->Client()->StructureRevision();
            revision != _netStructureRevision)
        {
            _netStructureRevision = revision;
            Assisi::Runtime::ResolveSceneAssets(*_scene, _assetCache, _assetDatabase);
        }
    }

    // A viewer window that opens staring at nothing undermines the one-click
    // demo it exists for. Once, when the joined world has arrived.
    FrameJoinedWorldOnce();

    // Worlds that simulate but are not drawn get neither the pose write-back nor
    // the transform propagation the render path does for the world it draws. Give
    // them both, in that order — see App::SyncUnrenderedWorld. Skipped entirely
    // while the session is frozen: nothing stepped, so there are no new poses to
    // write back, and the same flat-freeze rule as OnFixedUpdate applies.
    if (IsSimulating())
    {
        _worlds.ForEach(
            [this](Assisi::App::World &world)
            {
                if (world.simulate && world.state == Assisi::App::WorldState::Active &&
                    &world != _world)
                {
                    Assisi::App::SyncUnrenderedWorld(world);
                }
            });
    }

    // The editor's own systems act on the world being *viewed* — picking, the fly
    // camera and selection all follow the world selector, not the played world.
    const Assisi::App::SystemContext editorCtx{
        *_world, dt, GetSimTick(), &input, &_actions, GetEvents(), /*isActiveWorld=*/true, &_worlds};
    _systems.Run(Assisi::App::SystemPhase::Update,     editorCtx);
    _systems.Run(Assisi::App::SystemPhase::PostUpdate, editorCtx);

    // Game logic ticks only while Playing, after the editor's own systems — and
    // now in EVERY simulated world, each out of its own registry, rather than
    // only in whichever world the editor happens to be looking at. Systems that
    // consume input opt out of the non-active worlds by declaring
    // ActiveWorldOnly() (one InputContext, N worlds).
    //
    // This also drops an old quirk: the game phases used to run against the
    // *viewed* world, so inspecting the dormant edited world mid-play ticked
    // game logic into it while its FixedUpdate stayed frozen. A world that is
    // not simulating now runs nothing, whichever one is on screen.
    if (IsSimulating())
    {
        _worlds.ForEach(
            [this, dt, &input](Assisi::App::World &world)
            {
                if (world.state != Assisi::App::WorldState::Active || !world.simulate)
                    return;

                const Assisi::App::SystemContext ctx{
                    world,   dt, GetSimTick(), &input, &_actions, GetEvents(),
                    /*isActiveWorld=*/&world == _worlds.Active(), &_worlds};
                world.systems.Run(Assisi::App::SystemPhase::PreUpdate,  ctx);
                world.systems.Run(Assisi::App::SystemPhase::Update,     ctx);
                world.systems.Run(Assisi::App::SystemPhase::PostUpdate, ctx);
            });
    }
}

void EditorApp::FlushDeferred()
{
    // End-of-frame: apply entities queued by Scene::Destroy() this frame. Runs
    // after RenderFrame, so a destroyed entity lives out its final frame before
    // its pools are touched — keeping structural changes out of any mid-frame Query.
    // Per world: a resident world's queued destroys must flush too, or they pile up
    // until it is next shown and then all land at once. A world still Loading in the
    // background is skipped — its scene belongs to a worker until promotion.
    _worlds.ForEach([](Assisi::App::World &world)
                    {
                        if (world.state != Assisi::App::WorldState::Loading)
                            world.scene.FlushDestroyed();
                    });
}

// ---------------------------------------------------------------------------
// Undo/redo (editor-only)
// ---------------------------------------------------------------------------

Assisi::Editor::EditHistory::RebindHook EditorApp::MakeEditRebindHook()
{
    return [this](Assisi::ECS::Entity entity, Assisi::Core::Reflect::ComponentId id, bool present)
    { ApplyEditRebind(entity, id, present); };
}

bool EditorApp::IsMirrored(Assisi::ECS::Entity entity) const
{
    return _scene != nullptr && _scene->IsAlive(entity) && _scene->Has<Assisi::NetSync::Mirrored>(entity);
}

bool EditorApp::IsEditable(Assisi::ECS::Entity entity) const { return IsEditable() && !IsMirrored(entity); }

bool EditorApp::IsEditable() const
{
    // Both histories bind the *edited* world's scene by reference, and Save writes
    // it — so an edit made while a different world is being shown would be captured
    // into the wrong scene's history and could never be saved. Other resident
    // worlds are inspect-only (docs/multi-scene-design-notes.md §1).
    return _world != nullptr && _world == _worlds.Edited();
}

Assisi::Editor::EditHistory *EditorApp::ActiveHistory()
{
    if (!IsEditable())
        return nullptr;

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
                _physics->SetBodyTransform(*rbc, tc->position, tc->rotation);
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
                _physics->AddBodyFromDescriptor(*_scene, entity, *tc, *desc);
        }
        else
        {
            // Descriptor removed: tear down its Jolt body + the transient handle
            // (RigidBody is ACOMP(transient), never in the payload).
            if (const auto *rbc = _scene->Get<Physics::RigidBody>(entity))
                _physics->RemoveBody(*rbc);
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
    int collisionSteps = _physics->GetCollisionSteps();
    if (ImGui::InputInt("Physics collision steps", &collisionSteps))
        _physics->SetCollisionSteps(collisionSteps);

    ImGui::Separator();
    ImGui::TextDisabled("RMB: look  |  WASD: move  |  Space/Ctrl: up/down");
    ImGui::TextDisabled("Scroll: FOV  |  LMB: select  |  Esc: quit");
    ImGui::TextDisabled("F11: graphics settings  |  F9: performance capture");
    ImGui::End();
}

void EditorApp::DrawChiaraWindow()
{
    // F9 toggles it, next to F11's graphics overlay. Handled here rather than in
    // the engine so the key stays the app's to rebind or drop — same reasoning as
    // F11. Chiara owns no input of its own.
    if (GetInput().IsKeyPressed(Assisi::Window::Key::F9))
    {
        _showChiara = !_showChiara;
    }

    if (!_showChiara)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Performance capture", &_showChiara))
    {
        // The panel is App-level so a game gets the same one; the editor only
        // decides where it lives and what opens it.
        DrawChiaraPanel();
    }
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

    // Same shape, for the physics freeze the Inspector applies while a field is
    // held (HandlePhysicsEditing). Released at the end of this function.
    _physicsFreezeRequested = false;

    // One scope per panel. A closed ImGui window still runs its Draw function (the
    // Begin() returns false and it early-outs), so a panel that is open and one
    // that is collapsed are both visible here — which is the point: "the editor UI
    // costs half a millisecond" is useless, "the asset browser costs half a
    // millisecond" is actionable.
    {
        // First: resets ImGuizmo's per-frame state and draws the manipulator over the
        // scene (behind the panels below). Also refreshes IsUsingGizmo() for picking.
        ASSISI_PROFILE_SCOPE("panel/gizmo");
        DrawTransformGizmo();
    }

    { ASSISI_PROFILE_SCOPE("panel/options");      DrawOptionsWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/diagnostics");  DrawDiagnosticsWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/chiara");       DrawChiaraWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/game-control"); DrawGameControlWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/network");      DrawNetworkWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/entity-list");  DrawEntityListWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/history");      DrawHistoryWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/levels");       DrawLevelsWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/inspector");    DrawInspector(); }
    { ASSISI_PROFILE_SCOPE("panel/hello-image");  DrawHelloImageWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/asset-browser"); DrawAssetBrowser(); }
    { ASSISI_PROFILE_SCOPE("panel/stale-modal");  DrawStaleResolutionModal(); }
    { ASSISI_PROFILE_SCOPE("panel/host-modal");   DrawHostUnsavedModal(); }

    // Release the Inspector's physics freeze here rather than inside the panel.
    // The panel cannot be trusted to observe its own release: DrawInspector
    // early-returns when nothing is selected, so deselecting (or destroying) the
    // entity on the same frame the drag ends skipped the restore entirely and left
    // the body Static for the rest of the session — visibly stuck in mid-air, with
    // a descriptor that still read dynamic. This runs unconditionally.
    if (!_physicsFreezeRequested)
        ThawEditedBody();

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
    // is not fly-looking (mouse capture) and the window HAS OS focus — the
    // backend lost the cursor, so ImGui ignores all mouse input. The focus
    // check must be the GLFW attrib (a level): io.AppFocusLost is an edge
    // ImGui clears every frame, and an unfocused window with the cursor
    // elsewhere legitimately has no mouse position (first watchdog session
    // logged exactly that pattern — a ~10s alt-tab, not a wedge). While
    // FOCUSED, the glfw backend re-polls the cursor as a fallback every
    // frame, so this firing at all means something upstream is truly stuck.
    const bool wedgedMousePos =
        !ImGui::IsMousePosValid() && !GetInput().IsMouseCaptured() && GetWindow().IsFocused();
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
        "hovered='{}' nav='{}' modal='{}' popups={} wantMouse={} wantKb={} mouse=({:.0f},{:.0f}) captured={} "
        "focused={}",
        wedgedActiveId ? (wedgedMousePos ? "activeId+mousePos" : "activeId") : "mousePos", g.ActiveId,
        g.ActiveIdWindow ? g.ActiveIdWindow->Name : "<none>", static_cast<int>(g.ActiveIdSource),
        _imguiWedgeSeconds, g.HoveredWindow ? g.HoveredWindow->Name : "<none>",
        g.NavWindow ? g.NavWindow->Name : "<none>", modal ? modal->Name : "<none>", g.OpenPopupStack.Size,
        io.WantCaptureMouse, io.WantCaptureKeyboard, static_cast<double>(io.MousePos.x),
        static_cast<double>(io.MousePos.y), GetInput().IsMouseCaptured(), GetWindow().IsFocused());
}

} // namespace Assisi::Editor
