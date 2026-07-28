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

bool WorldManager::RefuseWhileIterating(std::string_view what) const
{
    if (_iterationDepth == 0)
        return false;

    Core::Log::Error("WorldManager: {} was called while iterating the resident worlds — refusing, "
                     "because it would invalidate the walk and can destroy the world whose code is "
                     "running. Game logic changes level with RequestTravel(); the host applies it "
                     "at the next frame safe point.",
                     what);
    return true;
}

void WorldManager::RequestTravel(std::string_view levelPath)
{
    // Deliberately allowed mid-iteration — this is the call a system makes
    // instead of LoadLevel. Last request of the frame wins.
    _travelRequest.emplace(levelPath);
}

World *WorldManager::ProcessTravelRequest()
{
    if (!_travelRequest)
        return nullptr;

    // Take the request before travelling: LoadLevel can run game code (a profile
    // installer), and a request it makes belongs to the next frame, not this one.
    const std::string path = *std::exchange(_travelRequest, std::nullopt);
    return LoadLevel(path);
}

World &WorldManager::Create(std::string_view label)
{
    // Not refusable: Create returns a reference, so there is no failure value.
    // The mutators above are the ones a system could plausibly reach for, and
    // LoadLevel (which calls this) is already guarded at its own entry.
    std::unique_ptr<World> world = std::make_unique<World>();
    world->name.assign(label).append("#").append(std::to_string(_nextId++));

    World &ref = *world;
    _worlds.push_back(std::move(world));
    return ref;
}

void WorldManager::RegisterProfile(std::string_view name, ProfileInstaller installer)
{
    if (name.empty() || !installer)
    {
        Core::Log::Error("WorldManager: RegisterProfile ignored — a profile needs both a name and "
                         "an installer.");
        return;
    }

    for (std::pair<std::string, ProfileInstaller> &profile : _profiles)
    {
        if (profile.first == name)
        {
            profile.second = std::move(installer);
            return;
        }
    }
    _profiles.emplace_back(std::string(name), std::move(installer));
}

void WorldManager::ApplyProfile(World &world, std::string_view name)
{
    const auto find = [this](std::string_view wanted) -> const ProfileInstaller *
    {
        for (const std::pair<std::string, ProfileInstaller> &profile : _profiles)
        {
            if (profile.first == wanted)
                return &profile.second;
        }
        return nullptr;
    };

    // Both names are owned before anything is written: `name` may view
    // world.profile itself (the async path parks the level's request there until
    // promotion), and the assignments below would otherwise read a string while
    // overwriting it.
    const std::string       requested = std::string(name);
    std::string             chosen    = requested.empty() ? _defaultProfile : requested;
    const ProfileInstaller *installer = chosen.empty() ? nullptr : find(chosen);

    if (installer == nullptr && !chosen.empty())
    {
        // Fall back to the default rather than leaving the world systemless: a
        // world that physics-steps but runs no game logic looks like a gameplay
        // bug, not the load error it is.
        Core::Log::Error("World '{}': unknown system profile '{}' — falling back to the default "
                         "profile ('{}').",
                         world.name, chosen,
                         _defaultProfile.empty() ? std::string_view{"(none set)"}
                                                 : std::string_view{_defaultProfile});
        chosen    = _defaultProfile;
        installer = chosen.empty() ? nullptr : find(chosen);
    }

    // Never stack one profile on another: Register is append-only and a repeated
    // name binds every After()/Before() edge to the first entry, so re-targeting a
    // world (the editor opening another level into the one it edits) must start
    // from empty.
    world.systems.Clear();

    // The level's request is recorded whether or not it could be honoured — it is
    // what a save round-trips, and a fallback must not rewrite the file with the
    // name of whatever ran instead.
    world.profile = requested;
    world.installedProfile.clear();

    if (installer == nullptr)
    {
        // No profile at all is the normal state for a host that registered none
        // (the editor with no game module, the tests) — not worth a warning.
        return;
    }

    (*installer)(world);
    world.installedProfile = std::move(chosen);
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
    if (RefuseWhileIterating("Destroy"))
        return false;

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
    if (RefuseWhileIterating("LoadLevel"))
        return nullptr;

    // A synchronous travel supersedes any background preload — wait it out and
    // drop it rather than racing two loads.
    CancelPendingLoad();

    World *const outgoing = _active;

    World &incoming = Create("Level");
    incoming.state  = WorldState::Loading;

    Runtime::LevelHeader header;
    bool                 loaded = false;
    if (_services.cache != nullptr && _services.database != nullptr && _services.renderer != nullptr)
    {
        // Keep, never ClearFirst: the outgoing world is still alive (and still
        // being drawn) until the swap below.
        loaded = App::LoadLevel(incoming.scene, levelPath, *_services.cache, *_services.database,
                                incoming.physics, *_services.renderer, AssetCacheReset::Keep,
                                &header);
    }
    else
    {
        // No render services (a headless server): the scene and its bodies are
        // all that matter.
        loaded = Runtime::SceneSerializer::LoadFromFile(incoming.scene, levelPath,
                                                        /*onProgress=*/{}, &header);
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

    // Content is committed, so the world can be given its systems. Before the
    // swap: the moment it goes Active the frame loop will dispatch it.
    ApplyProfile(incoming, header.profile);

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
    if (RefuseWhileIterating("BeginLoadLevel"))
        return nullptr;

    if (_pending)
    {
        Core::Log::Warn("BeginLoadLevel('{}') ignored: a background load ('{}') is already pending.",
                        levelPath, _pending->path);
        return nullptr;
    }

    World &incoming = Create("Level");
    incoming.state  = WorldState::Loading;

    const std::string path(levelPath);

    auto deserProgress = std::make_shared<std::atomic<float>>(0.f);

    if (_services.jobs == nullptr)
    {
        // No scheduler: do the worker half inline. Still correct — just the hitch
        // async travel exists to avoid. Asset streaming (phase 2) still happens
        // across frames via PumpPendingLoad.
        Runtime::LevelHeader header;
        const bool ok = Runtime::SceneSerializer::LoadFromFile(incoming.scene, path,
                                                               /*onProgress=*/{}, &header);
        if (ok)
        {
            incoming.physics.RebuildSceneBodies(incoming.scene);
            incoming.profile = std::move(header.profile);
        }
        deserProgress->store(1.f);
        _pending                = PendingLoad{.world = &incoming, .task = {}, .path = path, .syncResult = ok};
        _pending->deserProgress = deserProgress;
        _pending->workerDone    = true;
        _pending->workerOk      = ok;
        return &incoming;
    }

    // The worker touches ONLY this world's scene and physics — both untouched by
    // anything else while Loading — plus a copied path and the shared progress
    // atomic. No cache, no renderer, no manager state (asset resolution is GPU
    // work and stays on the main thread, in PumpPendingLoad). The world address is
    // stable, so capturing it is safe across any _worlds reallocation the main
    // thread may do meanwhile.
    World *const w = &incoming;
    Core::Task<bool> task = _services.jobs->Run(
        Core::Pool::Worker,
        [w, path, deserProgress]() -> bool
        {
            // Deserialize drives phase-1 progress 0 -> ~0.9 (the entity-scaling
            // cost); building bodies is the cheap tail to 1.0.
            Runtime::LevelHeader header;
            const bool           ok = Runtime::SceneSerializer::LoadFromFile(
                w->scene, path, [deserProgress](float f) { deserProgress->store(f * 0.9f); },
                &header);
            if (!ok)
                return false;
            w->physics.RebuildSceneBodies(w->scene);
            // Park the level's choice on the world itself; installing it is main-
            // thread work (an installer may touch anything) and happens at
            // promotion, after this task is joined.
            w->profile = std::move(header.profile);
            deserProgress->store(1.f);
            return true;
        });

    _pending                = PendingLoad{.world = &incoming, .task = std::move(task), .path = path};
    _pending->deserProgress = deserProgress;
    Core::Log::Info("Preload: '{}' loading in the background (world '{}').", path, incoming.name);
    return &incoming;
}

void WorldManager::PumpPendingLoad()
{
    if (!_pending || _pending->ready)
        return;

    // Phase 1: wait for the worker. Its scene is off-limits until it completes.
    if (!_pending->workerDone)
    {
        if (!_pending->task.IsValid() || !_pending->task.IsComplete())
            return; // still deserializing on the worker
        _pending->workerDone = true;
        _pending->workerOk   = _pending->task.Get();
    }

    // A failed deserialize has nothing to stream — it is "ready" so Promote can
    // run and discard it.
    if (!_pending->workerOk)
    {
        _pending->ready = true;
        return;
    }

    // No render services (headless): there are no GPU assets to stream.
    if (_services.cache == nullptr || _services.database == nullptr)
    {
        _pending->assetProgress = 1.f;
        _pending->ready         = true;
        return;
    }

    World &world = *_pending->world;

    // Phase 2: resolve the world's assets once, then let them stream in across
    // frames. This is the half the worker could not do (GPU); the world is not
    // rendered yet, so streaming placeholders are invisible.
    if (!_pending->resolveStarted)
    {
        Runtime::ResolveSceneAssets(world.scene, *_services.cache, *_services.database);
        _pending->resolveStarted        = true;
        _pending->resolveInitialPending = _services.cache->PendingLoadCount();
        world.streamingPending          = true;
    }
    else
    {
        App::UpgradeStreamingAssets(world.scene, *_services.cache, *_services.database,
                                    world.streamingPending);
    }

    // Progress across the streams. Cache-wide count, but during a preload the
    // active world is normally settled, so it tracks this world's loads.
    const std::size_t pending = _services.cache->PendingLoadCount();
    if (_pending->resolveInitialPending == 0 || pending == 0)
    {
        _pending->assetProgress = 1.f;
    }
    else
    {
        const float landed = 1.f - static_cast<float>(pending) /
                                       static_cast<float>(_pending->resolveInitialPending);
        _pending->assetProgress = std::clamp(landed, 0.f, 1.f);
    }

    // Ready only when every stream has landed — no pop-in after the swap.
    if (!_services.cache->HasPendingLoads())
        _pending->ready = true;
}

bool WorldManager::PendingLoadReady() const
{
    return _pending && _pending->ready;
}

std::string_view WorldManager::PendingLoadPath() const
{
    return _pending ? std::string_view{_pending->path} : std::string_view{};
}

float WorldManager::PendingLoadProgress() const
{
    if (!_pending)
        return 0.f;
    if (_pending->ready)
        return 1.f;
    // Deserialize is the first half of the bar, asset streaming the second.
    if (!_pending->workerDone)
        return _pending->deserProgress->load() * 0.5f;
    return 0.5f + _pending->assetProgress * 0.5f;
}

World *WorldManager::PromotePendingLoad()
{
    if (RefuseWhileIterating("PromotePendingLoad"))
        return nullptr;

    if (!_pending)
        return nullptr;

    // Bring the load to a promotable point. Normally the caller waited for
    // PendingLoadReady(), so this is immediate; if promotion is forced early, block
    // on the worker (help-waiting) and resolve assets inline — accepting the
    // streaming pop-in the ready-gate exists to avoid.
    if (!_pending->workerDone)
    {
        if (_pending->task.IsValid())
            _pending->task.Wait();
        PumpPendingLoad(); // latches workerDone/workerOk, and kicks off resolve
    }

    const bool        ok       = _pending->workerOk;
    World *const      incoming = _pending->world;
    const std::string path     = _pending->path;
    const bool        resolved = _pending->resolveStarted;
    _pending.reset();

    if (!ok)
    {
        Core::Log::Error("Preload of '{}' failed; discarding it, active world unchanged.", path);
        EraseWorld(*incoming);
        return nullptr;
    }

    // Assets were resolved by PumpPendingLoad while the world was still hidden, so
    // a ready promotion has no pop-in. Only a forced early promote (before the pump
    // ever resolved) needs the fallback resolve here.
    if (!resolved && _services.cache != nullptr && _services.database != nullptr)
    {
        Runtime::ResolveSceneAssets(incoming->scene, *_services.cache, *_services.database);
        incoming->streamingPending = true;
    }

    // The worker parked the level's requested profile here; install it now that we
    // are back on the main thread and the task is joined. Passing the field to a
    // function that also writes it is safe — ApplyProfile owns its copy first.
    ApplyProfile(*incoming, incoming->profile);

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
    if (RefuseWhileIterating("DestroyAllExcept"))
        return 0;

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
