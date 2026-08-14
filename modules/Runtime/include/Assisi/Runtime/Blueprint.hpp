/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Blueprint.hpp
/// @brief Reading a level/blueprint file's instances, flattening them, and the
/// per-world table that says what a live instance is.
///
/// Levels and blueprints are the same format and the same loader; what differs is
/// the verb. Loading a file *replaces* the world; instancing one *adds* to it at a
/// transform. `.alvl` and `.abp` are a hint to humans and something for the asset
/// browser to filter on — the extension never gates behaviour, and instancing a
/// `.alvl` into another `.alvl` works because the loader cannot tell them apart
/// and a distinction enforced weakly is worse than one enforced not at all.
///
/// See docs/blueprint-system-concept.md; the sections are cited where the code
/// implements a decision that would otherwise read as arbitrary.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/InstanceId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>

namespace Assisi::Runtime
{

/// @brief One of a member's components, encoded once so spawning is a decode
/// rather than a JSON walk.
///
/// **It has to be a decode, not a byte copy.** Components are not memcpy-safe —
/// MeshRenderer holds a `std::vector<AssetId>`, whose bytes are a pointer, so
/// copying them into a hundred bullets gives a hundred components sharing one
/// allocation and ninety-nine dangling the moment the first is destroyed.
/// Reflection also offers no generic way to clone a live component. BinaryCodec
/// walks fields rather than copying bytes, is what replication already uses, and
/// needs no new codegen.
struct PreparedComponent
{
    /// Which component this is. The name is kept beside the id because an
    /// instance's overrides address components by name.
    Core::Reflect::ComponentId id = Core::Reflect::kInvalidComponentId;
    std::string name;

    /// A full-state component block, with every EntityRef encoded as the *member
    /// index* it points at rather than a handle. Spawning maps those to the live
    /// entities through the codec's own reference hook, so nothing has to walk the
    /// decoded component afterwards looking for references to patch.
    std::vector<std::byte> block;
};

/// @brief One entry of a blueprint's flattened member list.
///
/// Flattened, because nesting evaporates at runtime: spawning a file that
/// instances `car.abp` and adds an antenna produces **one** instance with one id,
/// whose members are named by path (§4). The alternative — nested instances
/// keeping their own ids — means a tree of instances at runtime and every
/// operation having to ask which level of instance was meant.
struct BlueprintMemberDesc
{
    /// Path name within the instance: `antenna`, `car/body`, `car/wheel_fl`.
    /// Unique across the whole flattened list.
    std::string name;

    /// What to deserialize onto the member, with EntityRef fields already
    /// rewritten from file-local names to instance-local paths.
    nlohmann::json components;

    /// Whether the member carries a Parent. A parented member is positioned
    /// relative to that parent and the instance's placement reaches it through the
    /// chain; a parentless one is in file space, and the placement composes onto
    /// it directly (§3, "the component is absolute").
    ///
    /// Recomputed whenever `components` changes — an override may add a Parent or
    /// null one out, and this decides whether a whole placement lands on the
    /// member's Transform. See ApplyMemberOverride, which owns both.
    bool parented = false;

    /// The accumulated placement of the nesting chain at this member's declaration
    /// site, composed into its Transform above iff `parented` was false at flatten.
    ///
    /// Kept because the decision is reversible: an override applied *after* flatten
    /// can flip `parented`, and undoing or applying the composition needs the exact
    /// transform that was used. It is per member rather than per instance because
    /// members declared at different depths accumulated different chains.
    ECS::Transform placement;

    /// This member's components, encoded once (§11). Spawning decodes these;
    /// `components` above stays the authority for anything an instance overrode,
    /// and for any component the codec cannot round-trip losslessly.
    std::vector<PreparedComponent> prepared;

    /// Components an inner file deliberately deleted from this member with a
    /// `null` override.
    ///
    /// Recorded rather than simply absent, because absent and deleted have
    /// different answers for an *outer* override of the same component: an add
    /// starts from C++ defaults, but resurrecting a deleted component from a field
    /// edit would silently bring back every other field of something somebody
    /// removed on purpose (§5). Removal wins and the outer override is dropped.
    std::vector<std::string> removedComponents;
};

/// @brief A blueprint file, parsed and flattened once.
///
/// The member list is the order NetIds are assigned in (§9), so it must be a pure
/// function of the file's bytes. Nothing here depends on where an instance of it
/// is placed — the placement composes at expansion.
struct BlueprintDefinition
{
    /// Virtual path this was read from.
    std::string source;

    /// Flattened members, outer file's entities first, then each nested instance's
    /// members in the order the instances are listed.
    std::vector<BlueprintMemberDesc> members;

    /// Every system this file and its nested closure name, deduplicated. A spawn
    /// queues these, which is what closes the cross-machine hole in "a component
    /// whose system was never installed just does nothing": a client that receives
    /// a missile *runs the spawn*, so it installs the trail system by construction
    /// rather than by the level having remembered to ask (§8, §9).
    std::vector<std::string> systems;

    /// Members an in-file `removed` took out, by the same flattened path they had
    /// before the erase.
    ///
    /// Kept because a removal must not orphan a reference *fatally*. The name stays
    /// **declared and mapped at nothing** — at preparation, and again under the
    /// instance's own prefix at expansion — so a reference to it nulls with a
    /// warning, the way a per-instance removal's does and the way §6 says. Without
    /// it the name simply ceases to exist, becomes indistinguishable from one the
    /// file never declared, and takes the whole definition down with it.
    ///
    /// A *list*, not holes in `members`: these removals are authored in a file, so
    /// every instance of it has the same member list, and that list is the index
    /// NetIds are assigned from.
    std::vector<std::string> removedMembers;

    /// @p source plus every file reachable from it by instancing, sorted and
    /// deduplicated. Warms the cache (loading `car_with_antenna.abp` warms
    /// `car.abp`, since the closure is walked anyway) and is what a content hash
    /// over a level's real content would cover.
    std::vector<std::string> closure;

    /// Index of @p name in `members`, or nullopt. Linear: member lists are small
    /// and this runs at spawn, not per frame.
    [[nodiscard]] std::optional<uint32_t> IndexOf(std::string_view name) const;
};

/// @brief Why a file could not become a definition.
///
/// Says what *kind* of thing is wrong, not which file or member — the specifics
/// are known only inside the recursive flatten, often several nested files below
/// the one the caller named, so they are logged at the point they are still
/// known. A caller that asked for `lot.abp` learns that an instance cycle exists;
/// the chain that proves it (`lot.abp -> car.abp -> lot.abp`) is in the log.
/// Same split as NameError in Naming.hpp.
enum class BlueprintError : std::uint8_t
{
    FileUnreadable,    ///< The asset system could not read the file, or one it instances.
    MalformedJson,     ///< Read, but not parseable as JSON.
    UnsupportedVersion,///< A `version` this build does not read.
    MissingName,       ///< An entity or instance entry with no usable `name`.
    MissingSource,     ///< An instance entry that names no `source`.
    InstanceCycle,     ///< A file reachable from itself by instancing; expands forever.
    DuplicateMember,   ///< Two members would flatten to one name, making references ambiguous.
    NonUniformScale,   ///< An instance placement that shears; cannot compose exactly (§3).
    ComponentRejected, ///< A member's component values the reflection layer refuses.
};

/// @brief One line saying what is wrong, for a log or a field hint.
[[nodiscard]] std::string_view Describe(BlueprintError error);

/// @brief A definition, or why the file could not become one. The success value is
/// never null.
using BlueprintResult = std::expected<std::shared_ptr<const BlueprintDefinition>, BlueprintError>;

/// @brief Parses and flattens @p source, caching the result by virtual path.
///
/// @return the definition, or the reason it could not be built. **Never throws**:
///         callers as ordinary as the editor's Save reach this while walking a
///         scene, and a level the user cannot save is a worse failure than a level
///         with one broken blueprint in it. The specific file and member are
///         already logged by the time this returns — a caller that only needs to
///         know whether it worked can ask `.has_value()` without logging again.
///
/// A blueprint is parsed **once**: spawning a hundred bullets must not re-read and
/// re-parse `bullet.abp` a hundred times (§11). Cleared on level unload, never
/// evicted during a level. A failure is **not** cached: a blueprint that failed
/// because its nested file was missing is readable the moment somebody adds it.
///
/// **Safe to call from any thread.** Async travel deserializes on a worker and
/// stages instances there, while the editor asks for the same definitions per
/// frame on the main thread. The cache is synchronised, and the returned
/// definition is shared ownership rather than a borrow — it stays alive and
/// unchanged for as long as the caller holds it, even across a
/// ClearBlueprintCache() or an InvalidateBlueprint() from another thread. A
/// definition is immutable once built, so holding one across an eviction gets the
/// content it was built with, not a torn read of the next one.
///
/// The success value is never null.
[[nodiscard]] BlueprintResult GetBlueprintDefinition(std::string_view source);

/// @brief Drops every cached definition. Call on level unload, and after editing a
/// blueprint on disk.
///
/// Drops the *cache's* claim on them. Anything still holding a definition keeps
/// it; the memory goes when the last holder lets go.
void ClearBlueprintCache();

/// @brief Drops one cached definition and everything that instances it.
///
/// The editor's re-expand path, which cannot invalidate only the edited file: a
/// parking lot's flattened member list contains the car's members, so editing the
/// car changes the lot's definition too.
///
/// Drops the cache's claim, as ClearBlueprintCache does — the next ask rebuilds
/// from disk, and a caller mid-way through the old one is not cut off.
void InvalidateBlueprint(std::string_view source);

/// @brief One entry of a file's `instances` array — a source, a placement, and
/// (from §5 onward) what this instance changed about it.
///
/// This is the whole per-instance authoring vocabulary. A level lists which file
/// and what that instance changed, and nothing else: levels stay small, a fix
/// propagates on next load, and the behaviour a piece of content needs travels
/// with it.
struct LevelInstance
{
    /// Unique within the file, and the prefix its members are addressed by:
    /// instance `car_3` of a file declaring `body` contributes `car_3/body`.
    std::string name;

    /// Virtual path of the file to instance.
    std::string source;

    /// Where to put it. Only translation, rotation and *uniform* scale — a
    /// non-uniform scale here fails the load rather than being clamped (§3).
    ECS::Transform transform;

    /// What this instance changed: `{ memberPath: { ComponentName: {...} | null } }`.
    ///
    /// **Recorded, not computed.** An override exists because somebody edited that
    /// field, not because a comparison found a difference — a computed diff freezes
    /// the old values into every instance as fake overrides the moment the blueprint
    /// changes. A field nobody touched re-reads from the source on every load, which
    /// is what makes "fix it once, fixed everywhere" true and is the whole point of
    /// the format.
    ///
    /// Addresses downward and never upward: a level placing a lot may override
    /// `car_3/wheel_fl/…`. `null` reads unambiguously as "this instance does not
    /// have that component", since a real component is always an object.
    nlohmann::json overrides = nlohmann::json::object();

    /// Member paths this instance does not have. Removing a wheel from the third
    /// car of a placed lot is `["car_3/wheel_fl"]` — the same addressing overrides
    /// use, rather than a second scheme. A path also removes everything beneath
    /// it, so a whole nested instance can go.
    ///
    /// **Instances only shrink.** There is no way to add a member, because an
    /// instance's members must always be a subset of what its file declares —
    /// that invariant is what makes validating an instance, generating a typed
    /// view from the file alone, and dropping orphaned overrides safely possible.
    std::vector<std::string> removed;
};

/// @brief One row of the world's instance table.
struct BlueprintInstance
{
    /// The name the placing file gave it, and the prefix its members were
    /// addressed under. Empty for a runtime spawn, which no file names.
    std::string name;

    /// The file this instance is of. Without it, FindInstance<Car>(world, 7) would
    /// build a Car view over a crate's members and return nonsense (§2).
    std::string source;

    /// The placement. Held here because the editor's record needs it anyway, and
    /// because an override is recorded relative to it (§3).
    ECS::Transform transform;

    /// Whether this instance is **level content** — placed by an author, and
    /// written back when the level saves.
    ///
    /// Distinct from `levelInstanceIndex` because an instance the author places in
    /// the editor is authored but has no index yet; it gets one the next time the
    /// file is written. Deriving "should this be saved?" from the index instead
    /// drops every freshly placed instance on the first save.
    bool authored = false;

    /// Index in the level file's `instances` array for an instance the level
    /// placed, or -1 for one that was never in a file — a runtime spawn, or one
    /// the author placed since the last load. What lets a joining client be told
    /// "the level's third instance" rather than being sent its overrides (§9).
    int32_t levelInstanceIndex = -1;

    /// What this instance changed, and which members it does not have — see
    /// LevelInstance. Held so a save writes back exactly what was read, and so the
    /// editor has a record to add to. There are no *runtime* overrides: a caller
    /// that wants a red car writes the component after spawning, which is typed,
    /// direct, and expresses things overrides cannot (§5).
    nlohmann::json overrides = nlohmann::json::object();
    std::vector<std::string> removed;
};

/// @brief The world's instance table: one row per live instance, never per member.
///
/// It exists because the tag says *which* instance an entity belongs to and never
/// *what* that instance is. Not on the entities, not serialized, not replicated.
///
/// **Ids are per-world**, handed out from 1 upward, and start over when a level
/// loads, because the table is discarded with the world. They are not stable
/// across a save/load and nothing may assume they are.
class InstanceTable
{
public:
    /// @brief Adds a row and returns its fresh id (never 0).
    ECS::InstanceId Add(BlueprintInstance instance);

    /// @brief The row for @p id, or nullptr if no such instance is live.
    [[nodiscard]] const BlueprintInstance *Find(ECS::InstanceId id) const;

    /// @brief Drops @p id's row. The members are not touched — destroying them is
    /// the caller's business, and a row outliving its members would be a stored
    /// member list in disguise.
    void Remove(ECS::InstanceId id);

    /// @brief Puts a row back at an exact id, for undo.
    ///
    /// The editor's history stores the id it recorded against, and an undo has to
    /// land on that same id or every BlueprintMember tag pointing at it becomes an
    /// orphan. Add() cannot do this — it hands out the next free id, which after a
    /// delete-then-undo is a different one.
    ///
    /// Also advances the allocator past @p id, so a later Add cannot collide with a
    /// row that was restored from under it.
    void RestoreAt(ECS::InstanceId id, BlueprintInstance instance);

    /// @brief Records that @p id's row was written at @p index of the level file's
    /// instance array.
    ///
    /// Only a save knows this: an editor placement is authored the moment it is made
    /// and has no position until a file has one to give it, and every removal since
    /// the last save shifts the entries after it. A row still naming its old entry
    /// names another instance's.
    void SetLevelInstanceIndex(ECS::InstanceId id, int32_t index);

    /// @brief Every live instance, in id order. For the editor's outliner and for
    /// a save, which writes one entry per row.
    [[nodiscard]] std::vector<std::pair<ECS::InstanceId, const BlueprintInstance *>> All() const;

    /// @brief Empties the table and restarts ids from 1. Level unload.
    void Clear();

    [[nodiscard]] std::size_t Size() const { return _rows.size(); }

private:
    std::unordered_map<ECS::InstanceId, BlueprintInstance> _rows;
    /// The next id to hand out. A raw counter rather than an InstanceId because
    /// this is the one place that does arithmetic on the number — which is exactly
    /// what the type exists to forbid everywhere else.
    uint32_t _nextId = 1;
};

/// @brief Every live member of @p instanceId, by scanning the tag pool.
///
/// **"The members of instance 7" is a query**, computed when asked and discarded.
/// A member already destroyed simply is not found; there is no list to go stale.
/// That is the difference from a stored member list, which needs invalidating
/// every time a member dies or is reparented and gets it wrong once.
[[nodiscard]] std::vector<ECS::Entity> MembersOf(ECS::Scene &scene, ECS::InstanceId instanceId);

/// @brief One member leaves its instance; the entity lives on.
///
/// Removes the tag and nothing else. A partial instance is not a broken state,
/// because membership is a query — which is also why ExplodeInstance below does
/// not need to be all-or-nothing.
///
/// @return false if @p entity was not a member of anything.
bool PruneFromInstance(ECS::Scene &scene, ECS::Entity entity);

/// @brief The entity for member @p name of instance @p id, or NullEntity.
///
/// Resolves the name to an index once through the cached member list, then
/// compares integers per entity — which is why the tag carries an index rather
/// than a string.
[[nodiscard]] ECS::Entity FindMember(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId instanceId,
                                     std::string_view name);

/// @brief The row for @p id, optionally confirming it came from @p expectedSource.
///
/// The source check is why the table has to exist: without it a typed view over
/// instance 7 would be built over a crate's members and return nonsense (§2).
///
/// @return nullptr if no such instance is live, or if it is not from
///         @p expectedSource (when one is given).
[[nodiscard]] const BlueprintInstance *FindInstance(const InstanceTable &table, ECS::InstanceId instanceId,
                                                    std::string_view expectedSource = {});

/// @brief @p stem, or the first `stem_N` no live instance in @p table is using.
///
/// An instance's name is the prefix its members are addressed by, so two live
/// instances of one name mean two entities answering to `car/body` — which the
/// loader refuses outright (`nameToEntity` claims each path once). A level saved
/// with both is a level that never opens again.
///
/// This is the polite half of that rule: the editor calls it so an author who
/// places a second car gets `car_1` instead of a refusal. The refusal itself
/// lives in `SceneSerializer::PlaceInstance` — a rule enforced only by the
/// callers who remember it is a rule one gesture will skip (round-7 S17).
///
/// Unnamed instances are not considered and never collide — see PlaceInstance.
[[nodiscard]] std::string UniqueInstanceName(const InstanceTable &table, std::string_view stem);

/// @brief The `instances` array a save should write for a live table, in id order.
///
/// Built from the table rather than kept alongside it, so there is one source of
/// truth for where an instance is: the editor moves an instance by writing the
/// row, and the file follows.
[[nodiscard]] std::vector<LevelInstance> InstancesForSave(InstanceTable &table);

/// @brief Composes @p placement onto @p local, the way an instance's root reaches
/// a member.
///
/// Exact only because an instance's scale is constrained to be uniform (§3):
/// uniform scale commutes with rotation, so the product decomposes cleanly back to
/// TRS. A non-uniform scale anywhere in the chain introduces shear, and the result
/// cannot be represented as a Transform at all — which is why a file carrying one
/// fails the load rather than being clamped.
///
/// **One function, called by both sides.** Client expansion has to agree with host
/// expansion field for field, because the first snapshot is a delta against the
/// blueprint; two spellings of this that differ in the low bits are a silent
/// cross-build desync (docs/blueprint-implementation-plan.md §5, risk 1).
[[nodiscard]] ECS::Transform ComposeTransform(const ECS::Transform &placement, const ECS::Transform &local);

/// @brief The exact inverse: what @p local would have to be for
/// `ComposeTransform(placement, local)` to equal @p world.
///
/// This is how an override is recorded relative to its instance, and how "create
/// a blueprint from this selection" writes members around the new file's own
/// origin rather than around wherever they happened to be standing. Kept beside
/// its forward form on purpose — the two must agree to the bit, and they are the
/// pair a cross-build desync would come from.
[[nodiscard]] ECS::Transform InverseComposeTransform(const ECS::Transform &placement,
                                                     const ECS::Transform &world);

/// @brief Whether @p transform's scale is uniform enough to compose exactly.
[[nodiscard]] bool HasUniformScale(const ECS::Transform &transform);

/// @brief The origin a selection is authored around when it becomes a blueprint:
/// @p root's position and rotation, with unit scale.
///
/// Scale is left out on purpose, and that asymmetry is the whole point of this
/// existing as a named thing rather than as `*transform` at the call site.
/// Passing the full transform as the origin divides the members' scale out into
/// the placement: the copy standing in front of the author looks right (its
/// placement carries the scale back), but the file holds a unit-size thing, so
/// every *fresh* instance comes back the wrong size. A cube scaled to 0.6 and
/// saved as `small_crate` is a small crate — where it stands and which way it
/// faces is placement, how big it is is what it is.
///
/// An instance may still be scaled after the fact; that multiplies on top, the
/// same as any other placement.
[[nodiscard]] ECS::Transform AuthoringOrigin(const ECS::Transform &root);

/// @brief The origin @p entities is authored around: the AuthoringOrigin of the
/// first of them, or the identity if that one carries no Transform.
///
/// Takes the set rather than one entity so the anchor cannot come from outside
/// the file being written. The editor's gesture drops selected entities that are
/// dead or not editable, and anchoring on the raw selection lets a dropped one
/// supply the origin: every member then goes into the file measured from a pose
/// no member has, and the copy that replaces them stands off by the difference
/// (round-7 S16). The front is the anchor because the *first* entity selected is
/// a stable choice and the last is not — an author Ctrl-clicking three more
/// things should not move the origin.
[[nodiscard]] ECS::Transform AuthoringOriginFor(const ECS::Scene &scene,
                                                std::span<const ECS::Entity> entities);

/// @brief Rewrites every reflected EntityRef field in @p components by prepending
/// @p prefix to the name it holds.
///
/// A file names its own entities: `car.abp` says a wheel's parent is `body`. Once
/// that file is flattened into a lot, or placed as `car_3`, the member is called
/// `car_1/body` or `car_3/body`, and the unqualified name either resolves to
/// nothing or — worse, with two instances of the same file — to whichever one
/// answered first. Applied once per nesting level at flatten time and once more
/// for the instance's own name at expansion.
///
/// **One rule, two namespaces (§6).** A name with no leading `/` resolves in the
/// namespace of the thing being addressed; a leading `/` resolves in the namespace
/// of the file that *wrote the text*. For a reference authored inside its own file
/// those are the same namespace, which is why ordinary files never need the slash.
/// They diverge for an override, where writer and target are different files —
/// QualifyOverrideReferences is that case, and the only one.
///
/// **The invariant the rest of the system rests on:** after flatten, a reference
/// carries a leading `/` *if and only if* it is level-scoped. Everything a
/// blueprint could resolve — its own entities' references, and any override it
/// writes on a nested instance — is fully resolved into definition space with no
/// slash left. That is why this runs even at an empty prefix, where all it does is
/// strip the slash: without that strip a top-level file's own `/body` reaches
/// expansion looking exactly like a level-scoped one and meaning the opposite.
///
/// No-op for any field whose value is not a string.
void QualifyReferences(nlohmann::json &components, std::string_view prefix);

/// @brief Resolves an override's references, where two namespaces are both live.
///
/// A plain name addresses downward into the instance being overridden
/// (@p targetPrefix); a leading `/` addresses the file that wrote the claim
/// (@p writerPrefix). Both come out fully qualified into definition space, so the
/// invariant above holds and nothing downstream needs to know which form was used.
///
/// Called where both prefixes are still in scope — one step later the writing
/// file's identity is gone and the two cases are indistinguishable.
void QualifyOverrideReferences(nlohmann::json &componentOverrides, std::string_view writerPrefix,
                               std::string_view targetPrefix);

/// @brief Resolves a definition's references against the instance being expanded.
///
/// The other side of the invariant: by expansion time every reference a blueprint
/// could resolve already is, so a surviving leading `/` can only be level-scoped —
/// an entity of the file that placed this instance. It keeps its bare name and
/// does *not* take the instance prefix. Everything else addresses downward and does.
void QualifyInstanceReferences(nlohmann::json &components, std::string_view prefix);

/// @brief Merges one member's component overrides into its description.
///
/// The merge lattice, in one place because it is the part of the format most
/// easily decided differently at two keyboards:
///
///   - **Outermost wins, per field.** A level's override beats the blueprint's for
///     the same field only, so a lot setting a wheel's colour and a level setting
///     its radius both apply. The alternative reading — an outer claim replacing
///     the whole inner set — is how someone loses edits.
///   - **`null` removes the component**, and a removal already recorded beats an
///     outer field override, with a warning naming the member and the component.
///     Neither claim can be honoured and the engine cannot know which is the
///     mistake, so the load succeeds, the component stays gone, and a human
///     decides.
///   - **An add starts from C++ defaults**, never from what the blueprint had
///     before an inner level removed it — a re-add is a new component that happens
///     to share a name. This falls out for free: a deserialize starts from a
///     value-initialised component and writes only the fields present.
///   - **Two adds merge per field**, outermost winning, exactly as two field
///     overrides do. An add is an object claim like any other.
///
/// @p context names the instance for the warning, e.g. `car_3/wheel_fl`.
void ApplyMemberOverride(BlueprintMemberDesc &member, const nlohmann::json &componentOverrides,
                         std::string_view context);

/// @brief Whether @p memberName is covered by @p removed — named exactly, or
/// beneath a removed path.
///
/// The prefix rule is what lets a whole nested instance be removed: `car_3`
/// removes `car_3/body` too, and nothing has to know whether the path named a
/// member or an instance.
[[nodiscard]] bool IsMemberRemoved(std::string_view memberName, const std::vector<std::string> &removed);

/// @brief Reads an instance placement. Absent fields keep their defaults, so
/// `{"position": [0,4,0]}` is a legal transform.
///
/// The same field spellings the reflected Transform serializes with — rotation is
/// `[w, x, y, z]` — because a placement and a member's own transform have to be
/// readable the same way by a human editing the file.
[[nodiscard]] ECS::Transform TransformFromJson(const nlohmann::json &value);

/// @brief Writes an instance placement. The inverse of TransformFromJson.
[[nodiscard]] nlohmann::json TransformToJson(const ECS::Transform &transform);

} // namespace Assisi::Runtime
