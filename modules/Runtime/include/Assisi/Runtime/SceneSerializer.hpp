/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneSerializer.hpp
/// @brief JSON-based level file save/load for an ECS Scene.
///
/// All reflected (non-transient) component fields are persisted automatically
/// via ComponentRegistry.  Unrecognised component names in a file are skipped
/// with a warning so old levels remain loadable after component renames.
///
/// ## Format (version 1)
/// @code{.json}
/// {
///   "version": 1,
///   "entities": [
///     {
///       "components": {
///         "Transform": { "position": [0,0,0], "rotation": [1,0,0,0], "scale": [1,1,1] },
///         "PointLight": { "color": [1,1,1], "intensity": 100.0, "radius": 20.0 }
///       }
///     }
///   ]
/// }
/// @endcode
///
/// Entity IDs are not persisted; loading always clears the scene first and
/// allocates fresh sequential entities so generation numbers stay at zero.
///
/// ## Transient fields
/// Fields marked AFIELD(transient) (e.g. GPU handles, raw pointers) are
/// excluded from serialization.  Components where every field is transient
/// (e.g. MeshRenderer) are saved as an empty object `{}` — their
/// presence on the entity is preserved but no data is restored.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>

namespace Assisi::Runtime
{

class SceneSerializer
{
  public:
    /// @brief Serialize the entire scene to a JSON value.
    static nlohmann::json Save(ECS::Scene &scene);

    /// @brief Deserialize entities and components from a JSON value into the scene.
    ///
    /// Clears the scene before loading.  Only components registered in
    /// ComponentRegistry are restored; unrecognised names are skipped with a warning.
    static void Load(ECS::Scene &scene, const nlohmann::json &j);

    /// @brief Write the scene to a JSON file at the given filesystem path.
    ///
    /// @return true on success, false if the file could not be opened.
    static bool SaveToFile(ECS::Scene &scene, const std::filesystem::path &path);

    /// @brief Load the scene from an asset-relative path via AssetSystem.
    ///
    /// @param assetPath  Virtual path relative to the asset root (e.g. "levels/main.json").
    /// @return true on success, false on any IO or parse error.
    static bool LoadFromFile(ECS::Scene &scene, std::string_view assetPath);

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

    /// @brief Map a live entity to its stable serial index during the current Save.
    ///
    /// Only valid to call from within a component's serialize lambda.
    /// Returns nullopt if the entity is unknown or no save is in progress.
    static std::optional<uint64_t> EntityToIndex(ECS::Entity entity);

    /// @brief Map a serial index to the live entity created during the current Load.
    ///
    /// Only valid to call from within a component's addToScene lambda.
    /// Returns NullEntity if the index is out of range or no load is in progress.
    static ECS::Entity IndexToEntity(uint64_t index);

    /// @brief RAII scope that makes EntityRef fields serialize/deserialize against
    /// *raw entity handles* instead of remapped serial indices.
    ///
    /// The generated serialize/deserialize for an `AFIELD() ECS::Entity` field
    /// (e.g. `Parent.parent`) routes through EntityToIndex/IndexToEntity, which are
    /// engaged only inside Save/Load. Called outside that (e.g. the editor
    /// undo/redo system capturing or restoring a single component's JSON), they
    /// return "unknown" and the field silently collapses to NullEntity — flattening
    /// the hierarchy with no error.
    ///
    /// Within this scope the mapping is identity-preserving instead:
    ///   - EntityToIndex(e) returns a packed (slot, generation) key, no remap;
    ///   - IndexToEntity(k) returns the live handle at that slot, but only when its
///     generation still matches the one packed into `k` — a recycled slot
///     resolves to NullEntity rather than to its new occupant.
    /// This is exact only because the paired restore uses Scene::ReviveAt to bring
    /// entities back at their original (index, generation) — EntityAt(index) then
    /// resolves to the same handle that was captured.
    ///
    /// Asymmetry (documented on purpose): level files on disk use remapped serial
    /// indices (dense, stable across a save/load round-trip); undo payloads use raw
    /// handles (valid only under the ReviveAt exact-identity guarantee). Do not mix
    /// the two — a raw-context payload is not a valid level file and vice versa.
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