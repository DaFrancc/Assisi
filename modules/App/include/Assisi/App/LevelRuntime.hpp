/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LevelRuntime.hpp
/// @brief Turns serialized level data into a working scene: asset resolution +
///        physics body rebuild, composed over the modules that own each half.
///
/// SceneSerializer deserializes component *data*; Runtime::AssetResolve turns
/// mesh/material GUIDs into GPU pointers; PhysicsWorld::RebuildSceneBodies
/// turns RigidBodyDescriptors into Jolt bodies. This header is the layer that
/// composes them — it lives in App because App is the only module that links
/// both Runtime and Physics (Runtime deliberately does not link Physics).
///
/// Interim caveat (until asset-database S5, the cooker/PakProvider): a game
/// still builds its GUID→path index by scanning sidecars at startup via
/// AssetDatabase::Rebuild(). Rebuild *mints and writes* a sidecar for any
/// asset that lacks one — write-free only when every asset already has its
/// `.aast`. S5 replaces the scan with a baked index; only the material
/// reconcile (ReconcileMeshMaterials) is genuinely editor-only.

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace Assisi::Runtime
{
// Declared, not included: the definition lives in SceneSerializer.hpp, which
// drags in nlohmann/json — and this header is included (via World.hpp) almost
// everywhere. Callers that build a header include that themselves.
struct LevelHeader;
class InstanceTable;
} // namespace Assisi::Runtime

namespace Assisi::App
{

/// @brief Wires @p database into the two places that translate asset ids:
/// serialization's save-time path hint, and the cache's id↔path resolution
/// (mesh/material/texture loads and glTF import speak GUIDs; reserved
/// built-ins resolve without the database).
///
/// @p database is captured by reference and must outlive the installation
/// (it is engine-lifetime state in practice). Rebuild() mutates the same
/// object, so a re-scan needs no re-install — but calling this again with the
/// same database is harmless.
void InstallAssetResolvers(Render::AssetCache &cache, const Core::AssetDatabase &database);

/// @brief Rebuilds every transient from the scene's durable components, after
/// the scene's entities were replaced wholesale (level load or a play-session
/// restore): re-resolves each MeshRenderer's GPU pointers and rebuilds the
/// physics world from the RigidBodyDescriptors.
void RebindSceneAssetsAndPhysics(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                                 Physics::PhysicsWorld &physics);

/// @brief What a load should do with the asset cache before resolving.
enum class AssetCacheReset : std::uint8_t
{
    /// Free every cached GPU asset first (and evict the renderer's binding sets)
    /// so the new level starts from a clean cache. Correct only when no OTHER
    /// world is resident — the clear frees resources their resolved pointers
    /// still reference.
    ClearFirst,

    /// Leave the cache alone: the new level's assets are added to whatever is
    /// already there, and assets shared with a resident level are reused rather
    /// than re-uploaded. Required for any load that happens while another world
    /// is alive (docs/multi-scene-design-notes.md §0 — the Clear moves out of
    /// the load path and becomes a post-travel sweep).
    Keep,
};

/// @brief Loads a level by virtual path (e.g. "levels/Materials.alvl") into
/// @p scene and makes it runnable: deserialize, optionally drop the old asset set
/// and evict the renderer's cached bindings, then rebind assets + physics.
/// Returns false (scene untouched) if the file didn't resolve or deserialize.
///
/// ## Call this only at a safe point — never mid-frame.
///
/// Clearing the cache frees GPU assets **including the bindless descriptor
/// table** that this frame's already-recorded draws still reference; calling
/// this between BeginFrame and EndFrame faults the GPU. Call it before the
/// frame's draws are recorded — e.g. from OnUpdate, or marshalled to the
/// main-thread drain via Jobs().RunOnMain (which runs just before OnUpdate).
/// A load requested from UI code must be deferred, not applied in place.
///
/// Editor-state bookkeeping (undo-history wipe, selection/eyedropper reset,
/// play-state reset) is deliberately not here — it belongs to the caller that
/// has that state.
///
/// @p header (optional) receives the level's non-entity metadata — notably the
/// system profile it asks for, which the caller applies to the world it loaded
/// into (docs/world-system-binding-design-notes.md §3).
///
/// @p instances (optional) receives one row per blueprint instance the level
/// places. A level that places any and is given none fails to load rather than
/// arriving without most of its content.
bool LoadLevel(ECS::Scene &scene, std::string_view virtualPath, Render::AssetCache &cache,
               const Core::AssetDatabase &database, Physics::PhysicsWorld &physics,
               Runtime::SceneRenderer &sceneRenderer, AssetCacheReset reset = AssetCacheReset::ClearFirst,
               Runtime::LevelHeader *header = nullptr, Runtime::InstanceTable *instances = nullptr);

/// @brief LoadLevel from an absolute filesystem path instead of a virtual one.
///
/// Same safe-point rules, same everything — the only difference is where the
/// bytes come from. It exists for levels that are not assets: the temp snapshot
/// a play-in-editor host writes so its client processes can load the scene it is
/// actually simulating, unsaved edits included. Asset *references inside* the
/// level still resolve through the asset system as usual; it is only the level
/// file itself that lives outside it.
bool LoadLevelFile(ECS::Scene &scene, const std::filesystem::path &path, Render::AssetCache &cache,
                   const Core::AssetDatabase &database, Physics::PhysicsWorld &physics,
                   Runtime::SceneRenderer &sceneRenderer, AssetCacheReset reset = AssetCacheReset::ClearFirst,
                   Runtime::LevelHeader *header = nullptr, Runtime::InstanceTable *instances = nullptr);

/// @brief The simulation half of LoadLevel: deserialize the level and rebuild
/// its physics bodies, with no asset cache and no renderer involved.
///
/// This is what a dedicated server loads. It takes no Render or Runtime types
/// at all, which is the point — a headless process has no GPU assets to resolve
/// and no binding caches to evict, and asking it for an AssetCache just to
/// throw one away would be a lie about what it needs. Mesh/material GUIDs stay
/// in the scene as authored data (the server replicates them; it never resolves
/// them), so a client joining later gets the same references the level declared.
///
/// The safe-point warning on LoadLevel does not apply here: nothing GPU-owned
/// is freed. Returns false (scene untouched) if the level didn't resolve or
/// deserialize.
///
/// @note This header still *includes* the Render/Runtime headers for LoadLevel
/// above, so including it does not yet give a caller a render-free dependency
/// footprint — only a render-free call. Untangling the header is part of the
/// App core/presentation split committed to in the networking design notes.
bool LoadLevelSim(ECS::Scene &scene, std::string_view virtualPath, Physics::PhysicsWorld &physics,
                  Runtime::InstanceTable *instances = nullptr);

/// @brief Per-frame streaming upgrade: while the cache has async loads in
/// flight (and for one frame after the last finishes, to pick up the final
/// result), re-resolves every MeshRenderer so its transient pointers upgrade
/// in place — a null meshBuffer (billboard placeholder) becomes the mesh, the
/// fallback material becomes the real one.
///
/// @p wereLoading is the caller's persistent flag (start it false); the
/// function reads and updates it to implement the one-frame tail. Call once
/// per update tick.
void UpgradeStreamingAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                            bool &wereLoading);

} // namespace Assisi::App
