/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/BlueprintVerbs.hpp>

#include <Assisi/App/SystemCatalog.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <vector>

namespace Assisi::App
{

std::optional<ECS::InstanceId> SpawnBlueprint(World &world, std::string_view source, const ECS::Transform &placement)
{
    const std::expected<ECS::InstanceId, Runtime::LevelError> id =
        Runtime::SceneSerializer::ExpandInstance(world.scene, world.instances, source, placement);
    if (!id)
        return std::nullopt;

    const std::vector<ECS::Entity> members = Runtime::MembersOf(world.scene, *id);

    // The systems the blueprint names, queued for the next safe point. This is
    // what closes the hole "a component whose system was never installed just does
    // nothing" across a spawn: the behaviour a piece of content needs travels with
    // it, instead of depending on the level having happened to install it.
    if (const Runtime::BlueprintResult definition = Runtime::GetBlueprintDefinition(source))
        QueueSystemInstall(world, (*definition)->systems, source);

    // The prepared form holds asset *ids*, not loaded meshes, so a spawn has to
    // run the same resolve a level load runs — otherwise a spawned car arrives
    // with unresolved meshes (§11). A no-op with no services, which is the
    // headless case and correct: a server never resolves GPU assets.
    ResolveEntityAssets(world, members);

    // The members' bodies are built by the world's next reconcile. Propagated now,
    // for the reason App::BuildSceneBodies exists: a member parented to another is
    // placed from its parent's world matrix, and the matrix does not exist until
    // propagation has run over the entities that were just created.
    world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);

    return *id;
}

bool DestroyInstance(World &world, ECS::InstanceId instanceId)
{
    if (world.instances.Find(instanceId) == nullptr)
        return false;

    // Their bodies and characters go with them, on the world's next reconcile.
    for (const ECS::Entity member : Runtime::MembersOf(world.scene, instanceId))
    {
        world.scene.Destroy(member);
    }

    world.instances.Remove(instanceId);
    return true;
}

bool PruneFromInstance(World &world, ECS::Entity entity)
{
    return Runtime::PruneFromInstance(world.scene, entity);
}

bool ExplodeInstance(World &world, ECS::InstanceId instanceId)
{
    if (world.instances.Find(instanceId) == nullptr)
        return false;

    for (const ECS::Entity member : Runtime::MembersOf(world.scene, instanceId))
        (void)Runtime::PruneFromInstance(world.scene, member);

    world.instances.Remove(instanceId);
    return true;
}

ECS::Entity FindMember(World &world, ECS::InstanceId instanceId, std::string_view name)
{
    return Runtime::FindMember(world.scene, world.instances, instanceId, name);
}

const Runtime::BlueprintInstance *FindInstance(World &world, ECS::InstanceId instanceId,
                                               std::string_view expectedSource)
{
    return Runtime::FindInstance(world.instances, instanceId, expectedSource);
}

} // namespace Assisi::App
