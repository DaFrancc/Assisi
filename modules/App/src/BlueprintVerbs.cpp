/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/BlueprintVerbs.hpp>

#include <Assisi/App/SystemCatalog.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <vector>

namespace Assisi::App
{

std::optional<ECS::InstanceId> SpawnBlueprint(World &world, std::string_view source,
                                              const ECS::Transform &placement)
{
    const std::optional<ECS::InstanceId> id =
        Runtime::SceneSerializer::ExpandInstance(world.scene, world.instances, source, placement);
    if (!id)
        return std::nullopt;

    const std::vector<ECS::Entity> members = Runtime::MembersOf(world.scene, *id);

    // The systems the blueprint names, queued for the next safe point. This is
    // what closes the hole "a component whose system was never installed just does
    // nothing" across a spawn: the behaviour a piece of content needs travels with
    // it, instead of depending on the level having happened to install it.
    if (const std::shared_ptr<const Runtime::BlueprintDefinition> definition =
            Runtime::GetBlueprintDefinition(source))
        QueueSystemInstall(world, definition->systems, source);

    // The prepared form holds asset *ids*, not loaded meshes, so a spawn has to
    // run the same resolve a level load runs — otherwise a spawned car arrives
    // with unresolved meshes (§11). Skipped when there are no services, which is
    // the headless case and correct: a server never resolves GPU assets.
    if (world.manager != nullptr)
    {
        const WorldManager::Services &services = world.manager->GetServices();
        if (services.cache != nullptr && services.database != nullptr)
        {
            for (const ECS::Entity member : members)
            {
                if (Runtime::MeshRenderer *mesh = world.scene.Get<Runtime::MeshRenderer>(member))
                    Runtime::ResolveMeshRendererAssets(*mesh, *services.cache, *services.database);
            }
        }
    }

    // Propagate before building bodies, for the reason App::BuildSceneBodies
    // exists: a member parented to another is placed from its parent's world
    // matrix, and the matrix does not exist until propagation has run over the
    // entities that were just created.
    world.propagationTick = Runtime::PropagateTransforms(world.scene, world.propagationTick);
    const Physics::PhysicsWorld::ParentWorldFn parentWorld = ParentWorldResolver(world.scene);

    for (const ECS::Entity member : members)
    {
        const ECS::Transform               *transform  = world.scene.Get<ECS::Transform>(member);
        const Physics::RigidBodyDescriptor *descriptor = world.scene.Get<Physics::RigidBodyDescriptor>(member);
        if (transform != nullptr && descriptor != nullptr && world.scene.Get<Physics::RigidBody>(member) == nullptr)
            world.physics.AddBodyFromDescriptor(world.scene, member, *transform, *descriptor, parentWorld);
    }

    return id;
}

bool DestroyInstance(World &world, ECS::InstanceId instanceId)
{
    if (world.instances.Find(instanceId) == nullptr)
        return false;

    for (const ECS::Entity member : Runtime::MembersOf(world.scene, instanceId))
    {
        // Before the entity goes: destroying it drops the RigidBody component but
        // not the Jolt body it referenced, which is a separate handle in the
        // physics world and would keep colliding.
        if (const Physics::RigidBody *body = world.scene.Get<Physics::RigidBody>(member))
            world.physics.RemoveBody(*body);

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
