/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetResolve.hpp
/// @brief Resolves MeshRenderer asset references (durable GUIDs) into the
///        transient GPU pointers the draw path reads.
///
/// SceneSerializer only deserializes component *data*; this is the step that
/// makes a loaded scene drawable. It runs after a level load or scene restore,
/// after an inspector/browser edit to a single entity, and per frame while
/// async loads are still streaming in (so placeholders upgrade in place).

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Runtime
{

/// @brief Resolves one MeshRenderer's transient GPU pointers (mesh and one
/// Material per mesh slot) from its durable GUIDs.
///
/// Each slot uses the entity's override when it has a non-nil entry, otherwise
/// the mesh's default for that slot — the material GUID the glTF manifest
/// recorded at import, read from @p database rather than re-derived live. A
/// primitive mesh has no slot table, so `materials` stays empty and the draw
/// path uses the cache's fallback. Safe to call repeatedly: while the cache is
/// still streaming, a null meshBuffer (billboard placeholder) or fallback
/// material upgrades in place on a later call (see ResolveSceneAssets).
void ResolveMeshRendererAssets(MeshRenderer &meshRenderer, Render::AssetCache &cache,
                               const Core::AssetDatabase &database);

/// @brief ResolveMeshRendererAssets over every MeshRenderer in @p scene.
///
/// The whole-scene pass used after the scene's entities were replaced
/// wholesale (level load, play-session restore) and by the streaming upgrade
/// loop while loads are in flight.
void ResolveSceneAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database);

} // namespace Assisi::Runtime
