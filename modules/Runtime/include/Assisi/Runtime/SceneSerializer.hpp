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
///   "profile": "Gameplay",
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
/// ## Names, and why v1 is gone
/// Every entity carries a `name`, unique within the file, and every EntityRef
/// field (`Parent::parent` and friends) stores that name rather than a position
/// in the array. Positions are only safe while nothing outside the file points
/// into it, and blueprint overrides point into it by design: an override that
/// says "entity #1" means something different the moment somebody inserts a
/// member above it, and a red wheel silently becomes a red headlight. The same
/// fragility was already latent in `Parent`, where a hand-edited or merged file
/// could re-target a parent link with no error at all.
///
/// The name lives on the entity as Runtime::Name, which is the same field the
/// editor already shows — one name per entity, not a file identity beside a
/// display label. Load fills it in for every entity, so a name is never absent
/// after a round trip.
///
/// There is **no v1 reader**. The four level files in the tree were converted
/// when the format changed (docs/blueprint-system-concept.md §6); carrying a
/// positional path forever would have kept the failure it exists to remove.
///
/// A file is refused outright — the load fails and leaves an empty scene — for a
/// duplicate name, a missing or empty name, a name too long for Runtime::Name,
/// or an EntityRef naming an entity the file does not declare. Each of those is
/// a file that means something other than what it says, and the alternative to
/// refusing is a silently mis-wired scene.
///
/// ## Transient fields
/// Fields marked AFIELD(transient) (e.g. GPU handles, raw pointers) are
/// excluded from serialization.  Components where every field is transient
/// (e.g. MeshRenderer) are saved as an empty object `{}` — their
/// presence on the entity is preserved but no data is restored.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>

namespace Assisi::Runtime
{

/// @brief The parts of a level file that describe the level rather than its
/// entities. Optional in the file; absent fields keep their defaults, so older
/// levels load unchanged.
struct LevelHeader
{
    /// @brief Which system profile the level wants installed into the world it
    /// loads into (docs/world-system-binding-design-notes.md §3). Empty means
    /// "the host's default profile" — the common case, so most levels never
    /// mention it.
    ///
    /// A name, not a system list: systems are C++ functions, so data can only
    /// select among sets the game registered. Keeping the file's vocabulary to a
    /// single name is also what lets the game's system list evolve without
    /// touching level files.
    std::string profile;
};

class SceneSerializer
{
  public:
    /// @brief Optional progress reporter for a load, called with a fraction in
    /// [0, 1] as the component-deserialize pass advances. Invoked on the thread
    /// that drives the load (a worker, for async travel), so an implementation
    /// that publishes to another thread must synchronise itself.
    using ProgressFn = std::function<void(float)>;

    /// @brief Serialize the entire scene to a JSON value, plus @p header.
    ///
    /// The header must be passed by every save that wants to preserve it: a
    /// Scene does not carry it (it is a property of the level, not of the
    /// entities), so a save that omits it strips the field from the file.
    static nlohmann::json Save(ECS::Scene &scene, const LevelHeader &header = {});

    /// @brief Deserialize entities and components from a JSON value into the scene.
    ///
    /// Clears the scene before loading.  Only components registered in
    /// ComponentRegistry are restored; unrecognised names are skipped with a warning.
    /// @p onProgress (optional) is called as the per-entity deserialize pass runs —
    /// the dominant, entity-scaling cost — ending at 1.0.
    /// @p header (optional) receives the file's non-entity metadata; left
    /// untouched if the load fails its version check.
    ///
    /// **Throws** `std::runtime_error` on a wrong `version` or a malformed file
    /// (see the naming rules in the file comment). A version mismatch throws
    /// before the scene is cleared, so a direct caller keeps what it had;
    /// everything else is a file this got partway through, and clears. The
    /// LoadFrom* wrappers below turn every throw into `false` plus an empty scene,
    /// so only a direct caller sees the exception.
    ///
    /// Returning quietly instead is what made a version mismatch read as a
    /// *successful* load of an empty level all the way up to the caller.
    static void Load(ECS::Scene &scene, const nlohmann::json &j, const ProgressFn &onProgress = {},
                     LevelHeader *header = nullptr);

    /// @brief Write the scene to a JSON file at the given filesystem path.
    ///
    /// @return true on success, false if the file could not be opened.
    static bool SaveToFile(ECS::Scene &scene, const std::filesystem::path &path,
                           const LevelHeader &header = {});

    /// @brief Load the scene from an asset-relative path via AssetSystem.
    ///
    /// @param assetPath  Virtual path relative to the asset root (e.g. "levels/main.json").
    /// @param onProgress Optional; forwarded to Load() (see it) for load-progress UI.
    /// @param header     Optional; receives the file's non-entity metadata.
    /// @return true on success, false on any IO or parse error.
    static bool LoadFromFile(ECS::Scene &scene, std::string_view assetPath,
                             const ProgressFn &onProgress = {}, LevelHeader *header = nullptr);

    /// @brief Load the scene from an absolute filesystem path, bypassing the
    /// asset system.
    ///
    /// For levels that are not assets: the temp snapshot a play-in-editor host
    /// writes so its clients can load the *unsaved* scene it is simulating.
    /// Otherwise identical to LoadFromFile, failure handling included.
    static bool LoadFromDisk(ECS::Scene &scene, const std::filesystem::path &path,
                             const ProgressFn &onProgress = {}, LevelHeader *header = nullptr);

    /// @brief Moves a set of entities' component *data* from one scene to another.
    ///
    /// Entity migration (docs/multi-scene-design-notes.md §2, S4): the entities
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
    /// system capturing or restoring a single component's JSON), they return
    /// "unknown" and the field silently collapses to NullEntity — flattening the
    /// hierarchy with no error.
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
    /// Non-reentrant with Save/Load and with itself: exactly one context (remap or
    /// raw) may be active per thread at a time.
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