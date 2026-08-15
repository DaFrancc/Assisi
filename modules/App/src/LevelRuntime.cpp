/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/LevelRuntime.hpp>

#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetIdJson.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace Assisi::App
{

void InstallAssetResolvers(Render::AssetCache &cache, const Core::AssetDatabase &database)
{
    // Serialization's path hint (asset-database D2): saved GUID references carry
    // a readable last-known path regenerated from the database.
    Core::SetAssetIdHintResolver([&database](const Core::AssetId &id)
                                 { return database.PathFor(id).value_or(std::string{}); });

    // The cache: id↔path so mesh/material/texture resolution and glTF import
    // speak GUIDs. Reserved built-ins resolve without the database.
    cache.SetAssetResolvers(
        [&database](const Core::AssetId &id) -> Core::AssetPath
        {
            const std::optional<std::string> path = database.PathFor(id);
            return path ? Core::AssetPath{std::string_view{*path}} : Core::AssetPath{};
        },
        [&database](std::string_view virtualPath) -> Core::AssetId
        { return database.IdFor(virtualPath).value_or(Core::AssetId{}); });
}

void RebindSceneAssetsAndPhysics(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                                 Physics::PhysicsWorld &physics)
{
    Runtime::ResolveSceneAssets(scene, cache, database);
    (void)BuildSceneBodies(scene, physics);
}

namespace
{

/// The half of a load that is the same whichever way the bytes arrived.
void FinishLoad(World &world, const LevelServices &services, AssetCacheReset reset)
{
    if (reset == AssetCacheReset::ClearFirst)
    {
        // New asset set, and nothing else resident: drop the old cache and evict
        // the mesh pass's binding sets (they key on raw texture pointers we're
        // about to free) before re-resolving. With another world alive this would
        // dangle its resolved pointers, which is why it is a caller's choice.
        services.cache.Clear();
        services.renderer.InvalidateAssetBindings();
    }

    RebindSceneAssetsAndPhysics(world.scene, services.cache, services.database, world.physics);
}

} // namespace

Runtime::LevelResult LoadLevel(World &world, std::string_view virtualPath, const LevelServices &services,
                               const LevelLoadOptions &options)
{
    // Before the load, not after: the load *fills* the cache, and the member list
    // it caches is what resolves a BlueprintMember's index back to a name for the
    // rest of the level's life. ClearFirst means "the old asset set is gone",
    // which is exactly when the old level's blueprints stop being wanted.
    if (options.reset == AssetCacheReset::ClearFirst)
        Runtime::ClearBlueprintCache();

    // Passed straight out rather than flattened to a bool: the deserializer knows
    // exactly what was wrong with the file and this is the only thing between it
    // and the caller. A bool discards that reason and leaves every caller logging
    // "failed to load".
    const Runtime::LevelResult loaded = Runtime::SceneSerializer::LoadFromFile(
        world.scene, virtualPath, {.header = options.header, .instances = &world.instances});
    if (!loaded)
        return loaded;

    FinishLoad(world, services, options.reset);
    return {};
}

Runtime::LevelResult LoadLevelFile(World &world, const std::filesystem::path &path,
                                   const LevelServices &services, const LevelLoadOptions &options)
{
    if (options.reset == AssetCacheReset::ClearFirst)
        Runtime::ClearBlueprintCache();

    const Runtime::LevelResult loaded = Runtime::SceneSerializer::LoadFromDisk(
        world.scene, path, {.header = options.header, .instances = &world.instances});
    if (!loaded)
        return loaded;

    FinishLoad(world, services, options.reset);
    return {};
}

Runtime::LevelResult LoadLevelSim(World &world, std::string_view virtualPath)
{
    const Runtime::LevelResult loaded =
        Runtime::SceneSerializer::LoadFromFile(world.scene, virtualPath, {.instances = &world.instances});
    if (!loaded)
        return loaded;

    (void)BuildSceneBodies(world.scene, world.physics);
    return {};
}

void UpgradeStreamingAssets(ECS::Scene &scene, Render::AssetCache &cache, const Core::AssetDatabase &database,
                            bool &wereLoading)
{
    // Re-resolve while loads are pending, and for one frame after the last one
    // finishes so the final result is picked up.
    const bool loadsPending = cache.HasPendingLoads();
    if (loadsPending || wereLoading)
    {
        Runtime::ResolveSceneAssets(scene, cache, database);
        wereLoading = loadsPending;
    }
}

std::optional<std::uint64_t> HashLevelFile(std::string_view virtualPath)
{
    const std::expected<std::filesystem::path, Core::AssetError> resolved = Core::AssetSystem::Resolve(virtualPath);
    if (!resolved)
        return std::nullopt;

    return Core::HashTextFileNormalized(*resolved);
}

std::string JoinLevelErrorMessage(JoinLevelError error, std::string_view path)
{
    switch (error)
    {
    case JoinLevelError::NoLevel:
        return "the host is not running a level file, so there is no world to build here.";
    case JoinLevelError::Unresolvable:
        return "this build has no '" + std::string{path} + "'; get the level from the host and retry.";
    case JoinLevelError::Unreadable:
        return "cannot read '" + std::string{path} + "'.";
    case JoinLevelError::ContentMismatch:
        return "your copy of '" + std::string{path} + "' differs from the host's; sync the file from the host and "
               "retry.";
    case JoinLevelError::SystemsMissing:
        return "'" + std::string{path} + "' names a system this build does not declare.";
    }
    return "the host's level cannot be built here.";
}

std::expected<std::filesystem::path, JoinLevelError> ResolveJoinLevel(const NetSync::LevelIdentity &level)
{
    std::filesystem::path file;
    switch (level.addressing)
    {
    case NetSync::LevelAddressing::None:
        return std::unexpected(JoinLevelError::NoLevel);
    case NetSync::LevelAddressing::Virtual:
    {
        const std::expected<std::filesystem::path, Core::AssetError> resolved =
            Core::AssetSystem::Resolve(level.path);
        if (!resolved)
            return std::unexpected(JoinLevelError::Unresolvable);
        file = *resolved;
        break;
    }
    case NetSync::LevelAddressing::AbsolutePath:
        // A play-in-editor snapshot, written by a host on this machine. Nothing
        // about reading it needs a renderer, so no target has to refuse it.
        file = level.path;
        break;
    }

    // Resolving says where the file *would* be, not that it is there, so absence
    // is checked here. Kept apart from Unreadable because the two ask different
    // things of whoever is joining: fetch the level, versus fix your disk.
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec)
        return std::unexpected(JoinLevelError::Unresolvable);

    const std::optional<std::uint64_t> localHash = Core::HashTextFileNormalized(file);
    if (!localHash)
        return std::unexpected(JoinLevelError::Unreadable);

    if (*localHash != level.contentHash)
    {
        // The two numbers only answer "are they different", which the caller's
        // message already says — so they go to the log and the message stays
        // something the player can act on.
        Core::Log::Error("Join: level content hash mismatch for '{}' — host {}, local {}.", level.path,
                         Core::ToHex64(level.contentHash), Core::ToHex64(*localHash));
        return std::unexpected(JoinLevelError::ContentMismatch);
    }

    // The hash proves the file is byte-identical to the host's, which says nothing
    // about whether *this* build declares the systems it names: an older target can
    // match the file exactly and still be unable to run it.
    if (!LevelSystemsAreDeclared(level.path))
        return std::unexpected(JoinLevelError::SystemsMissing);

    return file;
}

StrippedEntities StripReplicatedEntities(ECS::Scene &scene, Physics::PhysicsWorld &physics)
{
    StrippedEntities stripped;

    std::vector<ECS::Entity> doomed;
    scene.ForEachEntity(
        [&](ECS::Entity entity)
        {
            if (scene.Has<NetSync::Replicated>(entity))
                doomed.push_back(entity);
        });

    for (const ECS::Entity entity : doomed)
    {
        // Out of the physics world first: Destroy only ends the entity, and a body
        // left behind is one nothing holds a handle to any more.
        if (const auto *body = scene.Get<Physics::RigidBody>(entity))
        {
            physics.RemoveBody(*body);
            scene.Remove<Physics::RigidBody>(entity);
        }
        scene.Destroy(entity);
    }
    scene.FlushDestroyed();

    // A child of a stripped entity now holds a dead parent handle. Left alone,
    // transform propagation reads it as a root and places it at its *local* pose —
    // a decoration adrift from what it decorated, in a world that otherwise looks
    // right. Dropping the link says the same thing honestly.
    std::vector<ECS::Entity> orphans;
    scene.ForEachEntity(
        [&](ECS::Entity entity)
        {
            const auto *parent = scene.Get<Runtime::Parent>(entity);
            if (parent != nullptr && !scene.IsAlive(parent->parent))
                orphans.push_back(entity);
        });
    for (const ECS::Entity entity : orphans)
        scene.Remove<Runtime::Parent>(entity);

    stripped.entities = doomed.size();
    stripped.orphans  = orphans.size();
    return stripped;
}

} // namespace Assisi::App
