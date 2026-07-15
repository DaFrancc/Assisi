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
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/SceneRegistry.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/GpuTelemetry.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Texture.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
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

    // --- ImGui panels ---
    void DrawOptionsWindow(); // frame graph + AA/VSync/FPS controls (F11); see SandboxOptions.cpp
    void DrawDiagnosticsWindow();
    void DrawLevelsWindow();
    void DrawInspector();
    void DrawHelloImageWindow(); // ImGui-texture-display smoke test
    void DrawAssetBrowser();

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

    /// @brief Runs the editor-only reconcile pass: scans the asset root,
    /// generating a `.aast` sidecar (with a minted GUID) for any asset that
    /// lacks one and rebuilding the GUID→path database. Auto-run once at
    /// startup and re-run by the asset browser's Reimport button. Refs are
    /// still path-based this stage, so nothing rebinds — the database is built
    /// but not yet the resolution key (asset-database S1).
    void ReimportAssets();

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
