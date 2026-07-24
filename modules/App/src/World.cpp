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

WorldManager::~WorldManager()
{
    // A background load must never outlive the worlds it writes into. This waits
    // the worker out (there is no cancellation token) before _worlds is destroyed.
    CancelPendingLoad();
}

World &WorldManager::Create(std::string_view label)
{
    std::unique_ptr<World> world = std::make_unique<World>();
    world->name.assign(label).append("#").append(std::to_string(_nextId++));

    World &ref = *world;
    _worlds.push_back(std::move(world));
    return ref;
}

void WorldManager::EraseWorld(World &world)
{
    const std::string name = world.name;
    std::erase_if(_worlds, [&name](const std::unique_ptr<World> &w) { return w->name == name; });
}

World *WorldManager::SwapToActive(World &incoming, std::string levelPath)
{
    World *const outgoing = (_active == &incoming) ? nullptr : _active;

    incoming.levelPath = std::move(levelPath);
    incoming.state     = WorldState::Active;
    incoming.simulate  = true;
    _active            = &incoming;

    if (outgoing != nullptr)
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
            EraseWorld(*outgoing);
        }
    }
    return &incoming;
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
    // A synchronous travel supersedes any background preload — wait it out and
    // drop it rather than racing two loads.
    CancelPendingLoad();

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
        EraseWorld(incoming);
        return nullptr;
    }

    World *const result = SwapToActive(incoming, std::string(levelPath));
    Core::Log::Info("Travel: now in '{}' ({}), {} world(s) resident.", result->name, levelPath,
                    _worlds.size());
    return result;
}

// ---------------------------------------------------------------------------
// Async travel (S5)
// ---------------------------------------------------------------------------

World *WorldManager::BeginLoadLevel(std::string_view levelPath)
{
    if (_pending)
    {
        Core::Log::Warn("BeginLoadLevel('{}') ignored: a background load ('{}') is already pending.",
                        levelPath, _pending->path);
        return nullptr;
    }

    World &incoming = Create("Level");
    incoming.state  = WorldState::Loading;

    const std::string path(levelPath);

    auto progress = std::make_shared<std::atomic<float>>(0.f);

    if (_services.jobs == nullptr)
    {
        // No scheduler: do the worker half inline. Still correct — just the hitch
        // async travel exists to avoid. Ready for promotion immediately.
        const bool ok = Runtime::SceneSerializer::LoadFromFile(incoming.scene, path);
        if (ok)
            incoming.physics.RebuildSceneBodies(incoming.scene);
        progress->store(1.f);
        _pending = PendingLoad{
            .world = &incoming, .task = {}, .path = path, .syncResult = ok, .progress = progress};
        return &incoming;
    }

    // The worker touches ONLY this world's scene and physics — both untouched by
    // anything else while Loading — plus a copied path and the shared progress
    // atomic. No cache, no renderer, no manager state. The world address is
    // stable, so capturing it is safe across any _worlds reallocation the main
    // thread may do meanwhile.
    World *const w = &incoming;
    Core::Task<bool> task = _services.jobs->Run(
        Core::Pool::Worker,
        [w, path, progress]() -> bool
        {
            // Deserialize drives the bar 0 -> ~0.9 (the entity-scaling cost);
            // building bodies is the cheap tail to 1.0.
            const bool ok = Runtime::SceneSerializer::LoadFromFile(
                w->scene, path, [progress](float f) { progress->store(f * 0.9f); });
            if (!ok)
                return false;
            w->physics.RebuildSceneBodies(w->scene);
            progress->store(1.f);
            return true;
        });

    _pending = PendingLoad{
        .world = &incoming, .task = std::move(task), .path = path, .syncResult = std::nullopt, .progress = progress};
    Core::Log::Info("Preload: '{}' loading in the background (world '{}').", path, incoming.name);
    return &incoming;
}

bool WorldManager::PendingLoadReady() const
{
    if (!_pending)
        return false;
    if (_pending->syncResult.has_value())
        return true; // synchronous fallback finished in BeginLoadLevel
    return _pending->task.IsValid() && _pending->task.IsComplete();
}

std::string_view WorldManager::PendingLoadPath() const
{
    return _pending ? std::string_view{_pending->path} : std::string_view{};
}

float WorldManager::PendingLoadProgress() const
{
    if (!_pending)
        return 0.f;
    if (_pending->syncResult.has_value())
        return 1.f; // the synchronous fallback finished before this could be polled
    return _pending->progress->load();
}

World *WorldManager::PromotePendingLoad()
{
    if (!_pending)
        return nullptr;

    // Take the result. If the worker has not finished, block on it (help-waiting)
    // — the "swap now even though it isn't ready" path.
    bool ok = false;
    if (_pending->syncResult.has_value())
    {
        ok = *_pending->syncResult;
    }
    else if (_pending->task.IsValid())
    {
        _pending->task.Wait();
        ok = _pending->task.Get();
    }

    World *const     incoming = _pending->world;
    const std::string path    = _pending->path;
    _pending.reset();

    if (!ok)
    {
        Core::Log::Error("Preload of '{}' failed; discarding it, active world unchanged.", path);
        EraseWorld(*incoming);
        return nullptr;
    }

    // The GPU half the worker could not do: resolve this world's assets on the
    // main thread. Streaming upgrades the placeholders over the next frames,
    // exactly as on a normal load. Headless (no cache) skips it.
    if (_services.cache != nullptr && _services.database != nullptr)
    {
        Runtime::ResolveSceneAssets(incoming->scene, *_services.cache, *_services.database);
        incoming->streamingPending = true;
    }

    World *const result = SwapToActive(*incoming, path);
    Core::Log::Info("Preload promoted: now in '{}' ({}), {} world(s) resident.", result->name, path,
                    _worlds.size());
    return result;
}

void WorldManager::CancelPendingLoad()
{
    if (!_pending)
        return;

    // No cancellation token exists yet, so "cancel" means wait the worker out and
    // throw the result away — the worker is writing into this world's scene, and
    // freeing it from under a live worker would be a use-after-free.
    if (!_pending->syncResult.has_value() && _pending->task.IsValid())
        _pending->task.Wait();

    World *const incoming = _pending->world;
    const std::string path = _pending->path;
    _pending.reset();
    EraseWorld(*incoming);
    Core::Log::Info("Preload of '{}' cancelled and discarded.", path);
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
    // A background load in flight would be writing into a world this is about to
    // free (or into `keep` if that is the pending world — it never is, since the
    // pending world holds no role). Wait it out and drop it first.
    CancelPendingLoad();

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
