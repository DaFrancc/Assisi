/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SandboxApp.hpp
/// @brief Assisi Sandbox — level viewer built on the Application layer.
///
/// The implementation is split across several translation units by concern:
///   - SandboxApp.cpp        lifecycle/setup + diagnostics + OnImGui dispatch
///   - SandboxCamera.cpp     fly camera, entity picking, eyedropper
///   - SandboxInspector.cpp  reflected component field editing
///   - SandboxAssetBrowser.cpp  asset browser + thumbnails
///   - SandboxLevels.cpp     level scan/save/load + the Levels window

#include <Assisi/App/Application.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Window/ActionMap.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Geometry/AssetImport.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/GpuTelemetry.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Texture.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// @brief Emitted when the user clicks to select (or deselect) an entity.
/// NullEntity means the click landed on empty space.
AEVENT()
struct EntitySelectionChangedEvent
{
    Assisi::ECS::Entity entity;
};

// ---------------------------------------------------------------------------
// SandboxApp
// ---------------------------------------------------------------------------

class SandboxApp : public Assisi::App::Application
{
  public:
    void OnStart() override;
    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void OnRender(Assisi::Render::RenderFrame &frame) override;
    void OnImGui() override;
    void OnResize(int width, int height) override;
    void OnRenderTargetsChanged(const nvrhi::FramebufferInfo &framebufferInfo) override;
    void FlushDeferred() override;

  private:
    // --- Setup ---
    void SetupCamera();
    void SetupScene();

    // --- Per-frame helpers ---
    void HandleEntityPicking();
    void UpdateCamera(float dt);
    /// @brief Recomputes _cameraTransform.worldMatrix from its TRS. The camera is
    /// parentless, so world == local; call before reading the view matrix.
    void RefreshCameraMatrix();
    /// @brief Reseeds _yaw/_pitch from the camera's current rotation so the fly
    /// controller resumes from the new orientation without snapping — called when
    /// a focus animation ends (or is cancelled by manual look input).
    void SyncYawPitchFromRotation();

    // --- ImGui panels ---
    void DrawOptionsWindow(); // frame graph + AA/VSync/FPS controls (F11); see SandboxOptions.cpp
    void DrawDiagnosticsWindow();
    void DrawLevelsWindow();
    void DrawInspector();
    void DrawHelloImageWindow(); // ImGui-texture-display smoke test
    void DrawAssetBrowser();
    void DrawGameControlWindow(); // Run/Pause/Stop the simulation (F5/F6/F7); see SandboxPlay.cpp
    void DrawEntityListWindow();  // scene entity list: click selects, double-click focuses; see SandboxPlay.cpp

    // --- Asset browser helpers ---
    /// @brief Arms the browser to write into @p meta's field at @p fieldOffset on
    /// the selected entity, and opens the window.
    void OpenAssetBrowserFor(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset);
    /// @brief Arms the browser to write into element @p slot of an AssetPathVector
    /// field (a MeshRenderer material slot), listing only materials, and opens it.
    void OpenAssetBrowserForSlot(const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset,
                                 int32_t slot);
    /// @brief Writes @p vpath into the pinned browser target field and closes.
    void SelectAsset(std::string_view vpath);
    /// @brief Re-resolves a MeshRenderer entity's mesh/texture from its current
    /// paths, so an inspector/browser edit takes effect without a level reload.
    void ReresolveEntityAssets(Assisi::ECS::Entity entity);
    /// @brief Resolves one MeshRenderer's transient GPU pointers (mesh and one
    /// Material per mesh slot) from its durable paths. Shared by level load and
    /// single-entity re-resolve.
    void ResolveMeshRendererAssets(Assisi::Runtime::MeshRenderer &mrc);
    /// @brief Reads _assetBrowserDir into the cached dirs/images lists. Called
    /// only when the listing may have changed, not every frame.
    void RescanAssetBrowser();

    // --- Inspector helpers ---
    bool EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta);
    /// @brief Draws one editable row per material slot of @p mrc's resolved mesh
    /// (labelled by the imported material name), each a `.amat` path + browse
    /// button writing into `materialOverrides[slot]`. @p fieldOffset is the offset
    /// of the materialOverrides vector within the MeshRenderer. Returns true if a
    /// row was edited (the caller re-resolves).
    bool EditMaterialSlots(Assisi::Runtime::MeshRenderer &mrc,
                           const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset);
    /// @brief Draws the path input for an AssetId field (@p inputId is the ImGui
    /// id): display = the id's resolved virtual path, typing a path re-resolves
    /// the id via the database. Returns true and writes @p id when edited. The
    /// caller lays out the browse button + label to the right.
    bool AssetIdPathField(const char *inputId, Assisi::Core::AssetId &id);
    void HandlePhysicsEditing(bool anyFieldEdited);

    /// @brief Writes an eyedropper-picked entity into the armed EntityRef field.
    void ApplyEyedropperPick(Assisi::ECS::Entity picked);

    // --- Level management ---
    void ScanLevels();
    void LoadLevel(const std::string &name);
    void SaveLevel(const std::string &name);

    // --- Play control (F5 run / F6 pause / F7 stop) ---
    /// @brief Enters play from the editing state: snapshots the scene so Stop can
    /// restore it, then begins simulating. No-op unless currently Editing.
    void StartPlay();
    /// @brief Resumes a paused simulation in place. No-op unless currently Paused.
    void ResumePlay();
    /// @brief Freezes simulation where it stands — physics, and any game-logic
    /// systems, stop ticking; nothing resets. No-op unless currently Playing.
    void PausePlay();
    /// @brief Stops simulating and restores the scene to the pre-play snapshot,
    /// discarding anything play mode changed. No-op when already Editing.
    void StopPlay();
    /// @brief True only while the world is actively simulating (physics + any game
    /// systems tick). False in both Editing and Paused; the editor camera/picking
    /// stay live regardless, so the scene is always navigable.
    [[nodiscard]] bool IsSimulating() const { return _playState == PlayState::Playing; }

    /// @brief Re-resolves every MeshRenderer's transient GPU pointers and rebuilds
    /// the physics bodies from their descriptors. Shared by the snapshot restore
    /// (StopPlay) and level load — both replace the scene's entities wholesale, so
    /// the transient state has to be rebuilt from the durable components.
    void RebindSceneAssetsAndPhysics();
    /// @brief Creates a Jolt body for @p entity from @p desc at @p tc's pose and
    /// attaches a RigidBody. Shared by the scene rebuild and live component-add.
    void AddPhysicsBody(Assisi::ECS::Entity entity, const Assisi::ECS::Transform &tc,
                        const Assisi::Physics::RigidBodyDescriptor &desc);
    /// @brief Creates a fresh entity with a Transform a few units in front of the
    /// editor camera (an empty object to build up via Add Component), selects it,
    /// and returns it. Used by the entity list's + button.
    Assisi::ECS::Entity CreateEntity();
    /// @brief Adds @p meta's component to the selected entity with default field
    /// values, wiring up any runtime state the component needs (mesh re-resolve,
    /// physics body). No-op if the entity already has it. Used by the inspector's
    /// Add Component field.
    void AddComponentToSelected(const Assisi::Core::Reflect::ComponentMeta &meta);
    /// @brief Starts the 0.5 s eased camera move that reframes @p entity, choosing
    /// a framing distance from its bounds. Used by an entity-list double-click.
    void FocusCameraOn(Assisi::ECS::Entity entity);

    /// @brief Runs the editor-only reconcile pass: scans the asset root,
    /// generating a `.aast` sidecar (with a minted GUID) for any asset that
    /// lacks one and rebuilding the GUID→path database. Auto-run once at
    /// startup and re-run by the asset browser's Reimport button. Refs are
    /// still path-based this stage, so nothing rebinds — the database is built
    /// but not yet the resolution key (asset-database S1).
    void ReimportAssets();

    /// @brief Brings every indexed glTF's materials up to date (asset-database
    /// S3/S4). A glTF with no manifest is exploded into `.amat` children; one
    /// that already has a manifest is reconciled against its current source —
    /// auto-refreshing provably-safe changes (geometry-only, additive slots) and
    /// badging anything ambiguous stale (see _staleMeshes) without clobbering it.
    /// Editor-only; runs inside ReimportAssets between the two database scans.
    /// Returns whether any file was written, so the caller only rescans then.
    bool ReconcileMeshMaterials();

    /// @brief Whether a mesh asset (by virtual path) was left stale by the last
    /// reconcile — its source changed in a way that could not be auto-resolved.
    /// Drives the asset-browser stale badge.
    [[nodiscard]] bool IsAssetStale(std::string_view vpath) const;

    // --- Stale-material resolution prompt (asset-database S4 / D5) ---
    /// @brief Arms the resolution modal for a stale glTF: computes its per-slot
    /// diff and requests the popup open next frame. Reached by clicking a stale
    /// mesh tile, or auto-opened for a stale mesh live in the open scene.
    void OpenStaleResolution(const std::string &vpath);
    /// @brief Draws the modal that shows a stale mesh's material conflicts and the
    /// author's choices (regenerate from source / keep mine / later). Called from
    /// OnImGui once per frame.
    void DrawStaleResolutionModal();
    /// @brief Applies the author's choice to the current _staleResolveTarget:
    /// @p regenerate true overwrites the materials from source, false keeps them
    /// and just accepts the new source hash. Clears the stale flag and, on
    /// regenerate, rebuilds the database and re-resolves live entities.
    void ApplyStaleResolution(bool regenerate);
    /// @brief Advances the liveness queue: opens the next still-stale mesh, or
    /// clears the modal target when the queue is empty. Called after each choice.
    void AdvanceStaleQueue();

    Assisi::ECS::Entity PickEntity(glm::vec2 mousePos);

    // --- Systems ---
    Assisi::App::SystemRegistry _systems;
    Assisi::Window::ActionMap   _actions;

    // --- State ---
    Assisi::ECS::SceneRegistry         _scenes;
    Assisi::ECS::Scene                *_scene = nullptr;
    Assisi::Physics::PhysicsWorld      _physics;

    // --- Rendering ---
    // The engine's default scene-render path owns lighting + the mesh pipeline;
    // OnRender is a single Render() call.
    Assisi::Runtime::SceneRenderer _sceneRenderer;

    // Resolves each entity's mesh/materialOverrides ids to shared GPU resources;
    // owns every mesh, texture, and material the scene draws (deduped by path).
    Assisi::Render::AssetCache _assetCache;

    // Editor-only GUID identity index (asset-database S1). ReimportAssets()
    // populates it by scanning the asset root and generating `.aast` sidecars.
    // Built but not yet the resolution key — references still resolve by path.
    Assisi::Core::AssetDatabase _assetDatabase;

    // Mesh assets (by virtual path) the last reconcile left stale: their glTF
    // source changed in a way the conservative classifier couldn't auto-resolve
    // (S4/D5). Surfaced as a badge in the asset browser. Rebuilt each reconcile.
    std::unordered_set<std::string> _staleMeshes;

    // Stale-material resolution modal (S4 second half / D5 prompt). The target is
    // the glTF being resolved ("" = closed); the diff is its per-slot conflict
    // detail, computed once on open. _staleResolveRequestOpen latches an
    // ImGui::OpenPopup on the next frame. The queue holds still-stale meshes that
    // are live in the open scene, prompted one after another (D5 liveness).
    std::string                     _staleResolveTarget;
    Assisi::Geometry::MaterialDiff  _staleResolveDiff;
    bool                            _staleResolveRequestOpen = false;
    std::vector<std::string>        _staleResolveQueue;

    // Smoke test for ImGui texture display — loaded once in SetupScene. Owns its
    // texture (not routed through _assetCache, which LoadLevel Clears).
    Assisi::Render::Texture _helloTexture;

    // The editor fly-camera is not level data, so it is plain state here rather
    // than an entity in a whole ECS scene of its own (which existed only so
    // LoadLevel's clear-and-load wouldn't wipe it). As plain members the camera
    // pose also survives level loads for free. RefreshCameraMatrix() recomputes
    // worldMatrix from the TRS (parentless, so world == local) before it is read.
    Assisi::Runtime::Transform _cameraTransform;
    Assisi::Runtime::Camera    _camera{60.f, 0.1f, 200.f, true};

    // Set by SetupCamera() before first use; these are just safe defaults.
    float _yaw   = 0.f;
    float _pitch = 0.f;

    static constexpr float kMoveSpeed        = 8.f;
    static constexpr float kMouseSensitivity = 0.1f;

    Assisi::ECS::Entity _selectedEntity = Assisi::ECS::NullEntity;
    bool                _wasDragging    = false;

    // Options overlay (frame graph + display/pacing settings), toggled with F11.
    // Owned by the app, not the engine — see DrawOptionsWindow in SandboxOptions.cpp.
    bool _showOptions = false;

    // --- Play control (game-control window, F5/F6/F7; see SandboxPlay.cpp) ---
    // Physics and any game-logic systems tick only while Playing; the editor
    // camera and picking stay live in every state so the scene is always
    // navigable. Run snapshots the scene so Stop can restore it, Pause freezes
    // in place, Stop restores the snapshot and returns to Editing.
    enum class PlayState : std::uint8_t
    {
        Editing,
        Playing,
        Paused
    };
    PlayState   _playState = PlayState::Editing;
    std::string _playSnapshot; ///< Scene JSON captured at Run; restored on Stop.

    // Camera focus animation (entity-list double-click). A fixed-duration eased
    // move that reframes the camera on an object; while active it owns the camera
    // transform (UpdateCamera advances it and skips fly control). Manual look
    // input cancels it. Always kCameraFocusDuration regardless of travel distance.
    bool                   _cameraFocusActive  = false;
    float                  _cameraFocusElapsed = 0.f;
    glm::vec3              _cameraFocusStartPos{0.f};
    glm::vec3              _cameraFocusEndPos{0.f};
    glm::quat              _cameraFocusStartRot{1.f, 0.f, 0.f, 0.f};
    glm::quat              _cameraFocusEndRot{1.f, 0.f, 0.f, 0.f};
    static constexpr float kCameraFocusDuration = 0.25f;

    // Inspector "Add Component" search field: the in-progress substring the user
    // is typing; matched case-insensitively against addable component names.
    char _addComponentBuf[64] = {};

    // NVIDIA GPU telemetry (clocks/power/util/temp) for the options overlay.
    // Lazily initialises NVML on first poll, so it costs nothing until the
    // overlay is opened; reports an invalid sample on non-NVIDIA systems.
    Assisi::Render::GpuTelemetry _gpuTelemetry;

    // Ring-buffer history for the telemetry graphs, advanced once per fresh NVML
    // sample (~5Hz, gated on GpuTelemetrySample::sequence) rather than per frame,
    // so the buffers span ~30s regardless of frame rate. _gpuTelemetryOffset is
    // the next write slot / chronological start (ImPlot Offset), _gpuTelemetryCount
    // saturates at the capacity. Only advance while the overlay is open.
    static constexpr int                        kGpuHistory = 150; // ~30s at 5Hz
    std::array<float, kGpuHistory>              _gpuClockHistory{};
    std::array<float, kGpuHistory>              _gpuUtilHistory{};
    std::array<float, kGpuHistory>              _gpuPowerHistory{};
    int                                         _gpuTelemetryOffset = 0;
    int                                         _gpuTelemetryCount  = 0;
    unsigned long long                          _lastGpuSequence    = 0;

    // Eyedropper: while armed, the next scene entity-pick is written into the
    // captured EntityRef field instead of changing the selection. The target is
    // pinned by (entity, component meta, field offset) rather than a raw pointer,
    // so a pool reallocation between arming and picking can't dangle it.
    bool                                        _eyedropperArmed       = false;
    Assisi::ECS::Entity                         _eyedropperEntity      = Assisi::ECS::NullEntity;
    const Assisi::Core::Reflect::ComponentMeta *_eyedropperMeta        = nullptr;
    std::size_t                                 _eyedropperFieldOffset = 0;

    // Asset browser: opened from an AssetPath field's browse button, it navigates
    // the asset directory and writes the picked path back into the field. The
    // target is pinned by (entity, component meta, field offset) and re-resolved
    // at write time — same anti-dangling scheme as the eyedropper above.
    bool                                        _assetBrowserOpen        = false;
    Assisi::ECS::Entity                         _assetBrowserEntity      = Assisi::ECS::NullEntity;
    const Assisi::Core::Reflect::ComponentMeta *_assetBrowserMeta        = nullptr;
    std::size_t                                 _assetBrowserFieldOffset = 0;
    /// @brief -1 when the target field is a plain AssetPath; >= 0 when it is
    /// element `[slot]` of an AssetPathVector (a MeshRenderer material slot). In
    /// the latter mode the browser lists only materials (and folders).
    int32_t                                     _assetBrowserVectorSlot  = -1;
    std::string                                 _assetBrowserDir; ///< Current dir, relative to the asset root ("" = root).

    // Cached listing of _assetBrowserDir — re-read only on navigation / open /
    // Refresh (see _assetBrowserDirty), never per frame.
    std::vector<std::string> _assetBrowserDirs;
    std::vector<std::string> _assetBrowserImages;
    std::vector<std::string> _assetBrowserMeshes;    ///< .glb/.gltf files (no thumbnail; shown as cube tiles).
    std::vector<std::string> _assetBrowserMaterials; ///< .amat files (shown as material-sphere tiles).
    bool                     _assetBrowserDirty     = true;
    bool                     _assetBrowserReadError = false;
    float                    _assetBrowserThumbSize = 256.f; ///< Tile size in px; adjustable via the zoom buttons.

    // Textures loaded to thumbnail the browser's image entries. Separate from
    // _assetCache so a level load (which Clears that) doesn't drop thumbnails.
    Assisi::Render::AssetCache _thumbnailCache;

    std::vector<std::string> _levelFiles;
    int                      _selectedLevel = 0;
    char                     _saveAsName[128] = {};
};
