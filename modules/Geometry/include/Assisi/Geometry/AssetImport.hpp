/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetImport.hpp
/// @brief Editor-side import pass: explode a glTF's materials into `.amat` files.
///
/// The reconcile pass (Core `AssetDatabase`) gives every file a GUID sidecar but
/// cannot reach into a glTF — it lives in Core, below Geometry. This is the step
/// that needs the importer: for a glTF whose materials have not been materialized
/// yet, it runs the importer, writes one `<model>_<name>.amat` (+ its `.aast`
/// sidecar, with a freshly minted GUID) per material next to the model, and
/// records the `slot → material GUID` binding into the glTF's own `.aast`
/// manifest. After this pass the mesh→material default is a *stored fact* the
/// database reads back, instead of being re-derived from the glTF every load.
///
/// Editor-only tooling. It lives in Geometry (not Core) because it needs the
/// glTF importer and `.amat` serializer; when a dedicated editor/tools `Assets`
/// module is stood up (doc §7) it moves there alongside the AssetDatabase.
/// Reconcile-not-clobber governs it: a glTF that already carries a manifest, or
/// a `.amat` that already exists on disk, is left untouched.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp> // AssetIdResolver, MeshImportError

namespace Assisi::Geometry
{

// --- Authoring a `.amat` by hand -------------------------------------------
//
// The passes above materialize materials *from a glTF*. These two back the
// editor's material panel, where the author is the source: they write a body and
// nothing else. Sidecars stay the reconcile pass's job — an authored material
// gets its GUID from the next database rebuild, exactly as any other new file
// does, so there is one place that mints ids rather than two.

/// @brief Why writing an authored `.amat` failed.
enum class MaterialWriteError : std::uint8_t
{
    SerializeFailed,  ///< MaterialData's reflection isn't linked, so there is nothing to write.
    PathUnresolvable, ///< The virtual path is malformed, or escapes the asset root.
    WriteFailed,      ///< The bytes could not be written.
    TargetExists,     ///< A rename's destination is taken; nothing was moved.
};

std::string_view ToString(MaterialWriteError error) noexcept;

/// @brief Write @p material to @p virtualPath under the asset root, replacing any
///        existing body.
///
/// Deliberately a clobber: the author edited *this* material and asked to save
/// it, so reconcile-not-clobber does not apply. The `.aast` beside it is never
/// read or written, which is what preserves the asset's identity across a save —
/// every level referencing it holds that GUID and nothing else.
///
/// The body is also mirrored into the authoring root when one is configured
/// (dev builds — see AssetSystem::SetAuthoringRoot). Without that an authored
/// material would live only in the staged asset copy, which the next clean build
/// deletes. This differs from the sidecar mirror, which declines to write a file
/// the durable tree does not already have: a sidecar may describe a build
/// artifact, whereas an authored `.amat` *is* source.
[[nodiscard]] std::expected<void, MaterialWriteError> SaveMaterial(std::string_view virtualPath,
                                                                   const MaterialData &material);

/// @brief A free `<dirVirtualPath>/<stem>.amat`, suffixing `_1`, `_2`, … until
///        nothing is at that path.
///
/// For the New / Duplicate actions, so neither silently overwrites a material.
/// An empty @p dirVirtualPath names the asset root. Advisory rather than a
/// reservation — nothing holds the name between this call and the write — which
/// is sound here because the editor is the only writer and is single-threaded.
[[nodiscard]] std::string UniqueMaterialPath(std::string_view dirVirtualPath, std::string_view stem);

/// @brief Move a material from @p oldVirtualPath to @p newVirtualPath, taking
///        its `.aast` sidecar with it.
///
/// A material has no name of its own — `MaterialData::Name` never enters the
/// file — so renaming one *is* moving its file, and the sidecar has to travel or
/// the GUID is lost and every reference to the material stops resolving. That
/// the id survives a move is the property AssetDatabase::Rebuild is built on;
/// this is the operation that has to honour it.
///
/// Refuses rather than clobbers when the destination exists: overwriting would
/// destroy an unrelated material to answer "that name is taken". On a partial
/// failure the payload is moved back, so a rename either happens or does not.
/// The authoring-root copy moves too, or the old name returns on the next build.
[[nodiscard]] std::expected<void, MaterialWriteError> RenameMaterial(std::string_view oldVirtualPath,
                                                                     std::string_view newVirtualPath);

/// @brief Delete a material and its `.aast` sidecar, in the asset root and in
///        the authoring root. Returns whether the material itself was removed.
///
/// References to it are deliberately not chased down: a mesh slot holding the
/// dead GUID falls back to the default material and keeps the id, so restoring
/// the file from version control restores the binding.
bool DeleteMaterialFile(std::string_view virtualPath);

/// @brief Explode @p gltfVirtualPath's materials into sibling `.amat` files and
///        write its `.aast` slot→material manifest.
///
/// The caller runs this only for a glTF that has no manifest yet (the reconcile
/// pass having already minted its sidecar). Texture channels in the written
/// `.amat`s are resolved to GUIDs through @p resolveTextureId (the editor backs
/// it with its AssetDatabase); an unresolved or embedded texture writes nil
/// (factor-only), exactly as importing the mesh would.
///
/// @param gltfVirtualPath Asset-relative path to the `.gltf`/`.glb`.
/// @param resolveTextureId Maps a sibling texture path to its GUID for the
///        written material channels.
/// @return The number of material slots written into the manifest, or a
///         MeshImportError if the glTF could not be imported or its sidecar
///         (which must already exist) could not be read.
[[nodiscard]] std::expected<std::size_t, MeshImportError>
ExplodeGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId);

/// @brief What a reconcile of an already-exploded glTF against its current
///        source concluded (S4 / D5, conservative classifier).
enum class ReconcileOutcome : std::uint8_t
{
    UpToDate,      ///< Stored source hash matches — nothing changed, no work.
    Stamped,       ///< First S4 sighting: source hash recorded; materials current.
    GeometryOnly,  ///< Source changed but every material is identical — hash refreshed.
    AdditiveSlots, ///< New slots appended (existing unchanged) — default `.amat`s written.
    ConflictStale, ///< Not provably safe (slot removed/reordered, or a material differs)
                   ///< — left untouched and still stale; needs manual resolution.
    Failed,        ///< The glTF or an existing `.amat` could not be read/imported.
};

/// @brief The result of ReconcileGltfMaterials.
struct ReconcileResult
{
    ReconcileOutcome outcome     = ReconcileOutcome::Failed;
    std::size_t addedSlots  = 0;          ///< Slots newly materialized (AdditiveSlots).
    bool changedDisk = false;             ///< Whether any file was written (caller rescans).
};

/// @brief Reconcile an already-exploded glTF (one carrying a manifest) against
///        its current source, per the conservative S4/D5 policy.
///
/// Recomputes the glTF's content hash and compares it to the stamped one. If it
/// matches → UpToDate. If the composite predates S4 (no stamp) → the hash is
/// recorded and it is treated as current (Stamped). On a mismatch the new
/// material table is compared, slot-by-slot, against the existing `.amat`s:
///   - all materials identical → **GeometryOnly**, hash refreshed;
///   - new slots appended, existing unchanged → **AdditiveSlots**, a default
///     `.amat` (+ sidecar) written for each and the manifest extended;
///   - anything else (slot removed/reordered, a material's fields differ, or an
///     existing `.amat` is unreadable) → **ConflictStale**: nothing is written,
///     so the mismatch keeps being detected until resolved by hand.
///
/// Existing `.amat` files are never overwritten or deleted — the guarantee that
/// makes silent auto-reconcile safe. Manual Reimport re-runs this pass.
///
/// @param resolveTextureId Path→GUID for importing the fresh material table.
/// @param resolveMaterialPath GUID→virtual-path, to load each existing `.amat`
///        named in the manifest for the comparison.
[[nodiscard]] ReconcileResult
ReconcileGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                       const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath);

// --- Prompt-driven conflict resolution (S4 second half / D5) ---------------
//
// ReconcileGltfMaterials auto-resolves only provably-safe diffs and leaves
// everything else ConflictStale. These three functions back the user-facing
// resolution the author reaches for a stale asset: DiffGltfMaterials describes
// *what* conflicts (to render a prompt), and RegenerateGltfMaterials /
// AcceptGltfSource *apply* the author's decision. Both write paths are gated on
// an explicit user choice, so RegenerateGltfMaterials is allowed to overwrite
// authored `.amat` bodies — the one place the reconcile-not-clobber default is
// deliberately lifted.

/// @brief How one material slot changed between an exploded glTF's stored
///        `.amat`s and its current source.
enum class SlotChange : std::uint8_t
{
    Unchanged, ///< The source slot matches the stored `.amat` field-for-field.
    Changed,   ///< The source slot differs from the stored `.amat` (a conflict).
    Added,     ///< A new source slot with no stored `.amat` yet.
    Removed,   ///< A stored slot the source no longer has (a conflict).
};

/// @brief One row of a MaterialDiff: a slot and how it changed.
struct SlotDiff
{
    std::uint32_t slot   = 0;
    SlotChange change = SlotChange::Unchanged;
    std::string name;               ///< Source material name (empty for a Removed slot; may be empty if unnamed).
    Core::AssetId existing;         ///< The stored `.amat` GUID for this slot (nil for Added).
};

/// @brief The per-slot classification of a stale glTF against its stored
///        materials — the data a resolution prompt renders. Slot-ordered over
///        the union of old and new slots.
struct MaterialDiff
{
    bool valid = false;                  ///< False if the glTF/sidecar/import could not be read.
    std::vector<SlotDiff> slots;

    /// @brief Whether any slot is a Changed or Removed conflict. (Added alone is
    /// the additive-safe case ReconcileGltfMaterials already handles, so a mesh
    /// left stale always has a conflict; still useful to render the detail.)
    [[nodiscard]] bool HasConflict() const;
};

/// @brief Classify @p gltfVirtualPath's current source against its stored
///        `.amat` manifest, slot by slot. Reads the same inputs the reconciler's
///        conflict path does, but returns per-slot detail for a prompt instead of
///        a single verdict. @p valid is false if any read/import failed.
[[nodiscard]] MaterialDiff
DiffGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                  const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath);

/// @brief Apply the author's "regenerate from source" choice: re-import the glTF
///        and overwrite the manifested `.amat` bodies with the fresh table,
///        **preserving each surviving slot's GUID** (so references and the
///        manifest stay valid), minting a file for any appended slot. Slots the
///        source dropped fall out of the rewritten manifest; their orphaned
///        `.amat` files are left on disk (never deleted). The source hash is
///        refreshed, clearing the stale state.
///
/// This is the authorized-clobber path — it exists precisely to discard
/// hand-edits the author chose not to keep. @p resolveMaterialPath maps a stored
/// GUID to the `.amat` path to overwrite.
///
/// @return The new manifest slot count, or nullopt if the glTF/sidecar could not
///         be read or imported (nothing is written in that case).
[[nodiscard]] std::optional<std::size_t>
RegenerateGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                        const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath);

/// @brief Apply the author's "keep my materials" choice: accept the current
///        source by re-stamping its hash into the glTF sidecar, leaving every
///        `.amat` and the manifest untouched. Clears the stale state without
///        changing any authored material. Returns false on a read/write failure.
[[nodiscard]] bool AcceptGltfSource(std::string_view gltfVirtualPath);

} /* namespace Assisi::Geometry */
