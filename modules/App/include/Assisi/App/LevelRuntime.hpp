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

#include <string_view>

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

/// @brief Loads a level by virtual path (e.g. "levels/Materials.alvl") into
/// @p scene and makes it runnable: deserialize, drop the old asset set, evict
/// the renderer's cached bindings, then rebind assets + physics. Returns false
/// (scene untouched) if the file didn't resolve or deserialize.
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
bool LoadLevel(ECS::Scene &scene, std::string_view virtualPath, Render::AssetCache &cache,
               const Core::AssetDatabase &database, Physics::PhysicsWorld &physics,
               Runtime::SceneRenderer &sceneRenderer);

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
