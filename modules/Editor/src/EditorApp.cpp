/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include "EditorOptionsPanel.hpp"
#include "ImGuiQueries.hpp"

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
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
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetComponents.hpp>
#endif
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

EditorApp::EditorApp(EditorConfig config)
    : _editorConfig(std::move(config)), _options(std::make_unique<EditorOptionsPanel>())
{
    // Application reads this during Initialize(), which runs before OnStart, so
    // it cannot wait for a hook — hence the constructor body.
    SetRestrictedViewer(_editorConfig.restrictedViewer);
}

EditorApp::~EditorApp() = default;

// The panel cannot reach Application's timing history — it is protected, and
// only a derived class may read it. So this hands the frame over, and applies
// the one result the panel can report but not perform.
void EditorApp::DrawOptionsWindow()
{
    const FrameStatsView stats = GetFrameStats();
    const bool           applyDisplay =
        _options->Draw({.input              = GetInput(),
                        .renderer           = _sceneRenderer,
                        .options            = GetOptions(),
                        .showEditorOverlays = _showEditorOverlays,
                        .fps                = GetFps(),
                        .cpuFrameMs         = GetCpuFrameMs(),
                        .gpuFrameMs         = GetGpuFrameMs(),
                        .cpuMs              = stats.cpuMs,
                        .gpuMs              = stats.gpuMs,
                        .frameDeltaMs       = stats.frameDeltaMs,
                        .offset             = stats.offset,
                        .sampleCount        = stats.sampleCount});
    if (applyDisplay)
    {
        ApplyDisplayOptions();
    }
}

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
    // Projection is left at the member initialiser in EditorApp.hpp: 60 deg FOV,
    // 0.1..200 clip, active.
    RefreshCameraMatrix();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EditorApp::OnStart()
{
    // Action bindings from game.json. A missing or malformed file warns and leaves
    // the defaults.
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

    // Before any session can exist: quantization is inside the handshake hash, so
    // it has to be settled before the first hello is written. Smoothing is purely
    // local, but reads the same file.
#if defined(ASSISI_NETWORKING)
    Assisi::NetSync::LoadQuantizationFromConfig();
    Assisi::NetSync::LoadSmoothingFromConfig();
#endif

    // What the manager needs to turn a level file into a running world on travel.
    // Captured by pointer; every one of these outlives the manager.
    _worlds.SetServices({.cache    = &_assetCache,
                         .database = &_assetDatabase,
                         .renderer = &_sceneRenderer,
                         .jobs     = &Jobs()});

    // Warn about Render systems the game declared. Systems come from each level's
    // own list, resolved through SystemCatalog, which every ASYSTEM declaration in
    // the link has already populated — the game registers nothing here. Each world
    // gets its own instances of them, so a system's cross-frame state does not
    // advance N× across resident worlds
    // (docs/world-system-binding-design-notes.md).
    if (!_warnedGameRenderSystems && !Assisi::App::SystemCatalog::Instance().All().empty())
    {
        for (const Assisi::App::SystemDefinition &definition : Assisi::App::SystemCatalog::Instance().All())
        {
            if (!definition.isRender)
                continue;
            // Once per process, not once per world: the subject is how the game
            // declared its systems, not any particular world.
            _warnedGameRenderSystems = true;
            Assisi::Core::Log::Warn("EditorApp: system '{}' is a Render system, which does not run "
                                    "in-editor — the editor owns rendering. It will run in the "
                                    "standalone game build only.",
                                    definition.name);
        }
    }

    // The starting world holds both roles: active (rendered, input-driven) and
    // edited (saved, dirtied, undone into). Opening a level clears this world's
    // scene in place rather than creating another one — that is what keeps the
    // history binding below valid for the whole session.
    _world = &_worlds.Create("Main");
    _worlds.SetActive(*_world);
    _worlds.SetEdited(*_world);
    _world->state    = Assisi::App::WorldState::Active;
    _world->simulate = false; // starts Editing; SetPlayState owns this from here on
    _scene           = &_world->scene;
    _physics         = &_world->physics;

    // Empty system list: Create() installs nothing and a world built in memory
    // names no systems. A startup level opened below applies whatever it asks for.
    (void)_worlds.ApplySystems(*_world, {}, "(new world)");

    // Editor-only undo/redo, bound to the edited world's scene — stable for the
    // session, because level loads Clear it in place and never swap the object. The
    // rebind hook rebuilds the transient state serialization drops after an apply
    // (see EditHistory.hpp). The instance table comes with it: an edit to a
    // blueprint member has to move the instance's override record in the same
    // transaction, or undo takes the value back and leaves the record claiming the
    // instance changed it.
    _history.emplace(*_scene, MakeEditRebindHook(), &_world->instances);

    // Give every asset a `.aast` GUID sidecar and build the GUID→path database.
    // Editor-only, hence here and not in Application: a shipped game consumes a
    // baked index and never scans or writes.
    ReimportAssets();

    SetupCamera();
    SetupScene();
    ScanLevels();
    ScanBlueprints();

    // Startup level from EditorConfig, so `<editor-exe> levels/Materials.alvl`
    // boots straight into that scene. Resolved through the asset system like any
    // other asset; a missing or mistyped path only warns.
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

    // A play-in-editor client: enter Play as a joiner straight away. Nothing else
    // about this process is special, deliberately — a viewer down a different code
    // path would stop testing the path it exists to test. The world it ends up
    // showing is the host's, built from the handshake in BuildJoinedWorld; whatever
    // level was loaded above is discarded on the way, and Stop brings it back.
#if defined(ASSISI_NETWORKING)
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
#endif // ASSISI_NETWORKING

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
                              SelectEntity(e.entity,
                                           e.additive ? SelectMode::Toggle : SelectMode::Replace);
                          }
                      });

}

void EditorApp::ReimportAssets()
{
    // A restricted viewer indexes what is there and writes nothing: it shares one
    // asset tree with the editor that spawned it, and two processes minting ids
    // into the same directory is a race whose loser silently gets a different id
    // for the same file.
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

    // Wire the rebuilt database into serialization's path hint and the asset
    // cache's id↔path translation. The resolvers hold the database by reference,
    // so re-running reimport needs no reinstall; doing it anyway is harmless.
    Assisi::App::InstallAssetResolvers(_assetCache, _assetDatabase);

    // Bring glTF materials up to date: explode new meshes, reconcile already
    // exploded ones against their current source. Any write means new or updated
    // files and manifests, so rebuild afterwards to bring them into the database
    // (steady state changes nothing and skips it). The hint resolver installed
    // just above is what gives written `.amat` channels their path hints.
    //
    // Never in a restricted viewer: writing is the whole of this pass.
    if (!IsRestrictedViewer() && ReconcileMeshMaterials())
    {
        if (const std::expected<std::size_t, Assisi::Core::AssetError> rescan = _assetDatabase.Rebuild(); rescan)
            Assisi::Core::Log::Info("Asset reimport: {} assets indexed after material reconcile.", *rescan);
    }

    // Queue a resolution prompt for every stale mesh the open scene actually draws:
    // those cannot wait for the author to click their badge. Stale meshes nothing
    // draws just keep the badge, resolved on click or on the next load. At startup
    // the scene has no MeshRenderers yet, so this finds nothing; it fires on the
    // manual Reimport button with a level loaded.
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

    // Force the browser to re-read on next open: a reimport may have created files
    // it lists (it filters by extension and never shows `.aast` itself).
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
    // same map the asset cache uses, so an exploded material references the same
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

        // Already exploded: reconcile against the current source instead.
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

// The two resolvers the reconcile/diff/regenerate calls take: texture
// path→GUID and material GUID→virtual-path, both backed by the database.
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
        // database back in sync and re-resolve every live entity drawing this mesh.
        // Accepting the source touches only the glTF's hash, which neither the
        // database nor a resolved entity depends on.
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

    // Before the scene renderer: the asset cache owns the bindless material-texture
    // table the mesh pipeline binds, and its layout/table are threaded through below.
    _assetCache.Initialize(device, &Jobs());

    // Unit collider silhouettes, in a persistent arena that is never reset, so they
    // survive level loads. The editor scales these per collider to outline the
    // collision volume; a capsule reuses the cylinder and sphere.
    _colliderArena.Initialize(device, sizeof(Assisi::Geometry::Vertex));
    _colliderBoxMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitCubeMesh());
    _colliderSphereMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitSphereMesh());
    _colliderCylinderMesh.Upload(_colliderArena, Assisi::Geometry::CreateUnitCylinderMesh());

    // The engine's default scene-render path: lighting plus the mesh pipeline.
    // Built against GetSceneFramebufferInfo() rather than the swapchain's own
    // FramebufferInfo, so it is already correct if options.json saved an MSAA mode.
    // The editor opts into the overlay passes (selection outline, entity icons,
    // collider wireframes); a game build leaves them off and never loads
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
    // Linear, not sRGB: thumbnails are drawn straight through ImGui, and sampling
    // them as sRGB would gamma-decode them and show them too dark.
    _thumbnailCache.Initialize(device, &Jobs(), Assisi::Render::ColorSpace::Linear);
    if (std::expected<void, Assisi::Core::AssetError> loaded =
            // Linear for the same reason as the thumbnails: ImGui, not the mesh shader.
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

    // Blend physics-driven Transforms between their last two fixed-step poses,
    // before Render() propagates world matrices, so bodies move at the display's
    // refresh rate rather than the physics rate. Only while simulating: paused or
    // stopped, physics must not stomp the Transforms, because an inspector edit or
    // the frozen pose is authoritative then.
    if (IsSimulating())
    {
        // Display rate over every physics-driven Transform, so it belongs in the
        // render breakdown rather than being lumped in with physics.
        ASSISI_PROFILE_SCOPE("physics-interpolate");
        // The resolver is required: a parented body's pose comes back in world
        // space while its Transform is an offset from its parent, so without it
        // every parented body drifts by its parent's transform once per frame.
        _physics->InterpolateTransforms(*_scene, GetInterpolationAlpha(),
                                        Assisi::App::ParentWorldResolver(*_scene));
    }

    // Must stay between the writeback above and Render()'s propagation: a bodied
    // mirror's visual offset is added *on top of* the pose the writeback just
    // wrote. A no-op unless this editor is a connected client.
#if defined(ASSISI_NETWORKING)
    {
        ASSISI_PROFILE_SCOPE("net-smooth-view");
        SmoothNetView();
    }
#endif

    // The editor camera is not part of the drawn scene, so nothing else propagates
    // it: refresh its world matrix before the view matrix is derived from it.
    RefreshCameraMatrix();
    // Editor overlays (selection outline, entity icons, collider wireframes) are
    // gated on the F11 "Editor overlays" checkbox, so the view can be decluttered
    // without touching how the scene renders. Whether the overlay *passes* exist at
    // all is a separate Initialize-time decision: EditorConfig::enableEditorVisuals.
    if (_showEditorOverlays)
        _sceneRenderer.SetHighlightedEntities(_selection);
    else
        _sceneRenderer.SetHighlightedEntity(Assisi::ECS::NullEntity);
    // Which of the highlighted entities is the one being edited. Must come after
    // the list above: SetHighlightedEntity(Null) clears this as part of clearing
    // the selection.
    _sceneRenderer.SetActiveHighlight(_showEditorOverlays ? _selectedEntity : Assisi::ECS::NullEntity);
    // Entity icons show while authoring/paused, but not during live play.
    _sceneRenderer.SetEditorIconsVisible(_showEditorOverlays && _playState != PlayState::Playing);
    if (_showEditorOverlays)
    {
        // Build half of the collider wireframes, queued before Render() consumes
        // them; `overlay-lines` inside Render() is the draw half.
        ASSISI_PROFILE_SCOPE("collider-wireframes");
        SubmitColliderWireframes();

        // A billboard where each instance was placed. An instance's root is a table
        // row rather than an entity, so nothing in the scene marks it otherwise —
        // and an unmarked origin is also an unclickable one.
        SubmitInstanceIcons();
    }
    // The propagation bookmark comes from the world, not the renderer: one renderer
    // serves several worlds once more than one is resident.
    _sceneRenderer.Render(frame, *_scene, _cameraTransform, _camera, _world->propagationTick);
}

void EditorApp::OnFixedUpdate(float dt)
{
    if (!_scene)
        return;
    // **Never gate this on IsSimulating().** It is false while Paused and during
    // the frames a join spends building its world, and both are states where the
    // wire must still be read: a join that stops polling never receives the
    // handshake it is waiting for, and a session whose host vanished must notice.
    //
    // First in the tick, so a command that arrived for this tick is applied on this
    // tick rather than the next. It also advances the join state machine, which is
    // why it takes dt.
#if defined(ASSISI_NETWORKING)
    PollNetSession(dt);
#endif

    // **The play state is a flat switch: while it is not Playing, nothing steps
    // anywhere.** Pause freezes the whole session — logic and physics, in every
    // resident world — not merely the world on screen. Per-world `simulate` selects
    // among the worlds of a *running* session; it never overrides a stopped one.
    //
    // A block rather than an early return: the session tick below runs in either
    // state, for the reason above it.
    if (IsSimulating())
    {
        // Every simulated world steps, each running its OWN FixedUpdate systems
        // immediately before its own physics — the Unity/Unreal convention of apply
        // forces this tick, then simulate them. Worlds step sequentially, which is
        // what lets them share one Jolt thread pool.
        //
        // Both conditions matter. Dormant means "resident and inspectable, but not
        // stepped", and a stale `simulate` must not be able to break that: resuming
        // while viewing the dormant edited world once stepped its physics.
        _worlds.ForEach(
            [this, dt](Assisi::App::World &world)
            {
                if (world.state != Assisi::App::WorldState::Active || !world.simulate)
                    return;

                world.systems.Run(Assisi::App::SystemPhase::FixedUpdate,
                                  {world, dt, GetSimTick(), &GetInput(), &_actions, GetEvents(),
                                   /*isActiveWorld=*/&world == _worlds.Active(), &_worlds});

                {
                    // Jolt's whole step, including its internal job dispatch:
                    // everything under `fixed-update` that is not a named ECS system.
                    ASSISI_PROFILE_SCOPE("physics-step");
                    world.physics.Update(dt);
                }
                {
                    // Snapshot the new poses for OnRender to blend. Linear in the body
                    // count and separable from the solve, so it gets its own slice: a
                    // big scene then says which of the two grew.
                    ASSISI_PROFILE_SCOPE("physics-capture");
                    world.physics.CaptureState();
                }
            });
    }

    // Between the step and the snapshot, and it has to stay there. A mirrored body
    // woken by a contact the server never had (client poses differ by whatever the
    // last correction has not yet removed, and Jolt wakes by island) must be put
    // back before anything reads it, including this frame's render writeback.
#if defined(ASSISI_NETWORKING)
    if (_netSession)
        _netSession->AfterPhysicsStep();
#endif

    // Last: a snapshot describes the world at the *end* of the tick it is stamped
    // with, so it has to be built after everything that moves it.
#if defined(ASSISI_NETWORKING)
    TickNetSession();
#endif
}

void EditorApp::OnUpdate(float dt)
{
    auto &input = GetInput();
    if (input.IsKeyPressed(Assisi::Window::Key::Escape) && !ImGuiWantsKeyboard())
        RequestClose();

    if (!_scene)
        return;

    // Top of the frame is the safe point for undo/redo: the previous frame's
    // FlushDestroyed has run, so a revived slot is free, and no render command list
    // is open, so mutating the scene is legal.
    HandleUndoRedoHotkeys();

    // Reconcile the selection with the scene before anything reads it. Several
    // paths move `_selectedEntity` on their own — a level load, switching worlds,
    // the undo just applied restoring a transaction's selection — and none know
    // about the rest of the list; a stale handle would outline a slot something
    // else has since taken.
    PruneSelection();

    // Delete removes the selection and each subtree, undoably. Gated like undo:
    // only with a history active and no text field owning the keyboard, so Delete
    // edits text there instead. Same safe mutation point as the undo above.
    if (Assisi::Editor::EditHistory *history = ActiveHistory();
        history != nullptr && !ImGui::GetIO().WantTextInput && _selectedEntity != Assisi::ECS::NullEntity &&
        _scene->IsAlive(_selectedEntity) && IsEditable(_selectedEntity) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        DeleteSelection();
    }

    // --- The frame's safe point ---------------------------------------------
    // Everything in the run of `_pending*` blocks below is work another code path
    // asked for and could not perform where it asked. Loading a level, creating or
    // destroying a world, and opening a blueprint all free GPU assets that draws
    // already recorded this frame still reference; panels and game systems both run
    // mid-frame. So each one raises a flag and is honoured here instead — before
    // this frame's draws are recorded, and outside any walk over the resident
    // worlds. Do not move this work back to its caller.

    // A joining client builds the host's level; the fixed step that noticed the
    // handshake runs mid-frame. A failure inside raises _pendingStopPlay, which the
    // next block picks up on this same frame.
#if defined(ASSISI_NETWORKING)
    if (_pendingJoinBuild)
    {
        _pendingJoinBuild = false;
        BuildJoinedWorld();
    }
#endif
#if defined(ASSISI_NETWORKING)
    if (_pendingStopPlay)
    {
        _pendingStopPlay = false;
        StopPlay();
    }
#endif

    // Worlds requested from the Game panel.
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

    // Opening a blueprint loads assets; closing destroys a world.
    if (_pendingBlueprintOpen)
    {
        const std::string request = *_pendingBlueprintOpen;
        _pendingBlueprintOpen.reset();
        OpenBlueprintForEditing(request);
    }
    if (_pendingBlueprintClose)
    {
        _pendingBlueprintClose = false;
        CloseBlueprintEditor();
    }

    // The blueprint rig's ambient, re-applied every frame rather than at the two
    // transitions: the renderer is shared by every world, so a knob left turned up
    // would light the level the author went back to.
    if (InBlueprintMode())
        _sceneRenderer.SetAmbient(_blueprintAmbientColor, _blueprintAmbient);
    else
        _sceneRenderer.SetAmbient(glm::vec3(1.f), Assisi::Render::kDefaultAmbientIntensity);

    // A travel a game system asked for last frame. Systems run inside the walk over
    // the resident worlds, and travelling from there would invalidate that walk and
    // could free the world the system is running in.
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

    // Advance a background preload. Once its worker has deserialized, this resolves
    // and streams the new world's assets — main-thread GPU work — while it stays
    // hidden, so "ready" means its meshes and materials are actually resident.
    _worlds.PumpPendingLoad();

    // Drain decoded-and-waiting asset uploads to the GPU under a per-frame budget,
    // batched into one submit. The async decode continuations that ran in DrainMain
    // only enqueued these; the GPU work (the writeTexture staging memcpy, arena
    // writes, material rows) happens here, before RenderFrame, so it is visible to
    // this frame's draws. The gentle tier keeps a seamless preload from stealing
    // frame time from the world being played; a foreground load has nothing to
    // protect and gets the generous one. A no-op on an empty queue.
    if (_worlds.HasPendingLoad())
        _assetCache.PumpPublishes(/*timeBudgetMs=*/2.0, /*byteBudget=*/16ull << 20);
    else
        _assetCache.PumpPublishes(/*timeBudgetMs=*/10.0, /*byteBudget=*/128ull << 20);

    // Swap billboard placeholders for the real mesh/material as each finishes
    // streaming. A UI-requested level load is marshalled via Jobs().RunOnMain (see
    // EditorLevels) and applied in Application::Run's DrainMain, just before this,
    // so the scene graph is already here while its assets are still arriving.
    // Every resident world is upgraded, not only the visible one, or a world you
    // are not looking at keeps its placeholders forever. A world still Loading is
    // skipped: a worker owns its scene until promotion.
    _worlds.ForEach([this](Assisi::App::World &world)
                    {
                        if (world.state == Assisi::App::WorldState::Loading)
                            return;
                        Assisi::App::UpgradeStreamingAssets(world.scene, _assetCache, _assetDatabase,
                                                            world.streamingPending);
                    });

    // Resolve assets for entities that arrived over the wire. A mirror carries
    // authored asset ids and null resolved pointers, and nothing else here would
    // look at it: UpgradeStreamingAssets above only acts while the *cache* has
    // loads in flight, which a spawn arriving over the wire is not. Without this
    // the world replicates correctly and draws nothing. The client's structure
    // revision is the signal; resolving is idempotent, so acting on it late costs
    // a frame of billboard and never correctness.
#if defined(ASSISI_NETWORKING)
    if (_netSession != nullptr && _netSession->Client() != nullptr)
    {
        if (const std::uint64_t revision = _netSession->Client()->StructureRevision();
            revision != _netStructureRevision)
        {
            _netStructureRevision = revision;
            Assisi::Runtime::ResolveSceneAssets(*_scene, _assetCache, _assetDatabase);
        }
    }
#endif

    // Worlds that simulate but are not drawn get neither the pose write-back nor
    // the transform propagation the render path performs for the world it draws.
    // Give them both, in that order (see App::SyncUnrenderedWorld). Skipped while
    // the session is frozen: nothing stepped, so there are no new poses.
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

    // The editor's own systems act on the world being *viewed*: picking, the fly
    // camera and selection follow the world selector, not the played world.
    const Assisi::App::SystemContext editorCtx{
        *_world, dt, GetSimTick(), &input, &_actions, GetEvents(), /*isActiveWorld=*/true, &_worlds};
    _systems.Run(Assisi::App::SystemPhase::Update,     editorCtx);
    _systems.Run(Assisi::App::SystemPhase::PostUpdate, editorCtx);

    // Game logic ticks only while Playing, after the editor's own systems, in every
    // simulated world and out of each world's own registry — never against the
    // *viewed* world, which used to tick game logic into the dormant edited world
    // while its FixedUpdate stayed frozen. A world that is not simulating runs
    // nothing, whichever one is on screen. Systems that consume input opt out of
    // the non-active worlds by declaring ActiveWorldOnly(): one InputContext, N
    // worlds.
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

void EditorApp::InstallQueuedSystems()
{
    // Every resident world, not just the active one: a blueprint can be spawned
    // into a background world, and a queue nobody drains leaves an entity holding
    // its components and running none of the code.
    //
    // Loading worlds included, harmlessly: their scene belongs to a worker until
    // promotion, but the system registry is main-thread only, and promotion runs
    // ApplySystems, which clears the queue anyway.
    _worlds.ForEach([](Assisi::App::World &world) { Assisi::App::DrainSystemInstalls(world); });
}

void EditorApp::FlushDeferred()
{
    // Apply the entities Scene::Destroy() queued this frame. Runs after
    // RenderFrame, so a destroyed entity lives out its final frame before its pools
    // are touched — which is what keeps structural changes out of any mid-frame
    // Query. Every resident world, or a background world's destroys pile up until
    // it is next shown and then land all at once. A world still Loading is skipped:
    // its scene belongs to a worker until promotion.
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

// **Keep this definition here, not in EditorNet.cpp.** Everything it does is
// networking, but it is a lifecycle override, and defining it in a file that
// leaves the build leaves a hole in the vtable.
void EditorApp::OnShutdown()
{
#if defined(ASSISI_NETWORKING)
    // Closing the window ends a play session. The two things that outlive this
    // process if nobody says otherwise are a socket and a fleet of viewer windows.
    // Deliberately *not* a full StopPlay: its scene restore re-resolves assets
    // against a renderer already on its way down, and nothing will look at the
    // result.
    ShutdownNetSession();
    ShutdownPieClients();
#endif
}

bool EditorApp::IsMirrored(Assisi::ECS::Entity entity) const
{
#if defined(ASSISI_NETWORKING)
    return _scene != nullptr && _scene->IsAlive(entity) && _scene->Has<Assisi::NetSync::Mirrored>(entity);
#else
    // No replication, so nothing in this scene came from anyone else: every entity
    // is authored here and therefore editable.
    (void)entity;
    return false;
#endif
}

bool EditorApp::IsEditable(Assisi::ECS::Entity entity) const { return IsEditable() && !IsMirrored(entity); }

std::string EditorApp::DescribeEntity(Assisi::ECS::Entity entity) const
{
    if (entity == Assisi::ECS::NullEntity || _scene == nullptr)
        return "(none)";

    // A blueprint member's instance comes first, because that is the part that
    // disambiguates: a level of forty cars has forty entities called "wheel_fl".
    if (_world != nullptr)
    {
        if (const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(entity))
        {
            if (const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(tag->instanceId))
            {
                const Assisi::Runtime::BlueprintResult definition =
                    Assisi::Runtime::GetBlueprintDefinition(row->source);
                const std::string memberPath =
                    definition && tag->memberIndex < (*definition)->members.size()
                        ? (*definition)->members[tag->memberIndex].name
                        : std::format("#{}", tag->memberIndex);

                return std::format("{} › {}", row->name.empty() ? row->source : row->name, memberPath);
            }
        }
    }

    if (const auto *name = _scene->Get<Assisi::Runtime::Name>(entity); name != nullptr && !name->value.Empty())
        return std::string{name->value.View()};

    return std::format("Entity [{}:{}]", entity.index, entity.generation);
}

bool EditorApp::IsEditable() const
{
    // Every history binds the *edited* world's scene by reference, and Save writes
    // that scene — so an edit made while a different world is shown would be
    // captured into the wrong scene's history and could never be saved. Other
    // resident worlds are inspect-only (docs/multi-scene-design-notes.md).
    return _world != nullptr && _world == _worlds.Edited();
}

Assisi::Editor::EditHistory *EditorApp::ActiveHistory()
{
    if (!IsEditable())
        return nullptr;

    switch (_playState)
    {
    case PlayState::Editing:
        // The blueprint world has its own stack and the level's stays where it
        // was: an edit in one must never land in the other's history.
        if (InBlueprintMode())
            return _blueprintHistory ? &*_blueprintHistory : nullptr;
        return _history ? &*_history : nullptr;
    case PlayState::Paused:
        return _pausedHistory ? &*_pausedHistory : nullptr;
    case PlayState::Playing:
        return nullptr; // the simulation owns the scene; edits are neither captured nor undoable
    }
    return nullptr;
}

std::vector<Assisi::Editor::EditHistory *> EditorApp::AllHistories()
{
    std::vector<Assisi::Editor::EditHistory *> histories;
    histories.reserve(3);
    for (std::optional<Assisi::Editor::EditHistory> *slot : {&_history, &_pausedHistory, &_blueprintHistory})
    {
        if (*slot)
            histories.push_back(&**slot);
    }
    return histories;
}

bool EditorApp::IsSceneDirty()
{
    // Tracks the *editing* history, where saves happen, never the paused scratch.
    if (InBlueprintMode())
    {
        return _blueprintHistory.has_value() &&
               _blueprintHistory->CurrentStateToken() != _blueprintSavedToken;
    }
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

    // Attribute the edit to an entity — its Name, or its [index:generation] id —
    // so every history row identifies what it acted on. The separator is plain
    // ASCII " - ": the default ImGui font atlas has no em-dash glyph, so "—" would
    // render as "?".
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
    // edits use. Ids resolve once, via the function-local statics in ComponentIdOf.
    using namespace Assisi;
    static const Core::Reflect::ComponentId kTransform = Core::Reflect::ComponentIdOf<Runtime::Transform>();
    static const Core::Reflect::ComponentId kRigidBodyDesc =
        Core::Reflect::ComponentIdOf<Physics::RigidBodyDescriptor>();
    static const Core::Reflect::ComponentId kMeshRenderer = Core::Reflect::ComponentIdOf<Runtime::MeshRenderer>();

    if (id == kTransform)
    {
        // A restored or edited Transform drags any physics body to the new pose.
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
            // Descriptor came back: rebuild the Jolt body from it, mirroring the
            // component-add path, unless a body somehow already exists.
            const auto *tc   = _scene->Get<Runtime::Transform>(entity);
            const auto *desc = _scene->Get<Physics::RigidBodyDescriptor>(entity);
            if (tc && desc && _scene->Get<Physics::RigidBody>(entity) == nullptr)
                _physics->AddBodyFromDescriptor(*_scene, entity, *tc, *desc,
                                                Assisi::App::ParentWorldResolver(*_scene));
        }
        else
        {
            // Descriptor removed: tear down its Jolt body and the transient handle.
            // RigidBody is ACOMP(transient), so it is never in the payload.
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
    // Routes to whichever history is live: the main one while editing, the scratch
    // one while paused, none while playing (physics owns Transforms then). Called
    // from the top of OnUpdate, which is where mutating the scene is safe.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history == nullptr)
        return;

    // A History-panel jump requested last frame and deferred to here. Negative is
    // undo N steps, positive is redo N.
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
    // **Gate on WantTextInput, not WantCaptureKeyboard.** The latter is true
    // whenever an ImGui window is merely focused, which it is right after any
    // inspector edit, so it swallows Ctrl-Z for every ImGui-driven edit.
    // WantTextInput is true only while a text field is being typed into — exactly
    // when ImGui's own InputText undo should own Ctrl-Z instead.
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
        // The selection the transaction recorded: a revived entity is valid again,
        // and a destroyed one lands on NullEntity, i.e. an empty inspector.
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

    // Collision substeps per fixed step. 1 is a single solve, relying on
    // speculative contacts and per-body CCD like Unity/Unreal; raising it trades
    // CPU for shallower impact penetration. Watch the CPU ms above while changing it.
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
    // F9 toggles it, alongside F11's graphics overlay. The key lives here rather
    // than in the engine so it stays the app's to rebind or drop; Chiara owns no
    // input of its own.
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
        // App-level, so a game gets the same panel; the editor only decides where
        // it lives and what opens it.
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
    // undone (future) entries greyed below. Clicking a row defers a jump to it.
    const std::vector<std::string> undoLabels = history->UndoLabels(); // oldest -> newest
    const std::vector<std::string> redoLabels = history->RedoLabels(); // next-redo -> older

    // The base state, before any retained edit; current when the undo stack is empty.
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
    // Per-frame "an edit widget is being held" accumulator, raised by the gizmo and
    // the inspector below and read by the end-of-frame capture sweep.
    _captureEditingActive = false;

    // Same shape, for the physics freeze the Inspector applies while a field is
    // held (HandlePhysicsEditing). Released at the end of this function.
    _physicsFreezeRequested = false;

    // One profile scope per panel, so the breakdown names the panel rather than
    // "the editor UI". A closed ImGui window still runs its Draw function — Begin()
    // returns false and it early-outs — so collapsed panels show up here too.
    {
        // Must be first: this resets ImGuizmo's per-frame state, draws the
        // manipulator under the panels below, and refreshes IsUsingGizmo() for
        // picking.
        ASSISI_PROFILE_SCOPE("panel/gizmo");
        DrawTransformGizmo();
    }

    // Blueprint mode hides the panels that act on *the level* — play control, the
    // network, level open/save, placement — because the level is not what is in
    // front of you. A Play button that runs a world containing one crate and an
    // editor sun is a control whose only possible use is a mistake.
    const bool blueprintMode = InBlueprintMode();

    { ASSISI_PROFILE_SCOPE("panel/options");      DrawOptionsWindow(); }
    if (!blueprintMode)
    {
        { ASSISI_PROFILE_SCOPE("panel/diagnostics");  DrawDiagnosticsWindow(); }
        { ASSISI_PROFILE_SCOPE("panel/chiara");       DrawChiaraWindow(); }
        { ASSISI_PROFILE_SCOPE("panel/game-control"); DrawGameControlWindow(); }
#if defined(ASSISI_NETWORKING)
        { ASSISI_PROFILE_SCOPE("panel/network");      DrawNetworkWindow(); }
#endif
    }
    { ASSISI_PROFILE_SCOPE("panel/entity-list");  DrawEntityListWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/history");      DrawHistoryWindow(); }
    if (!blueprintMode)
    {
        { ASSISI_PROFILE_SCOPE("panel/levels");     DrawLevelsWindow(); }
        { ASSISI_PROFILE_SCOPE("panel/blueprints"); DrawBlueprintsWindow(); }
    }
    { ASSISI_PROFILE_SCOPE("panel/blueprint-mode"); DrawBlueprintEditorWindow(); }
    { ASSISI_PROFILE_SCOPE("panel/inspector");    DrawInspector(); }
    if (!blueprintMode)
    {
        { ASSISI_PROFILE_SCOPE("panel/hello-image"); DrawHelloImageWindow(); }
    }
    { ASSISI_PROFILE_SCOPE("panel/asset-browser"); DrawAssetBrowser(); }
    { ASSISI_PROFILE_SCOPE("panel/stale-modal");  DrawStaleResolutionModal(); }
    { ASSISI_PROFILE_SCOPE("panel/save-confirm-modal"); DrawSaveConfirmModal(); }
#if defined(ASSISI_NETWORKING)
    if (!blueprintMode)
    {
        { ASSISI_PROFILE_SCOPE("panel/host-modal"); DrawHostUnsavedModal(); }
    }
#endif

    // **Release the Inspector's physics freeze here, unconditionally, not inside
    // the panel.** DrawInspector early-returns when nothing is selected, so
    // deselecting or destroying the entity on the frame the drag ends skips the
    // restore entirely and leaves the body Static for the rest of the session —
    // stuck in mid-air with a descriptor that still reads dynamic.
    if (!_physicsFreezeRequested)
        ThawEditedBody();

    // The same shape one gesture up. The gizmo and the Inspector both move an
    // instance's placement and neither can see whether the other still holds it;
    // the gizmo draws first, so closing the gesture there on "nobody is holding
    // *me*" cut an Inspector scrub into one transaction per frame. Both only raise
    // a hold now, and here, with every panel drawn, is where the drag is known to
    // be over. Its position relative to the sweep below is readability only — the
    // two gesture systems are independent.
    SweepInstanceGesture();

    // Commit finished drags and typing, drop no-ops, abandon dead-entity gestures.
    // After every panel has drawn, so each open gesture sees its final value, and
    // never mid-panel: the commit only reads the scene and stores JSON, but one
    // sweep point keeps the capture model simple. Playing captures nothing.
    if (Assisi::Editor::EditHistory *history = ActiveHistory())
        history->EndFrameSweep(_captureEditingActive);

    // Show unsaved changes in the OS window title, re-setting it only when the
    // dirty state flips rather than every frame. The base title is the game's own,
    // from game.json, not an editor hardcode.
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
    // Diagnostic for "ImGui stops responding until a new window opens". While any
    // widget holds ActiveId, ImGui suppresses hover on every other window — clicks
    // land nowhere, windows cannot be dragged, ImGuizmo goes dead — while the
    // engine's own input (fly camera, F11) keeps working. Opening a window appears
    // to fix it only because FocusWindow() clears a stuck ActiveId.
    //
    // A widget holding ActiveId for seconds with no mouse button down and no text
    // field being edited is wedged, not in use. Log enough internal state to name
    // the widget and whatever freed it.
    const ImGuiIO      &io = ImGui::GetIO();
    const ImGuiContext &g  = *ImGui::GetCurrentContext();

    const bool anyMouseDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
    // Class 1: a widget holds ActiveId with no button down and no text edit, so
    // hover is suppressed everywhere and every window plays dead.
    const bool wedgedActiveId = g.ActiveId != 0 && !anyMouseDown && !io.WantTextInput;
    // Class 2: ImGui has no valid mouse position although the editor is not
    // fly-looking (mouse capture) and the window has OS focus — the backend lost
    // the cursor, so ImGui ignores all mouse input. **The focus test must be the
    // GLFW attrib, a level, not io.AppFocusLost**, which is an edge ImGui clears
    // every frame; an unfocused window with the cursor elsewhere legitimately has
    // no mouse position, and alt-tabbing away once logged exactly that as a wedge.
    // While focused the glfw backend re-polls the cursor as a fallback every frame,
    // so this firing at all means something upstream is genuinely stuck.
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
