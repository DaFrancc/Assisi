/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file World.hpp
/// @brief A level at runtime — scene + physics + the per-level state that used
///        to live as scattered members of the app — and the manager that owns
///        them. Design: docs/multi-scene-design-notes.md.
///
/// An app used to hold "the" scene and "the" physics world as members. That
/// works exactly as long as one level is resident, which is not long: a game
/// changing level mid-play needs the outgoing and incoming levels alive at the
/// same time, and a game running two levels at once (different players in
/// different places) needs both simulating. Both are just "more than one
/// World".
///
/// Stage S1 introduces the bundle and moves the app onto it without changing
/// behaviour: exactly one world exists, and it is both the active and the
/// edited world. Multiple residents (S2) and in-play level change (S3) build on
/// this without touching the panels.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
    World() = default;
    World(const World &)            = delete;
    World &operator=(const World &) = delete;

    ECS::Scene            scene;
    Physics::PhysicsWorld physics;

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
    /// @brief Creates a world and returns it. The name is generated from
    /// @p label plus a monotonic counter, so callers never collide and two
    /// worlds of the same level are distinguishable.
    ///
    /// The new world starts Loading, unsimulated, and holds no role — the
    /// caller loads into it and then activates it.
    World &Create(std::string_view label = "World");

    /// @brief Destroys the named world.
    ///
    /// @return false (nothing destroyed) if @p name is unknown, or if it is the
    /// active or the edited world — those roles must be moved to a successor
    /// first, since the app dereferences them unconditionally.
    bool Destroy(std::string_view name);

    [[nodiscard]] World       *Find(std::string_view name);
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
        Render::AssetCache        *cache    = nullptr;
        const Core::AssetDatabase *database = nullptr;
        Runtime::SceneRenderer    *renderer = nullptr;
    };
    void SetServices(const Services &services) { _services = services; }

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
    World        *_active = nullptr;
    World        *_edited = nullptr;
    std::uint32_t _nextId = 1;
    Services      _services;
};

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

} // namespace Assisi::App
