/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/World.hpp>

#include <Assisi/App/LevelRuntime.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AssetResolve.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <algorithm>
#include <vector>

namespace Assisi::App
{

World &WorldManager::Create(std::string_view label)
{
    std::unique_ptr<World> world = std::make_unique<World>();
    world->name.assign(label).append("#").append(std::to_string(_nextId++));

    World &ref = *world;
    _worlds.push_back(std::move(world));
    return ref;
}

bool WorldManager::Destroy(std::string_view name)
{
    const auto it = std::ranges::find_if(_worlds, [name](const std::unique_ptr<World> &w)
                                         { return w->name == name; });
    if (it == _worlds.end())
    {
        Core::Log::Warn("WorldManager: Destroy('{}') — no such world.", name);
        return false;
    }

    // The app dereferences both roles unconditionally every frame, so a world
    // holding one may only be destroyed after the role has moved to a successor.
    if (it->get() == _active)
    {
        Core::Log::Error("WorldManager: refusing to destroy '{}' — it is the active world. "
                         "Activate a successor first.",
                         name);
        return false;
    }
    if (it->get() == _edited)
    {
        Core::Log::Error("WorldManager: refusing to destroy '{}' — it is the edited world.", name);
        return false;
    }

    _worlds.erase(it);
    return true;
}

World *WorldManager::Find(std::string_view name)
{
    const auto it = std::ranges::find_if(_worlds, [name](const std::unique_ptr<World> &w)
                                         { return w->name == name; });
    return it == _worlds.end() ? nullptr : it->get();
}

const World *WorldManager::Find(std::string_view name) const
{
    const auto it = std::ranges::find_if(_worlds, [name](const std::unique_ptr<World> &w)
                                         { return w->name == name; });
    return it == _worlds.end() ? nullptr : it->get();
}

World *WorldManager::LoadLevel(std::string_view levelPath)
{
    World *const outgoing = _active;

    World &incoming = Create("Level");
    incoming.state  = WorldState::Loading;

    bool loaded = false;
    if (_services.cache != nullptr && _services.database != nullptr && _services.renderer != nullptr)
    {
        // Keep, never ClearFirst: the outgoing world is still alive (and still
        // being drawn) until the swap below.
        loaded = App::LoadLevel(incoming.scene, levelPath, *_services.cache, *_services.database,
                                incoming.physics, *_services.renderer, AssetCacheReset::Keep);
    }
    else
    {
        // No render services (a headless server): the scene and its bodies are
        // all that matter.
        loaded = Runtime::SceneSerializer::LoadFromFile(incoming.scene, levelPath);
        if (loaded)
            incoming.physics.RebuildSceneBodies(incoming.scene);
    }

    if (!loaded)
    {
        // A failed travel must never strand the game between worlds: drop the
        // half-built one and leave everything else exactly as it was.
        Core::Log::Error("Travel to '{}' failed; staying in '{}'.", levelPath,
                         outgoing != nullptr ? outgoing->name : std::string_view{"(none)"});
        const std::string name = incoming.name;
        std::erase_if(_worlds, [&name](const std::unique_ptr<World> &w) { return w->name == name; });
        return nullptr;
    }

    incoming.levelPath = std::string(levelPath);
    incoming.state     = WorldState::Active;
    incoming.simulate  = true;
    _active            = &incoming;

    if (outgoing != nullptr && outgoing != &incoming)
    {
        // Anything the outgoing world queued for destruction is applied now — the
        // world may be about to disappear, and if it is the edited one those
        // destroys must not surface later as a surprise on Stop.
        outgoing->scene.FlushDestroyed();
        outgoing->simulate = false;

        if (outgoing == _edited)
        {
            // The authored level stays resident so Stop can restore it.
            outgoing->state = WorldState::Dormant;
        }
        else
        {
            outgoing->state = WorldState::Unloading;
            const std::string name = outgoing->name;
            std::erase_if(_worlds, [&name](const std::unique_ptr<World> &w) { return w->name == name; });
        }
    }

    Core::Log::Info("Travel: now in '{}' ({}), {} world(s) resident.", incoming.name, levelPath,
                    _worlds.size());
    return &incoming;
}

ECS::Entity WorldManager::MigrateEntity(World &src, World &dst, ECS::Entity root)
{
    if (Find(src.name) == nullptr || Find(dst.name) == nullptr)
    {
        Core::Log::Error("MigrateEntity: source or destination world is not managed here.");
        return ECS::NullEntity;
    }
    if (&src == &dst || !src.scene.IsAlive(root))
    {
        Core::Log::Error("MigrateEntity: root is not alive in the source world (or src == dst).");
        return ECS::NullEntity;
    }

    // The whole subtree travels with the root — a player carries whatever is
    // parented to it. The set is closed under Parent, so TransferEntities never
    // has to null an in-subtree child ref.
    const std::vector<ECS::Entity> subtree = Runtime::GatherSubtree(src.scene, root);

    // Tear down each migrated entity's Jolt body in the SOURCE world before the
    // ECS entities leave. Destroying an entity drops its RigidBody component but
    // not the Jolt body it referenced — that is a separate handle in src.physics,
    // and would leak (and keep colliding) otherwise.
    for (const ECS::Entity e : subtree)
    {
        if (const Physics::RigidBody *body = src.scene.Get<Physics::RigidBody>(e))
            src.physics.RemoveBody(*body);
    }

    // Move the component data. This creates the destination entities, remaps
    // in-set EntityRefs, and destroys the source entities (deferred).
    const std::vector<ECS::Entity> arrived =
        Runtime::SceneSerializer::TransferEntities(src.scene, dst.scene, subtree);
    src.scene.FlushDestroyed();

    // Rebuild transients in the DESTINATION world. RigidBody and the MeshRenderer
    // pointers are transient (never serialized), so the arrived entities have the
    // durable RigidBodyDescriptor/mesh ids but no live body or resolved GPU
    // pointers yet.
    for (const ECS::Entity e : arrived)
    {
        const Runtime::Transform          *transform = dst.scene.Get<Runtime::Transform>(e);
        const Physics::RigidBodyDescriptor *desc      = dst.scene.Get<Physics::RigidBodyDescriptor>(e);
        if (transform != nullptr && desc != nullptr && dst.scene.Get<Physics::RigidBody>(e) == nullptr)
            dst.physics.AddBodyFromDescriptor(dst.scene, e, *transform, *desc);

        if (Runtime::MeshRenderer *mesh = dst.scene.Get<Runtime::MeshRenderer>(e);
            mesh != nullptr && _services.cache != nullptr && _services.database != nullptr)
            Runtime::ResolveMeshRendererAssets(*mesh, *_services.cache, *_services.database);
    }

    // arrived is parallel to subtree, and subtree[0] is the root (GatherSubtree is
    // root-first), so arrived[0] is the destination handle of the root.
    Core::Log::Info("Migrate: moved {} entit{} from '{}' to '{}'.", arrived.size(),
                    arrived.size() == 1 ? "y" : "ies", src.name, dst.name);
    return arrived.empty() ? ECS::NullEntity : arrived.front();
}

bool WorldManager::SweepAssetCache()
{
    if (_services.cache == nullptr || _services.database == nullptr || _services.renderer == nullptr)
        return false;

    // The sweep condition, stated as the code sees it: exactly one world that is
    // still drawable, plus at most the edited world sitting dormant. In a game
    // build that is "one world left after travel"; in the editor the edited world
    // is resident for the whole session, so demanding literally one world would
    // mean the sweep never ran there.
    World *dormantEdited = nullptr;
    World *live          = nullptr;
    for (const std::unique_ptr<World> &world : _worlds)
    {
        if (world.get() == _edited && world->state == WorldState::Dormant)
        {
            dormantEdited = world.get();
            continue;
        }
        if (live != nullptr)
        {
            Core::Log::Debug("AssetCache sweep skipped: {} live worlds resident.", _worlds.size());
            return false;
        }
        live = world.get();
    }
    if (live == nullptr)
        return false;

    // The dormant world's MeshRenderers point into what is about to be freed. It
    // is not drawn, so dropping the pointers is enough; Stop's rebind rebuilds
    // them. Do it BEFORE the Clear so nothing holds a stale pointer at any point.
    if (dormantEdited != nullptr)
        Runtime::ClearSceneAssetBindings(dormantEdited->scene);

    _services.cache->Clear();
    _services.renderer->InvalidateAssetBindings();

    // Re-resolve re-imports from disk asynchronously — the survivor will show
    // placeholders for a moment, exactly as on a normal level load.
    Runtime::ResolveSceneAssets(live->scene, *_services.cache, *_services.database);
    live->streamingPending = true;

    Core::Log::Info("AssetCache swept after travel (survivor '{}'{}).", live->name,
                    dormantEdited != nullptr ? ", edited world's bindings dropped" : "");
    return true;
}

std::size_t WorldManager::DestroyAllExcept(World &keep)
{
    _active = &keep;
    _edited = &keep;

    const std::size_t before = _worlds.size();
    std::erase_if(_worlds, [&keep](const std::unique_ptr<World> &w) { return w.get() != &keep; });
    return before - _worlds.size();
}

void WorldManager::SetActive(World &world)
{
    _active = &world;
}

void WorldManager::SetEdited(World &world)
{
    _edited = &world;
}

void SyncUnrenderedWorld(World &world)
{
    // Poses first: without this the propagation below would compute correct
    // matrices for positions the bodies left behind at spawn.
    world.physics.SyncTransforms(world.scene);
    world.propagationTick = Runtime::PropagateTransforms(world.scene, world.propagationTick);
}

} // namespace Assisi::App
