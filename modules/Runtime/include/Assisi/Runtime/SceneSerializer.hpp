/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializer.hpp
/// @brief JSON-based level file save/load for an ECS Scene.
///
/// All reflected (non-transient) component fields are persisted automatically
/// via ComponentRegistry.  Unrecognised component names in a file are skipped
/// with a warning so old levels remain loadable after component renames.
///
/// ## Format (version 2)
/// @code{.json}
/// {
///   "version": 2,
///   "systems": ["Bounce"],
///   "entities": [
///     {
///       "name": "body",
///       "components": {
///         "Transform": { "position": [0,0,0], "rotation": [1,0,0,0], "scale": [1,1,1] },
///         "PointLight": { "color": [1,1,1], "intensity": 100.0, "radius": 20.0 }
///       }
///     },
///     {
///       "name": "wheel_fl",
///       "components": { "Parent": { "parent": "body" } }
///     }
///   ]
/// }
/// @endcode
///
/// Entity IDs are not persisted; loading always clears the scene first and
/// allocates fresh sequential entities so generation numbers stay at zero.
///
/// ## Names
/// Every entity carries a `name`, unique within the file, and every EntityRef
/// field (`Parent::parent` and friends) stores that name rather than a position
/// in the array. Positions are only safe while nothing outside the file points
/// into it, and blueprint overrides point into it by design: an override that
/// says "entity #1" means something different the moment somebody inserts a
/// member above it, and a red wheel silently becomes a red headlight.
///
/// The name lives on the entity as Runtime::Name, the same field the editor
/// shows — one name per entity, not a file identity beside a display label.
/// Load fills it in for every entity, so a name is never absent after a round
/// trip.
///
/// There is **no v1 reader**: a positional path would keep the exact failure
/// names exist to remove.
///
/// A file is refused outright — the load fails and leaves an empty scene — for a
/// duplicate name, a missing or empty name, a name too long for Runtime::Name,
/// or an EntityRef naming an entity the file does not declare. Each of those is
/// a file that means something other than what it says, and the alternative to
/// refusing is a silently mis-wired scene.
///
/// ## Transient fields
/// Fields marked AFIELD(transient) (e.g. GPU handles, raw pointers) are
/// excluded from serialization.  A component whose fields are all transient is
/// saved as an empty object `{}` — its presence on the entity is preserved but
/// no data is restored.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/LevelError.hpp>

namespace Assisi::Runtime
{

/// @brief The parts of a level file that describe the level rather than its
/// entities. Optional in the file; absent fields keep their defaults, so older
/// levels load unchanged.
struct LevelHeader
{
    /// What this file instances, in file order — **as read**. Filled by Load and
    /// never consulted by Save, which builds the array from the live instance
    /// table instead. The index into it is how a level-placed instance is named on
    /// the wire, so the order is part of the format rather than an artefact.
    std::vector<LevelInstance> instances;

    /// @brief The systems this file needs, by name — closer to a module import
    /// than an include.
    ///
    /// The list is a **union, not a concatenation** — naming a system twice, or
    /// two nested blueprints both naming `Bounce`, installs it once — and **file
    /// order carries no meaning**, because run order comes from `after`/`before`
    /// on the system itself. This reader accepts any name; a name this build does
    /// not declare is refused by WorldManager::ApplySystems, because a level that
    /// names a system it does not get is a level that runs without it, silently.
    ///
    /// The list is authored, never derived. Inferring it from the members'
    /// components would have every blueprint with a rigid body declare a
    /// dependency on physics when almost none mean it.
    std::vector<std::string> systems;
};

/// @brief Optional progress reporter for a load, called with a fraction in
/// [0, 1] as the component-deserialize pass advances. Invoked on the thread
/// that drives the load (a worker, for async travel), so an implementation
/// that publishes to another thread must synchronise itself.
using LoadProgressFn = std::function<void (float)>;

/// @brief Everything a load can optionally be given. All inputs — what a failed
/// load did to the scene comes back on the LevelFailure.
///
/// At namespace scope rather than nested in SceneSerializer: GCC parses a nested
/// class's default member initializers only at the end of the enclosing class,
/// which leaves `= {}` unusable as a default argument.
struct LoadOptions
{
    /// Reports progress as the component-deserialize pass advances. Carries an
    /// initializer it does not need so that designated initializers naming only
    /// the later members do not trip -Wmissing-field-initializers.
    LoadProgressFn onProgress = {};

    /// Receives the file's non-entity metadata. Left untouched if the load fails
    /// its version check.
    LevelHeader *header = nullptr;

    /// Receives one row per instance the file places, and is where the ids the
    /// BlueprintMember tags carry are allocated. Passing none for a file that
    /// *has* instances fails the load rather than dropping them: a silently
    /// instance-free level is a level missing most of its content.
    InstanceTable *instances = nullptr;
};

class SceneSerializer
{
public:
    /// @brief Kept as a member name for the callers and signatures that spell it
    /// that way; LoadProgressFn above is the same type.
    using ProgressFn = LoadProgressFn;

    /// @brief Serialize the entire scene to a JSON value, plus @p header.
    ///
    /// The header must be passed by every save that wants to preserve it: a
    /// Scene does not carry it (it is a property of the level, not of the
    /// entities), so a save that omits it strips the field from the file.
    ///
    /// Entities carrying ECS::BlueprintMember are **not** written as entities. They
    /// belong to an instance, and writing them as well would bake a copy of the
    /// blueprint into the level and undo the entire point — a fix to the blueprint
    /// would stop propagating. The `instances` array is built from @p instances
    /// rather than from `header`, so there is one source of truth for where an
    /// instance is: move the row and the file follows.
    ///
    /// @p instances is required if the scene holds any member at all. Without it
    /// the members are still skipped, and the result would be a level missing both
    /// its instances *and* the entities they expanded into — so that case logs an
    /// error rather than quietly writing a smaller level.
    ///
    /// Takes @p instances mutably: writing the file is what decides each row's
    /// position in it, and the rows are renumbered to match — see
    /// InstanceTable::SetLevelInstanceIndex.
    static nlohmann::json Save(ECS::Scene &scene, const LevelHeader &header = {},
                               InstanceTable *instances = nullptr);

    /// @brief Deserialize entities and components from a JSON value into the scene.
    ///
    /// Clears the scene before loading.  Only components registered in
    /// ComponentRegistry are restored; unrecognised names are skipped with a warning.
    /// What @p options can carry is on LoadOptions.
    ///
    /// **Never throws of its own accord.** A wrong `version`, a wrong *shape* —
    /// no `entities` array, a quoted version, a document that is not an object —
    /// or a malformed file (see the naming rules in the file comment) comes back
    /// as a LevelError; the top-level reads are guarded rather than left to
    /// nlohmann, whose answer to a bad shape is a throw. What can still escape is
    /// a component's `addToScene` hook, which is generated code this only calls;
    /// LoadFromFile and LoadFromDisk catch that for the paths they own.
    ///
    /// A version mismatch, a bad top-level shape, and a serialization context
    /// already live on this thread (ContextBusy — a load cannot run inside another
    /// one, because the clear would strand the outer context's tables on destroyed
    /// entities) are all refused *before* the scene is cleared, so that caller
    /// keeps what it had. Every other failure is a file this got partway through,
    /// and leaves an empty scene rather than a half-built one.
    /// LevelFailure::sceneReplaced is how a caller learns which of those it got.
    ///
    /// The return type is the error channel and has to stay one: a bare bool makes
    /// a version mismatch read as a *successful* load of an empty level.
    [[nodiscard]] static LevelResult Load(ECS::Scene &scene, const nlohmann::json &j,
                                          const LoadOptions &options = {});

    /// @brief Expands one instance of @p source into @p scene at @p placement,
    /// outside any level load.
    ///
    /// The runtime spawn path (§7's SpawnBlueprint is a thin wrapper). All or
    /// nothing: a missing nested file three members in leaves no partial instance.
    ///
    /// References inside the blueprint resolve among its own members and nowhere
    /// else, which is the whole difference from the level-load path: a runtime
    /// spawn has no file around it to point at.
    ///
    /// @return the new instance id, or why the blueprint could not be used.
    [[nodiscard]] static std::expected<ECS::InstanceId, LevelError>
    ExpandInstance(ECS::Scene &scene, InstanceTable &instances, std::string_view source,
                   const ECS::Transform &placement);

    /// @brief What a placement produced.
    struct ExpandedInstance
    {
        ECS::InstanceId instanceId;

        /// Parallel to the blueprint's member list, with NullEntity where this
        /// instance removed one. The editor needs the actual entities to build the
        /// undo entry — placing an instance is one InstanceDelta plus one
        /// EntityDelta per member, undone atomically.
        std::vector<ECS::Entity> members;
    };

    /// @brief Places one instance from a full entry — name, source, placement, and
    /// whatever it overrides.
    ///
    /// The authoring counterpart of ExpandInstance above, which is the runtime
    /// spawn: this one is *named*, so its members are addressable as
    /// `car_3/wheel_fl` and a level entity can point at them.
    ///
    /// @param authored true for content an author placed, which a save writes back.
    ///        A runtime spawn is false: it exists because something in the game
    ///        asked for it, and writing it into the file would make it authored the
    ///        next time the level loads.
    [[nodiscard]] static std::expected<ExpandedInstance, LevelError>
    PlaceInstance(ECS::Scene &scene, InstanceTable &instances, const LevelInstance &entry, bool authored);

    /// @brief What a re-expansion left behind.
    struct ReexpandedInstance
    {
        /// Parallel to the file's *new* member list, NullEntity where this instance
        /// removed one — the same shape ExpandedInstance has. A member that survived
        /// the edit appears here with the handle it already had.
        std::vector<ECS::Entity> members;

        /// Members the edit deleted, destroyed by the call (deferred, as ever) and
        /// in slot order so a log or a test reads the same twice.
        ///
        /// The caller needs these for more than tidiness: an undo transaction naming
        /// one of them can no longer be replayed, so the editor truncates its
        /// history against this list.
        std::vector<ECS::Entity> destroyed;
    };

    /// @brief Brings one live instance up to date with its file, **keeping every
    /// surviving member's exact handle**.
    ///
    /// The editor's "you edited the blueprint, now every copy catches up" path. It
    /// has to be a diff rather than destroy-and-recreate, and the reason is undo:
    /// entity handles are `(slot, generation)`, EditHistory stores exact handles, and
    /// Scene::ReviveAt is valid only for a currently-free slot. Rebuild forty cars
    /// behind undo's back and a later Ctrl-Z revives into a slot something else now
    /// occupies.
    ///
    /// Members are matched **by name**, which is why @p previousMemberNames must be
    /// the member list the live tags were written against — read it out of the old
    /// definition *before* invalidating the cache, because that is the only copy. A
    /// name in both lists keeps its entity (stripped and rebuilt in place), a name
    /// only in the old list is destroyed, a name only in the new one is created.
    ///
    /// The instance's own record is untouched: placement, overrides and removals
    /// belong to the level that placed it, not to the file being edited. An override
    /// naming a member the file no longer declares is dropped for this expansion with
    /// a warning but stays in the record — so re-adding that member by hand brings
    /// the override back, rather than having silently discarded somebody's edit.
    ///
    /// **Precondition.** The caller has already taken off whatever engine-side state
    /// serialization does not cover — the Jolt body above all — for every live member
    /// of @p instanceId. This strips reflected components only. Rebuild those from
    /// `members` afterwards.
    ///
    /// @return an error, having changed nothing, if the instance is not live
    ///         (InstanceNotLive) or its file no longer loads (BlueprintUnusable).
    ///         Both are checked before the first member is touched, which is what
    ///         makes "changed nothing" true.
    [[nodiscard]] static std::expected<ReexpandedInstance, LevelError>
    ReexpandInstance(ECS::Scene &scene, InstanceTable &instances, ECS::InstanceId instanceId,
                     std::span<const std::string> previousMemberNames);

    /// @brief Writes @p entities as a standalone file — the "create blueprint from
    /// selection" half of authoring.
    ///
    /// Members are stored around @p origin rather than around wherever they were
    /// standing, so the new file is placeable. An entity whose parent comes along
    /// is already relative to that parent and left alone; every other entity is a
    /// root of the new file and is written as its *world* transform measured from
    /// @p origin — including one whose parent was left behind, whose local offset
    /// names a space the new file does not have.
    ///
    /// A reference pointing *outside* @p entities becomes null, with a warning —
    /// the same rule entity migration follows, and for the same reason: the file
    /// cannot name something it does not contain.
    ///
    /// @return false if the file could not be written, or if the selection
    ///         contains a blueprint member. Wrapping an instance in a new blueprint
    ///         is nesting, which the file expresses as an `instances` entry rather
    ///         than as copied entities — copying them would bake the inner
    ///         blueprint in and stop a fix to it from propagating.
    static bool SaveEntitiesToFile(ECS::Scene &scene, std::span<const ECS::Entity> entities,
                                   const std::filesystem::path &path, const ECS::Transform &origin);

    /// @brief Encodes @p definition's members into codec blocks, once, so every
    /// later spawn of it is a decode rather than a JSON walk (§11).
    ///
    /// Called by Runtime::GetBlueprintDefinition as the last step of building one.
    /// It lives here rather than in Blueprint.cpp because encoding needs live
    /// components, which means deserializing the members into a scratch scene —
    /// and that needs this file's name-resolution context.
    ///
    /// The scratch scene's entities are created in member order, so member *i* is
    /// entity `{i, 0}` and an EntityRef between members encodes as the member index
    /// itself. Spawning then maps those to live entities through the codec's own
    /// reference hook, with nothing to walk afterwards.
    ///
    /// @return false if a member names a reference the file does not declare, which
    ///         makes the whole blueprint unusable rather than quietly mis-wired.
    static bool PrepareBlueprint(BlueprintDefinition &definition);

    /// @brief Write the scene to a JSON file at the given filesystem path.
    ///
    /// @return true on success, false if the file could not be opened or the
    ///         write did not complete.
    static bool SaveToFile(ECS::Scene &scene, const std::filesystem::path &path, const LevelHeader &header = {},
                           InstanceTable *instances = nullptr);

    /// @brief The systems a level names, read **without** loading it.
    ///
    /// Exists because a load that fails after filling the scene cannot be undone.
    /// The editor loads into the world it already has, so by the time an unknown
    /// system name is discovered the previous level is already gone — refusing
    /// then leaves a world holding new content and no systems, which is worse
    /// than either outcome. Checking the names first is the only way to refuse
    /// cleanly.
    ///
    /// @return the names, or why the file could not be read or parsed. An absent
    ///   `systems` array is success with an empty list, which is the normal case.
    [[nodiscard]] static std::expected<std::vector<std::string>, LevelError>
    ReadLevelSystems(std::string_view assetPath);

    /// @brief Load the scene from an asset-relative path via AssetSystem.
    ///
    /// Reads and parses the file before calling Load, so an unreadable or
    /// unparseable one costs the caller nothing.
    ///
    /// @param assetPath Virtual path relative to the asset root (e.g.
    ///        "levels/main.alvl").
    /// @param options   Forwarded to Load; see LoadOptions.
    /// @return Load's own result, or FileUnreadable / MalformedJson for a file
    ///         that never reached it. A throw out of a component's addToScene
    ///         hook is caught here: it clears the half-populated scene and
    ///         reports the clearing as its own, because a throw is the one path
    ///         where Load cannot say so itself.
    [[nodiscard]] static LevelResult LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                                                  const LoadOptions &options = {});

    /// @brief Load the scene from an absolute filesystem path, bypassing the
    /// asset system.
    ///
    /// For levels that are not assets: the temp snapshot a play-in-editor host
    /// writes so its clients can load the *unsaved* scene it is simulating.
    /// Otherwise identical to LoadFromFile, failure handling included.
    [[nodiscard]] static LevelResult LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                                                  const LoadOptions &options = {});

    /// @brief Moves a set of entities' component *data* from one scene to another.
    ///
    /// Entity migration: the entities
    /// in @p entities are serialized out of @p src, recreated in @p dst as fresh
    /// entities, and destroyed in @p src. This is a **third** EntityRef mapping
    /// mode, distinct from the two above:
    ///   - Save/Load remaps against a whole scene and *clears* the destination;
    ///   - the raw-identity context revives at exact handles, which would collide
    ///     with the destination's own entities;
    ///   - this maps a *subset* of one scene onto fresh handles in another,
    ///     leaving the destination otherwise untouched.
    ///
    /// EntityRef fields (e.g. Parent) that point WITHIN the migrated set are
    /// remapped to the new destination entities. A ref that points OUTSIDE the
    /// set — at an entity left behind in @p src — resolves to NullEntity, and
    /// each such dropped ref is logged: it is silent data loss otherwise. Pass a
    /// set closed under the subtree relation (Runtime::GatherSubtree) so a child's
    /// Parent is never the thing left behind.
    ///
    /// Transients are NOT rebuilt here — SceneSerializer links neither Physics nor
    /// the render layer, so a migrated entity's Jolt body and resolved GPU
    /// pointers do not exist in @p dst yet. The caller (App::MigrateEntity) tears
    /// down the source-world body and rebuilds both per destination world.
    ///
    /// @return the created destination entities, parallel to @p entities (so
    /// @p entities[i] became the returned handle at index i). Empty if a
    /// serialization context is already active on this thread.
    static std::vector<ECS::Entity> TransferEntities(ECS::Scene &src, ECS::Scene &dst,
                                                     std::span<const ECS::Entity> entities);

    /// @brief Serialize an EntityRef field's value the way the active context
    /// addresses entities.
    ///
    /// Called only from a component's generated serialize lambda. The JSON *type*
    /// is the mode, which is what lets one pair of functions serve all three:
    ///   - a **string** — the target's name — inside Save/Load of a file;
    ///   - a **number** — an index within the moved set — inside TransferEntities;
    ///   - a **number** — a packed (slot, generation) key — inside a
    ///     ScopedRawEntityContext (editor undo payloads);
    ///   - **null** for NullEntity, for a target outside a Save's scene, or when
    ///     no context is active at all.
    ///
    /// A raw-context payload is therefore still not a valid level file and vice
    /// versa — the asymmetry documented on ScopedRawEntityContext — but the two
    /// can no longer be confused for each other by accident, because a name and
    /// an index are not the same JSON type.
    static nlohmann::json EntityToRef(ECS::Entity entity);

    /// @brief Resolve an EntityRef field's serialized value against the active
    /// context. The inverse of EntityToRef; same three modes.
    ///
    /// Called only from a component's generated addToScene lambda. Returns
    /// NullEntity when the value is null, when no context is active, or when the
    /// reference cannot be resolved. In a *file* load an unresolvable name is not
    /// merely nulled: it is recorded, and Load refuses the whole file once the
    /// pass finishes. The refusal is deferred to there rather than thrown from
    /// here because this runs inside generated code with no idea which file,
    /// entity or component it is speaking for.
    static ECS::Entity RefToEntity(const nlohmann::json &value);

    /// @brief RAII scope that makes EntityRef fields serialize/deserialize against
    /// *raw entity handles* instead of remapped serial indices.
    ///
    /// The generated serialize/deserialize for an `AFIELD() ECS::Entity` field
    /// (e.g. `Parent.parent`) routes through EntityToRef/RefToEntity, which resolve
    /// names only inside Save/Load. Called outside that (e.g. the editor undo/redo
    /// system capturing or restoring a single component's JSON), they resolve to
    /// NullEntity and the field silently collapses — flattening the hierarchy with
    /// no error.
    ///
    /// Within this scope the mapping is identity-preserving instead:
    ///   - EntityToRef(e) returns a packed (slot, generation) key, no remap;
    ///   - RefToEntity(k) returns the live handle at that slot, but only when its
    ///     generation still matches the one packed into `k` — a recycled slot
    ///     resolves to NullEntity rather than to its new occupant.
    /// This is exact only because the paired restore uses Scene::ReviveAt to bring
    /// entities back at their original (index, generation) — EntityAt(index) then
    /// resolves to the same handle that was captured.
    ///
    /// Asymmetry (documented on purpose): level files on disk address entities by
    /// name (stable across a save/load round-trip *and* across an edit that
    /// inserts entities); undo payloads use raw handles (valid only under the
    /// ReviveAt exact-identity guarantee). Do not mix the two — a raw-context
    /// payload is not a valid level file and vice versa.
    ///
    /// Non-reentrant with Save/Load and with itself. Serialization contexts stack;
    /// this one does not stack with anything, because the hooks check it first —
    /// so a raw context and a serialization context are never both live on a
    /// thread.
    class ScopedRawEntityContext
    {
public:
        explicit ScopedRawEntityContext(ECS::Scene &scene);
        ~ScopedRawEntityContext();

        ScopedRawEntityContext(const ScopedRawEntityContext &)            = delete;
        ScopedRawEntityContext &operator=(const ScopedRawEntityContext &) = delete;
        ScopedRawEntityContext(ScopedRawEntityContext &&)                 = delete;
        ScopedRawEntityContext &operator=(ScopedRawEntityContext &&)      = delete;
    };
};

} // namespace Assisi::Runtime