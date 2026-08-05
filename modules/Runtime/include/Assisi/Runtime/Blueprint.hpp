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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Transform.hpp>

namespace Assisi::Runtime
{

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
    bool parented = false;
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

    /// @p source plus every file reachable from it by instancing, sorted and
    /// deduplicated. Warms the cache (loading `car_with_antenna.abp` warms
    /// `car.abp`, since the closure is walked anyway) and is what a content hash
    /// over a level's real content would cover.
    std::vector<std::string> closure;

    /// Index of @p name in `members`, or nullopt. Linear: member lists are small
    /// and this runs at spawn, not per frame.
    [[nodiscard]] std::optional<uint32_t> IndexOf(std::string_view name) const;
};

/// @brief Parses and flattens @p source, caching the result by virtual path.
///
/// @return nullptr if the file cannot be read or is malformed — a missing nested
///         file, a cycle in the instance graph, a duplicate member name, or a
///         non-uniform instance scale. Every one of those is logged with the file
///         and the member it is about.
///
/// A blueprint is parsed **once**: spawning a hundred bullets must not re-read and
/// re-parse `bullet.abp` a hundred times (§11). Cleared on level unload, never
/// evicted during a level.
const BlueprintDefinition *GetBlueprintDefinition(std::string_view source);

/// @brief Drops every cached definition. Call on level unload, and after editing a
/// blueprint on disk.
void ClearBlueprintCache();

/// @brief Drops one cached definition and everything that instances it.
///
/// The editor's re-expand path, which cannot invalidate only the edited file: a
/// parking lot's flattened member list contains the car's members, so editing the
/// car changes the lot's definition too.
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

    /// Index in the level file's `instances` array for an instance the level
    /// placed, or -1 for one spawned at runtime. What lets a joining client be
    /// told "the level's third instance" rather than being sent its overrides
    /// (§9), and what lets a save write the instance back where it came from.
    int32_t levelInstanceIndex = -1;
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
    uint32_t Add(BlueprintInstance instance);

    /// @brief The row for @p id, or nullptr if no such instance is live.
    [[nodiscard]] const BlueprintInstance *Find(uint32_t id) const;

    /// @brief Drops @p id's row. The members are not touched — destroying them is
    /// the caller's business, and a row outliving its members would be a stored
    /// member list in disguise.
    void Remove(uint32_t id);

    /// @brief Every live instance, in id order. For the editor's outliner and for
    /// a save, which writes one entry per row.
    [[nodiscard]] std::vector<std::pair<uint32_t, const BlueprintInstance *>> All() const;

    /// @brief Empties the table and restarts ids from 1. Level unload.
    void Clear();

    [[nodiscard]] std::size_t Size() const { return _rows.size(); }

  private:
    std::unordered_map<uint32_t, BlueprintInstance> _rows;
    uint32_t                                        _nextId = 1;
};

/// @brief The `instances` array a save should write for a live table, in id order.
///
/// Built from the table rather than kept alongside it, so there is one source of
/// truth for where an instance is: the editor moves an instance by writing the
/// row, and the file follows.
[[nodiscard]] std::vector<LevelInstance> InstancesForSave(const InstanceTable &table);

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

/// @brief Whether @p transform's scale is uniform enough to compose exactly.
[[nodiscard]] bool HasUniformScale(const ECS::Transform &transform);

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
/// A leading `/` on a name means "the file that wrote this", which for a
/// reference authored inside the file *is* that file, so it is stripped and the
/// name prefixed like any other. The two only diverge for an override, where the
/// writing file and the file being addressed are different (§6).
///
/// No-op for an empty prefix, and for any field whose value is not a string.
void QualifyReferences(nlohmann::json &components, std::string_view prefix);

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
