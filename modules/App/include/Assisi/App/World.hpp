/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file World.hpp
/// @brief A level at runtime — scene + physics + the state that is per-level
///        rather than per-app — and the manager that owns every resident one.
///
/// Several worlds can be resident at once, because one "the scene" plus one
/// "the physics world" only carries a host that never has two levels alive: a
/// game changing level mid-play needs the outgoing and incoming levels alive at
/// the same time, and a game running two levels at once (different players in
/// different places) needs both simulating.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::Core
{
class AssetDatabase;
}
namespace Assisi::Render
{
class AssetCache;
}
namespace Assisi::Runtime
{
class SceneRenderer;
}

namespace Assisi::App
{

/// @brief Where a world is in its lifecycle. Only worlds that are Active or
/// Dormant are usable; the other two states are transitions.
enum class WorldState : std::uint8_t
{
    Loading,   ///< Being filled in; not yet steppable or renderable.
    Active,    ///< Live: simulated (if `simulate`) and eligible to be rendered.
    Dormant,   ///< Resident and inspectable, but not stepped — e.g. the edited
               ///< world while the game has travelled elsewhere.
    Unloading, ///< Scheduled for destruction at the next safe point.
};

/// @brief One resident level: its entities, its physics, and the bookkeeping
/// that is per-level rather than per-app.
///
/// Addresses are stable for the world's lifetime — the edit history, the
/// panels, and (after the networking merge) a replication session all hold
/// references into a world, so WorldManager must never reseat one. That is why
/// worlds are heap-allocated behind unique_ptr rather than stored by value.
///
/// Non-copyable and non-movable: PhysicsWorld owns Jolt state that entity
/// components point into, and ECS::Scene is likewise pinned.
struct World
{
    World()  = default;
    ~World() = default;

    World(const World &)            = delete;
    World &operator=(const World &) = delete;
    World(World &&)                 = delete;
    World &operator=(World &&)      = delete;

    ECS::Scene scene;
    Physics::PhysicsWorld physics;

    /// One row per live blueprint instance in this world: which file it is of,
    /// where it was placed, and where the level file put it.
    ///
    /// The world owns it, not the scene, because it is not entity data — the only
    /// thing a blueprint leaves on the entities is the ECS::BlueprintMember tag,
    /// which says *which* instance an entity belongs to and never *what* that
    /// instance is. Ids are per world and restart at 1 on every load, because the
    /// table is discarded with the world.
    Runtime::InstanceTable instances;

    /// This world's game systems, installed once from its level's list (see
    /// WorldManager::ApplySystems). Per world rather than per app because a
    /// system's cross-frame state lives in its registered lambda's captures: one
    /// shared instance running over several worlds would advance that state N×
    /// too fast. Empty until the level's
    /// list is applied, which is deliberate — Create() installs nothing.
    SystemRegistry systems;

    /// The system names the **level asked for**, in file order. What a save
    /// writes back, so it is preserved verbatim: rewriting it with whatever was
    /// actually installed would silently destroy the author's choice in a host
    /// that happens not to declare one of them.
    ///
    /// A blueprint spawned into this world adds its own names to the registry
    /// without touching this list — those belong to the blueprint's file, not to
    /// the level's.
    std::vector<std::string> systemNames;

    /// Systems a blueprint spawned into this world has asked for, waiting for the
    /// frame's safe point (App::QueueSystemInstall / App::DrainSystemInstalls).
    ///
    /// **A member, not a process-global list keyed by `World*`.** The frame loop
    /// drains one line after the marshalled work where deferred level loads land,
    /// which is exactly what frees worlds — so an entry held outside the world
    /// could name freed memory. Owned by the world, the queue cannot outlive it,
    /// and replacing the world's content (ApplySystems) drops it with the rest of
    /// what that content owned.
    struct PendingSystems
    {
        /// A union, not a concatenation — a hundred bullets spawned in one frame
        /// leave one name, not a hundred.
        std::vector<std::string> names;
        /// Named in the error if one of them turns out to be undeclared: the
        /// blueprint that asked first. Whichever spawn opened the queue owns the
        /// message, which is arbitrary only when several are equally to blame.
        std::string context;
    };
    PendingSystems pendingSystems;

    /// Unique key within the manager. Deliberately NOT the level path: travel
    /// A→B→A leaves two worlds of one path resident, so names are generated.
    std::string name;

    /// Virtual path of the level loaded into this world ("" = never loaded, or
    /// built in memory). What the level handshake and the title bar report.
    std::string levelPath;

    WorldState state = WorldState::Loading;

    /// Stepped by the fixed-update loop? Whether a world with nobody in it
    /// ticks is game policy, not engine policy — the engine only exposes the
    /// flag.
    bool simulate = false;

    /// UpgradeStreamingAssets' per-scene "loads were in flight last tick" flag,
    /// which drives the one-tick tail that picks up the final resolve.
    bool streamingPending = false;

    /// Change-detection bookmark for Runtime::PropagateTransforms — the scene
    /// tick at the end of this world's last propagation. Per world, not per
    /// renderer: one renderer serving two worlds would skip propagation in
    /// whichever it drew second.
    std::uint64_t propagationTick = 0;

    /// The manager that created this world, or null for one built standalone (a
    /// test, or a headless process with no manager). Set by WorldManager::Create
    /// and never by anything else — a world belongs to exactly one manager for
    /// its whole life.
    ///
    /// It exists so a call that has only a World can still reach the engine
    /// services a spawn needs: App::SpawnBlueprint resolves the new members'
    /// assets through them, which is what keeps a spawned car from arriving with
    /// unresolved meshes. Null simply means no asset resolve, which is correct for
    /// a headless host and for a test.
    class WorldManager *manager = nullptr;
};

/// @brief Owns every resident world and tracks the two roles the app cares
/// about: which world is *active* and which is *edited*.
///
/// The roles are distinct on purpose. The **active** world is the one being
/// rendered and driven by input — it changes when the game travels to another
/// level. The **edited** world is the one the editor saves, marks dirty, and
/// undoes into; it stays put across a play session no matter where the game
/// travels, which is what lets Stop restore the level the author was working on
/// with its undo history intact. In a game build nothing is edited and the role
/// is simply unset.
class WorldManager
{
public:
    WorldManager() = default;

    /// Waits out any in-flight background load before the worlds are destroyed —
    /// a worker must never be left writing into freed scene/physics.
    ~WorldManager();

    WorldManager(const WorldManager &)            = delete;
    WorldManager &operator=(const WorldManager &) = delete;
    // Every world it owns holds a `manager` back-pointer to it, so relocating one
    // dangles all of them.
    WorldManager(WorldManager &&)            = delete;
    WorldManager &operator=(WorldManager &&) = delete;

    /// @brief Creates a world and returns it. The name is generated from
    /// @p label plus a monotonic counter, so callers never collide and two
    /// worlds of the same level are distinguishable.
    ///
    /// The new world starts Loading, unsimulated, holds no role, and has an
    /// **empty system registry** — the level's list is applied at the commit
    /// points below, never here, so a world is never at risk of being installed
    /// into twice (see ApplySystems).
    World &Create(std::string_view label = "World");

    // --- Systems -------------------------------------------------------------
    //
    // Which systems a world runs is decided once, when its content is committed,
    // by the *list of names* its file carries. Code defines the systems — they
    // are C++ functions, so data cannot create them — and declares them with
    // ASYSTEM, which is what puts them in the catalog; a file merely names them.

    /// @brief Installs @p names into @p world, replacing whatever it had.
    ///
    /// Resolves every name first, then clears (see SystemRegistry::Clear —
    /// re-registering over an existing set corrupts the ordering graph, because
    /// After()/Before() bind to the first entry of a name) and installs from
    /// SystemCatalog. The clear also takes @p world's **queued** installs
    /// (`World::pendingSystems`): systems asked for by a blueprint the outgoing
    /// level spawned belong to content this call is replacing, and draining them
    /// afterwards would install them into the incoming level.
    ///
    /// @param context Named in the error when a name is unknown — the level path.
    /// @return false if any name is unknown, in which case **nothing changed** —
    ///         not the registry, not the queue. A half-installed world runs and
    ///         looks nearly right, which is the failure mode worth avoiding; the
    ///         caller refuses the load. (`world.systemNames` is the exception: it
    ///         records what the file asked for either way, since that is what a
    ///         save round-trips.)
    /// `[[nodiscard]]`: silently discarding the result loads a level that runs
    /// none of its systems. Write the discard down where it is genuinely
    /// meaningless (an empty list).
    [[nodiscard]] bool ApplySystems(World &world, std::span<const std::string> names,
                                    std::string_view context);

    /// @brief Destroys the named world.
    ///
    /// @return false (nothing destroyed) if @p name is unknown, or if it is the
    /// active or the edited world — those roles must be moved to a successor
    /// first, since the app dereferences them unconditionally.
    bool Destroy(std::string_view name);

    [[nodiscard]] World *Find(std::string_view name);
    [[nodiscard]] const World *Find(std::string_view name) const;

    /// @brief The world being rendered and driven by input. Null only before
    /// the app creates its first world.
    [[nodiscard]] World *Active() { return _active; }
    [[nodiscard]] const World *Active() const { return _active; }
    void SetActive(World &world);

    /// @brief The world the editor saves/dirties/undoes into, or null in a game
    /// build. See the class comment for why this is not just Active().
    [[nodiscard]] World *Edited() { return _edited; }
    [[nodiscard]] const World *Edited() const { return _edited; }
    void SetEdited(World &world);

    [[nodiscard]] std::size_t Count() const { return _worlds.size(); }

    /// @brief Calls @p fn(World&) for each resident world in **creation
    /// order** — deterministic, so stepping several worlds is reproducible.
    template <typename F> void ForEach(F &&fn)
    {
        // The guard makes the mutating operations refuse while this is running.
        // The frame loop dispatches game systems from inside a ForEach, and a
        // system calling LoadLevel would erase/append to _worlds under this loop
        // and could destroy the very world whose system is executing. Systems
        // travel through RequestTravel instead; this turns the mistake into an
        // error message rather than a use-after-free.
        ++_iterationDepth;
        struct Unguard
        {
            std::uint32_t &depth;
            ~Unguard() { --depth; }
        } unguard{_iterationDepth};

        for (const std::unique_ptr<World> &world : _worlds)
        {
            fn(*world);
        }
    }
    template <typename F> void ForEach(F &&fn) const
    {
        for (const std::unique_ptr<World> &world : _worlds)
        {
            fn(static_cast<const World &>(*world));
        }
    }

    /// @brief The engine services a world needs to turn a level file into a
    /// running world. Installed once at startup by the app; a headless server
    /// leaves the render-side ones null and gets a scene + physics with no GPU
    /// assets resolved.
    struct Services
    {
        Render::AssetCache *cache    = nullptr;
        const Core::AssetDatabase *database = nullptr;
        Runtime::SceneRenderer *renderer = nullptr;
        /// The scheduler async travel loads on. Null → BeginLoadLevel falls back
        /// to a synchronous load (still correct, just hitches).
        Core::JobSystem *jobs = nullptr;
    };
    void SetServices(const Services &services) { _services = services; }

    /// @brief The installed services. For code that has a World and needs the
    /// engine-wide pieces a load would use — App::SpawnBlueprint resolving a
    /// freshly spawned instance's assets, notably.
    [[nodiscard]] const Services &GetServices() const { return _services; }

    /// @brief **Travel**: loads @p levelPath into a new world, makes it the
    /// active one, and retires the world that was active.
    ///
    /// This is the game-facing "change level" call — the thing a trigger volume
    /// or a match-end handler invokes. It is deliberately not the editor's Open
    /// Level, which changes *what you are editing* and reuses the edited world in
    /// place; travel replaces the world being played.
    ///
    /// What happens to the outgoing world depends on its role. The **edited**
    /// world is never destroyed — it goes Dormant and unsimulated, so Stop can
    /// still restore the level its author was working on with its undo history
    /// intact. Any other outgoing world is destroyed.
    ///
    /// The asset cache is *not* cleared during the load: the outgoing world is
    /// still alive and rendering while the new one is built. Once the swap is
    /// done, SweepAssetCache() below is the moment to reclaim.
    ///
    /// @return the new world, or nullptr if the level did not load — in which
    /// case nothing changed: the half-created world is destroyed and the caller
    /// keeps playing where it was. Call only at a frame safe point, never
    /// between BeginFrame and EndFrame.
    World *LoadLevel(std::string_view levelPath);

    // --- Deferred travel -----------------------------------------------------
    //
    // The game-facing way to change level. LoadLevel itself is a frame-safe-point
    // operation (it resolves GPU assets and destroys worlds), and the thing most
    // likely to want it — a trigger volume, a match-end handler — runs inside the
    // frame loop's iteration over the worlds, which is the one place it must not
    // be called from. So game code *requests*, and the host applies it at a point
    // where both are safe.

    /// @brief Queues a travel to @p levelPath, to be applied by
    /// ProcessTravelRequest() at the host's next frame safe point.
    ///
    /// Safe to call from anywhere, including from inside a system — that is the
    /// point of it. Only the most recent request survives: two systems asking for
    /// different levels in one frame is a game-logic conflict, and taking the last
    /// one keeps it deterministic rather than travelling twice.
    void RequestTravel(std::string_view levelPath);

    /// @brief Whether a travel has been requested and not yet applied.
    [[nodiscard]] bool HasTravelRequest() const { return _travelRequest.has_value(); }

    /// @brief Applies a queued travel, if any, and clears the request.
    ///
    /// Call once per frame from a safe point — never between BeginFrame and
    /// EndFrame, and never from inside ForEach. Does nothing when no request is
    /// pending. @return whatever LoadLevel returned (nullptr on failure or when
    /// nothing was queued).
    World *ProcessTravelRequest();

    /// @brief Moves an entity — and its whole subtree — from one resident world
    /// into another, keeping its component state and rebuilding its transients in
    /// the destination.
    ///
    /// This is the "persistent across travel" set (Unreal's seamless-travel
    /// entities): the player, their inventory. The game marks what travels;
    /// everything else belongs to the level and is left behind. Subtree-aware —
    /// children come with the root. EntityRefs within the moved set remap;
    /// refs pointing outside it null with a warning (Runtime::TransferEntities).
    ///
    /// Transients are rebuilt per destination world: the Jolt body is removed
    /// from @p src's PhysicsWorld and recreated from its RigidBodyDescriptor in
    /// @p dst's, and MeshRenderer pointers re-resolve against the cache. Needs the
    /// render Services installed (for the mesh re-resolve); without them the
    /// component data still moves and physics rebuilds, but meshes stay
    /// unresolved until something else resolves the destination scene.
    ///
    /// @return the destination handle of @p root, or NullEntity if @p src or
    /// @p dst is unknown to this manager, or @p root is not alive in @p src.
    ECS::Entity MigrateEntity(World &src, World &dst, ECS::Entity root);

    // --- Async travel (S5) ---------------------------------------------------
    //
    // Travel in three moves instead of one, so a heavy level never hitches the
    // running world: begin a background load, let it finish while the current
    // world keeps simulating, then swap when you choose to. The swap is O(1) —
    // all the cost (parsing the file, building the Jolt bodies) was paid on a
    // worker.
    //
    // What is safe off the main thread, and why it is split this way: a Loading
    // world's scene and physics are touched by nobody else (the fixed loop skips
    // it, the renderer never draws it), so deserializing into it and building its
    // Jolt bodies — a *separate* PhysicsSystem from the one being stepped — runs
    // on a worker with no contention. Resolving its GPU assets is NOT thread-safe,
    // so that half waits for the main-thread promotion, after which assets stream
    // in exactly as on any load.

    /// @brief Starts loading @p levelPath into a new Loading world on a worker,
    /// without disturbing the active world. Drive it each frame with
    /// PumpPendingLoad(), poll PendingLoadReady(), then swap with
    /// PromotePendingLoad(). With no jobs service the deserialize runs
    /// synchronously; asset streaming still proceeds across frames.
    ///
    /// @return the Loading world (address stable), or nullptr if a load is
    /// already pending (one at a time) or the world could not be created.
    World *BeginLoadLevel(std::string_view levelPath);

    /// @brief Advances a pending background load: once the worker has
    /// deserialized, this resolves the world's assets and streams them in — on the
    /// main thread, over successive frames — so that "ready" means meshes and
    /// materials are actually resident, not just the scene graph. Call once per
    /// frame at a safe point (it touches the asset cache / GPU). No-op if nothing
    /// is pending or the worker is still running.
    void PumpPendingLoad();

    /// @brief True when a background load is fully ready for an instant
    /// PromotePendingLoad() — deserialized AND its assets streamed in (no pop-in).
    /// False if none is pending or it is still loading/streaming.
    [[nodiscard]] bool PendingLoadReady() const;

    /// @brief Progress of the pending load in [0, 1] — advances as the worker
    /// deserializes, reaching 1.0 when ready. 0 if none is pending. A UI can show
    /// this as a percentage; it is only a rough guide (the asset streaming after
    /// promotion is separate).
    [[nodiscard]] float PendingLoadProgress() const;

    /// @brief Whether a background load is in flight or awaiting promotion.
    [[nodiscard]] bool HasPendingLoad() const { return _pending.has_value(); }

    /// @brief The level path of the pending load, or "" if none.
    [[nodiscard]] std::string_view PendingLoadPath() const;

    /// @brief Swaps the finished background world in as active, retiring the
    /// outgoing world (the edited world goes dormant; a play world is destroyed) —
    /// the same swap travel does, but instant. Resolves the new world's GPU assets
    /// first (the half the worker could not do), so streaming pop-in follows
    /// exactly as on a normal load. If the load is not ready yet this blocks
    /// (help-waiting) until it is — the "swap now anyway" path.
    ///
    /// @return the promoted world, or nullptr if none was pending or the load
    /// failed (in which case the half-built world is destroyed and the active
    /// world is untouched). Call only at a frame safe point (it may resolve/free
    /// GPU assets), never between BeginFrame and EndFrame.
    World *PromotePendingLoad();

    /// @brief Abandons a pending load: waits out the worker (there is no
    /// cancellation token yet) and destroys the half-built world. Used by teardown
    /// and by Stop, which must not leave a worker writing into a world it is about
    /// to free.
    void CancelPendingLoad();

    /// @brief Reclaims GPU memory after a travel, when it is safe to.
    ///
    /// "Safe" is narrow and deliberately so: every live world's MeshRenderers
    /// hold raw pointers into the cache, so a Clear is only allowed when at most
    /// one world can still be drawing — one live world, plus at most a dormant
    /// edited world whose bindings are dropped here and rebuilt when it is
    /// restored. Under those conditions this clears the cache and re-resolves
    /// the survivor, which re-imports its assets from disk asynchronously
    /// (expect the same placeholder pop-in as a normal level load). Otherwise it
    /// does nothing and says why at debug level.
    ///
    /// @return true if the sweep ran.
    bool SweepAssetCache();

    /// @brief Destroys every world except @p keep, moving both roles to it first.
    /// Used by Stop, which tears down whatever the play session created.
    /// @return how many worlds were destroyed.
    std::size_t DestroyAllExcept(World &keep);

private:
    // A vector, not a map: worlds number in the handful, so the O(n) name lookup
    // is cheaper than hashing, and creation order gives deterministic iteration.
    // unique_ptr elements keep addresses stable across insert/erase, which the
    // references held by EditHistory and the panels depend on.
    std::vector<std::unique_ptr<World>> _worlds;
    World *_active = nullptr;
    World *_edited = nullptr;
    std::uint32_t _nextId = 1;
    Services _services;

    // Non-zero while a ForEach is walking _worlds; the mutating operations refuse
    // rather than invalidate it. A counter, not a flag, so nested iteration
    // unwinds correctly.
    std::uint32_t _iterationDepth = 0;
    std::optional<std::string> _travelRequest;

    // Logs and returns false when @p what would mutate the world list mid-walk.
    [[nodiscard]] bool RefuseWhileIterating(std::string_view what) const;

    // An in-flight or ready-to-promote background load. The worker fills
    // `world`'s scene + physics and returns whether it succeeded; a synchronous
    // fallback (no jobs service) records the same result in `syncResult` with an
    // invalid task. At most one at a time.
    struct PendingLoad
    {
        World *world = nullptr;
        Core::Task<bool>       task;                 ///< Invalid in the sync path.
        std::string path;
        std::optional<bool>    syncResult = std::nullopt; ///< Set (only) by the sync fallback.

        // Phase-1 (deserialize) progress in [0,1], written by the worker, read by
        // the UI thread. shared_ptr so the worker lambda owns a copy (outliving any
        // PendingLoad move) and so the atomic doesn't make PendingLoad non-movable.
        std::shared_ptr<std::atomic<float>> deserProgress = std::make_shared<std::atomic<float>>(0.f);

        // Phase-2 (asset streaming) state, all main-thread only, advanced by
        // PumpPendingLoad once the worker is done.
        bool workerDone     = false;        ///< Worker finished (or sync path); scene now safe to touch.
        bool workerOk       = false;        ///< ...and it succeeded.
        bool resolveStarted = false;        ///< ResolveSceneAssets has kicked off the streams.
        std::size_t resolveInitialPending = 0; ///< Cache pending-count captured when resolve began.
        float assetProgress  = 0.f;         ///< [0,1] fraction of the streams landed.
        bool ready          = false;        ///< Deserialized AND assets resident (or a failed load).
    };
    std::optional<PendingLoad> _pending;

    // Makes @p incoming the active world and retires whoever was active — the
    // edited world goes dormant, any other outgoing world is destroyed. Shared by
    // synchronous LoadLevel and async PromotePendingLoad.
    World *SwapToActive(World &incoming, std::string levelPath);

    // Removes @p world from the store unconditionally (no role checks). Used for
    // half-built worlds that never held a role. Safe to call with a dangling name
    // capture only after any worker touching it has finished.
    void EraseWorld(World &world);
};

/// @brief Resolves the MeshRenderer of every entity in @p entities from @p
/// world's render services — the step that makes freshly created entities
/// *drawable* rather than merely present.
///
/// Serialized and prepared component data holds durable asset **ids**; the draw
/// path reads the transient GPU pointers Runtime::ResolveMeshRendererAssets
/// derives from them. A level load resolves its whole scene at once
/// (RebindSceneAssetsAndPhysics); entities that appear afterwards — a spawn, a
/// migration, a blueprint instance arriving over the wire — have to be resolved
/// as they are created, or they draw nothing at all.
///
/// **A no-op when the world has no manager, or its manager has no cache and
/// database.** That is the headless case and it is correct rather than merely
/// tolerated: a dedicated server holds the same entities and has no GPU to
/// resolve them onto.
void ResolveEntityAssets(World &world, std::span<const ECS::Entity> entities);

/// @brief Brings a simulated-but-unrendered world's Transforms up to date, in the
/// order that actually works: **poses first, matrices second**.
///
/// The render path does two things for the world it draws — writes physics poses
/// into Transforms, then propagates those into world matrices. A resident world
/// that simulates without being drawn gets neither. Propagating alone is the
/// tempting half-fix and is worse than useless: it produces perfectly correct
/// matrices of stale spawn poses. Call once per frame, after the fixed-step loop,
/// for every simulated world that is not the one being rendered.
void SyncUnrenderedWorld(World &world);

/// @brief Builds the parent-world lookup physics needs to place and read back a
/// parented entity's pose (Physics::PhysicsWorld::ParentWorldFn).
///
/// Every path that creates bodies from a scene or writes physics poses into
/// Transforms must pass this, or a parented body is created at its local pose and
/// then drifts by its parent's transform every frame afterwards. Blueprint
/// instances make parented bodies ordinary rather than exotic — a car's wheels
/// are under its body.
///
/// The lookup reads `Transform::worldMatrix`, which is transient and computed by
/// Runtime::PropagateTransforms, so **propagate before building bodies** from a
/// freshly loaded or restored scene. App::BuildSceneBodies does that in the right
/// order; prefer it over calling RebuildSceneBodies directly.
///
/// The returned callable borrows @p scene and is valid for as long as it is.
[[nodiscard]] Physics::PhysicsWorld::ParentWorldFn ParentWorldResolver(ECS::Scene &scene);

/// @brief Rebuilds a scene's physics bodies in the order that works: **propagate
/// first, then create bodies**.
///
/// A body is created in world space and a parented entity's Transform is an
/// offset from its parent, so the parent's world matrix has to exist before the
/// body can be placed. World matrices are transient — never serialized — so a
/// freshly loaded or restored scene has none until propagation runs. Doing it the
/// other way round places every parented body at its local pose.
///
/// @param propagationTick The caller's propagation bookmark, or 0 to recompute
///        every matrix (correct for a scene whose entities were just replaced).
/// @return The scene's change tick after propagating; store it if the caller
///         keeps a bookmark, discard it to simply lose one frame of skipping.
uint64_t BuildSceneBodies(ECS::Scene &scene, Physics::PhysicsWorld &physics, uint64_t propagationTick = 0);

} // namespace Assisi::App
