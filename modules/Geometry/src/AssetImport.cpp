/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AssetImport.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Assisi/Core/AssetSidecar.hpp> // AssetSidecar, MintAssetId, (De)SerializeSidecar
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/ContentHash.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/MaterialFile.hpp> // (De)SerializeMaterial
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{
namespace
{
namespace fs = std::filesystem;

/// @brief Read a whole file into a string via absolute path. nullopt on failure.
std::optional<std::string> ReadWholeFile(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// @brief Write text to an absolute path, truncating. Returns false on failure.
bool WriteWholeFile(const fs::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return false;
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

/// @brief The `.aast` path for a payload file ("model.gltf" -> "model.gltf.aast").
fs::path SidecarPathOf(const fs::path &payload)
{
    fs::path sidecar = payload;
    sidecar += ".aast";
    return sidecar;
}

/// @brief Turn a glTF material name into a safe filename component. Non
///        `[A-Za-z0-9._-]` bytes become '_'; an empty name becomes "material".
std::string SanitizeName(std::string_view name)
{
    std::string safe;
    safe.reserve(name.size());
    for (const char c : name)
    {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '_' || c == '-';
        safe.push_back(keep ? c : '_');
    }
    if (safe.empty())
    {
        safe = "material";
    }
    return safe;
}

/// @brief The GUID already recorded in @p sidecarPath, or nil if it is missing
///        or malformed. Used both to read a glTF's own id and to reuse the id of
///        a `.amat` that already exists (reconcile-not-clobber).
Core::AssetId ExistingSidecarId(const fs::path &sidecarPath)
{
    const std::optional<std::string> text = ReadWholeFile(sidecarPath);
    if (!text.has_value())
    {
        return {};
    }
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> sidecar = Core::DeserializeSidecar(*text);
    return sidecar.has_value() ? sidecar->guid : Core::AssetId{};
}

/// @brief Write @p material to @p amatAbs (+ a minted-GUID sidecar), returning
///        its id. Reconcile-not-clobber: if the file already exists, its bytes
///        and id are kept and only the id is returned. Nil on any write failure.
Core::AssetId WriteMaterialFile(const fs::path &amatAbs, const MaterialData &material)
{
    const fs::path amatSidecar = SidecarPathOf(amatAbs);
    if (fs::exists(amatSidecar))
    {
        return ExistingSidecarId(amatSidecar);
    }

    const std::expected<std::string, MaterialFileError> amatText = SerializeMaterial(material);
    if (!amatText)
    {
        Core::Log::Error("AssetImport: cannot serialize material '{}' ({}).", amatAbs.generic_string(),
                         ToString(amatText.error()));
        return {};
    }

    const Core::AssetId id = Core::MintAssetId();
    if (!WriteWholeFile(amatAbs, *amatText) ||
        !WriteWholeFile(amatSidecar, Core::SerializeSidecar(Core::AssetSidecar{.guid = id})))
    {
        Core::Log::Warn("AssetImport: failed to write '{}'.", amatAbs.generic_string());
        return {};
    }
    return id;
}

/// @brief FNV-1a hash of a glTF's source bytes (its `.gltf`/`.glb` file — where
///        material definitions live). nullopt if the file cannot be read.
std::optional<std::uint64_t> HashGltfSource(std::string_view gltfVirtualPath)
{
    const std::expected<std::vector<std::byte>, Core::AssetError> bytes =
        Core::AssetSystem::ReadBinary(gltfVirtualPath);
    if (!bytes)
    {
        return std::nullopt;
    }
    return Core::ContentHash64(*bytes);
}

/// @brief Structural equality of two materials over their *serialized* fields
///        (factors + channel GUIDs) — deliberately ignoring `Name`, which never
///        enters a `.amat`. This is the "did the material change?" test the
///        reconciler classifies with.
bool SameMaterialFields(const MaterialData &a, const MaterialData &b)
{
    return a.BaseColorFactor == b.BaseColorFactor && a.MetallicFactor == b.MetallicFactor &&
           a.RoughnessFactor == b.RoughnessFactor && a.NormalScale == b.NormalScale &&
           a.OcclusionStrength == b.OcclusionStrength && a.EmissiveFactor == b.EmissiveFactor &&
           a.BaseColorTexture == b.BaseColorTexture && a.NormalTexture == b.NormalTexture &&
           a.MetallicRoughnessTexture == b.MetallicRoughnessTexture && a.OcclusionTexture == b.OcclusionTexture &&
           a.EmissiveTexture == b.EmissiveTexture;
}

/// @brief Load the stored `.amat` for each manifest slot into a dense slot→
///        material vector, returning it and the slot count (highest slot + 1).
///        A gap slot, or an `.amat` that can't be resolved/read/parsed, stays
///        nullopt — the callers treat that as "can't prove safe" / "conflict".
std::vector<std::optional<MaterialData>>
LoadManifestMaterials(const Core::AssetSidecar &sidecar,
                      const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath,
                      std::size_t &oldCount)
{
    oldCount = 0;
    for (const Core::AssetSubAsset &entry : sidecar.subAssets)
    {
        oldCount = std::max<std::size_t>(oldCount, static_cast<std::size_t>(entry.slot) + 1);
    }
    std::vector<std::optional<MaterialData>> materials(oldCount);
    for (const Core::AssetSubAsset &entry : sidecar.subAssets)
    {
        const std::string path = resolveMaterialPath(entry.material);
        if (path.empty())
        {
            continue;
        }
        const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(path);
        if (!text)
        {
            continue;
        }
        const std::expected<MaterialData, MaterialFileError> material = DeserializeMaterial(*text);
        if (material)
        {
            materials[entry.slot] = *material;
        }
    }
    return materials;
}

/// @brief Overwrite @p amatAbs's body with @p material, **keeping its GUID** —
///        the existing sidecar's id is preserved (or a fresh one minted if the
///        `.amat` is new). Unlike WriteMaterialFile this deliberately clobbers an
///        existing body; it is used only on the user-authorized regenerate path.
///        Returns the (preserved or minted) id, nil on any write failure.
Core::AssetId OverwriteMaterialFile(const fs::path &amatAbs, const MaterialData &material)
{
    const std::expected<std::string, MaterialFileError> amatText = SerializeMaterial(material);
    if (!amatText)
    {
        Core::Log::Error("AssetImport: cannot serialize material '{}' ({}).", amatAbs.generic_string(),
                         ToString(amatText.error()));
        return {};
    }
    if (!WriteWholeFile(amatAbs, *amatText))
    {
        Core::Log::Warn("AssetImport: failed to overwrite '{}'.", amatAbs.generic_string());
        return {};
    }

    // Keep the slot's identity: reuse the existing sidecar's GUID if present, so
    // the manifest and every reference to this material stay valid across the
    // body rewrite. Only a brand-new file mints (and needs a sidecar written).
    const fs::path amatSidecar = SidecarPathOf(amatAbs);
    if (fs::exists(amatSidecar))
    {
        return ExistingSidecarId(amatSidecar);
    }
    const Core::AssetId id = Core::MintAssetId();
    if (!WriteWholeFile(amatSidecar, Core::SerializeSidecar(Core::AssetSidecar{.guid = id})))
    {
        Core::Log::Warn("AssetImport: failed to write sidecar for '{}'.", amatAbs.generic_string());
        return {};
    }
    return id;
}

} // namespace

std::expected<std::size_t, MeshImportError> ExplodeGltfMaterials(std::string_view gltfVirtualPath,
                                                                 const AssetIdResolver &resolveTextureId)
{
    // Import first: this both validates the glTF and yields the slot-indexed
    // material table whose channels are already resolved to texture GUIDs.
    std::expected<MeshData, MeshImportError> imported = ImportMesh(gltfVirtualPath, resolveTextureId);
    if (!imported)
    {
        return std::unexpected(imported.error());
    }

    // Absolute paths under the asset root. The glTF exists (we just read it), so
    // Resolve() succeeds; its siblings inherit the parent directory.
    const std::expected<fs::path, Core::AssetError> gltfAbs = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!gltfAbs)
    {
        return std::unexpected(MeshImportError::ReadFailed);
    }
    const fs::path    parentDir = gltfAbs->parent_path();
    const fs::path    gltfSidecar = SidecarPathOf(*gltfAbs);
    const std::string modelStem = gltfAbs->stem().string();

    // The reconcile pass minted the glTF's sidecar before this pass ran; without
    // it we have no id to hang the manifest on, so this is a hard stop.
    const Core::AssetId gltfId = ExistingSidecarId(gltfSidecar);
    if (gltfId.IsNil())
    {
        Core::Log::Warn("ExplodeGltfMaterials: '{}' has no readable sidecar; skipping material explosion.",
                        gltfVirtualPath);
        return std::unexpected(MeshImportError::ReadFailed);
    }

    const std::vector<MaterialData> &materials = imported->Materials;

    std::vector<Core::AssetSubAsset> manifest;
    manifest.reserve(materials.size());
    std::unordered_set<std::string> usedNames; // guards against duplicate material names

    for (std::size_t slot = 0; slot < materials.size(); ++slot)
    {
        const MaterialData &material = materials[slot];

        // "<model>_<name>.amat", disambiguated by slot only if the base collides
        // (duplicate/empty material names) — deterministic across runs.
        std::string base     = modelStem + "_" + SanitizeName(material.Name);
        std::string fileName = base + ".amat";
        if (!usedNames.insert(fileName).second)
        {
            fileName = base + "_" + std::to_string(slot) + ".amat";
            usedNames.insert(fileName);
        }

        const Core::AssetId materialId = WriteMaterialFile(parentDir / fileName, material);
        if (materialId.IsNil())
        {
            continue; // write failed -> leave this slot out of the manifest -> fallback
        }
        manifest.push_back(Core::AssetSubAsset{.slot = static_cast<std::uint32_t>(slot), .material = materialId});
    }

    // Record the slot→material bindings + the source hash into the glTF's
    // sidecar, preserving its id. This is additive relationship data, not a
    // clobber of identity — the one deliberate write to an existing sidecar the
    // reconcile rule allows.
    const Core::AssetSidecar gltfSidecarData{
        .guid = gltfId, .subAssets = std::move(manifest), .sourceHash = HashGltfSource(gltfVirtualPath)};
    if (!WriteWholeFile(gltfSidecar, Core::SerializeSidecar(gltfSidecarData)))
    {
        Core::Log::Warn("ExplodeGltfMaterials: wrote '{}' materials but failed to write its manifest.",
                        gltfVirtualPath);
        return std::unexpected(MeshImportError::ReadFailed);
    }

    return gltfSidecarData.subAssets.size();
}

ReconcileResult ReconcileGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                                       const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath)
{
    const std::expected<fs::path, Core::AssetError> gltfAbs = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!gltfAbs)
    {
        return {.outcome = ReconcileOutcome::Failed};
    }
    const fs::path gltfSidecarPath = SidecarPathOf(*gltfAbs);

    // Read the current sidecar (guid + manifest + stamped hash). A caller only
    // reconciles a composite that already has a manifest, but re-read here so the
    // rewrite preserves every field verbatim.
    const std::optional<std::string> sidecarText = ReadWholeFile(gltfSidecarPath);
    if (!sidecarText.has_value())
    {
        return {.outcome = ReconcileOutcome::Failed};
    }
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> sidecar = Core::DeserializeSidecar(*sidecarText);
    if (!sidecar.has_value() || sidecar->guid.IsNil())
    {
        return {.outcome = ReconcileOutcome::Failed};
    }

    const std::optional<std::uint64_t> currentHash = HashGltfSource(gltfVirtualPath);
    if (!currentHash.has_value())
    {
        return {.outcome = ReconcileOutcome::Failed};
    }

    // A composite that predates S4 has a manifest but no stamp: its `.amat`s were
    // just generated from this source, so record the hash and call it current.
    if (!sidecar->sourceHash.has_value())
    {
        Core::AssetSidecar stamped = *sidecar;
        stamped.sourceHash         = *currentHash;
        const bool wrote           = WriteWholeFile(gltfSidecarPath, Core::SerializeSidecar(stamped));
        return {.outcome = ReconcileOutcome::Stamped, .changedDisk = wrote};
    }

    if (*sidecar->sourceHash == *currentHash)
    {
        return {.outcome = ReconcileOutcome::UpToDate};
    }

    // Stale: import the fresh table and load the existing `.amat`s to classify.
    const std::expected<MeshData, MeshImportError> imported = ImportMesh(gltfVirtualPath, resolveTextureId);
    if (!imported)
    {
        return {.outcome = ReconcileOutcome::Failed};
    }
    const std::vector<MaterialData> &newTable = imported->Materials;

    // Dense slot→material from the manifest; a hole or an unreadable/ unparseable
    // `.amat` means we can't safely compare, so treat the whole thing as a
    // conflict rather than risk clobbering authored state.
    std::size_t                              oldCount     = 0;
    std::vector<std::optional<MaterialData>> oldMaterials = LoadManifestMaterials(*sidecar, resolveMaterialPath, oldCount);

    // Provably-safe requires every existing slot to be present and byte-for-byte
    // (field-for-field) what the new source produces.
    const std::size_t common = std::min<std::size_t>(oldCount, newTable.size());
    bool              existingUnchanged = (oldCount <= newTable.size()); // a removed slot is never safe
    for (std::size_t slot = 0; slot < common && existingUnchanged; ++slot)
    {
        if (!oldMaterials[slot].has_value() || !SameMaterialFields(newTable[slot], *oldMaterials[slot]))
        {
            existingUnchanged = false;
        }
    }

    if (!existingUnchanged)
    {
        // Slot removed/reordered, a material's fields changed, or an existing
        // `.amat` was unreadable — not provably safe. Leave everything (hash
        // included) untouched so the staleness keeps being reported.
        return {.outcome = ReconcileOutcome::ConflictStale};
    }

    // Safe: existing slots are unchanged. Materialize any appended slots, then
    // refresh the manifest + hash. New `.amat`s are always slot-suffixed so they
    // can never collide with an existing slot's file.
    Core::AssetSidecar updated = *sidecar;
    const fs::path     parentDir = gltfAbs->parent_path();
    const std::string  modelStem = gltfAbs->stem().string();
    std::size_t        added     = 0;
    for (std::size_t slot = oldCount; slot < newTable.size(); ++slot)
    {
        const std::string fileName =
            modelStem + "_" + SanitizeName(newTable[slot].Name) + "_" + std::to_string(slot) + ".amat";
        const Core::AssetId materialId = WriteMaterialFile(parentDir / fileName, newTable[slot]);
        if (materialId.IsNil())
        {
            continue;
        }
        updated.subAssets.push_back(
            Core::AssetSubAsset{.slot = static_cast<std::uint32_t>(slot), .material = materialId});
        ++added;
    }

    updated.sourceHash = *currentHash;
    const bool wrote   = WriteWholeFile(gltfSidecarPath, Core::SerializeSidecar(updated));

    return {.outcome     = added > 0 ? ReconcileOutcome::AdditiveSlots : ReconcileOutcome::GeometryOnly,
            .addedSlots  = added,
            .changedDisk = wrote};
}

// --- Prompt-driven conflict resolution (S4 second half / D5) ---------------

bool MaterialDiff::HasConflict() const
{
    for (const SlotDiff &slot : slots)
    {
        if (slot.change == SlotChange::Changed || slot.change == SlotChange::Removed)
        {
            return true;
        }
    }
    return false;
}

MaterialDiff DiffGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                               const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath)
{
    const std::expected<fs::path, Core::AssetError> gltfAbs = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!gltfAbs)
    {
        return {};
    }
    const std::optional<std::string> sidecarText = ReadWholeFile(SidecarPathOf(*gltfAbs));
    if (!sidecarText.has_value())
    {
        return {};
    }
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> sidecar = Core::DeserializeSidecar(*sidecarText);
    if (!sidecar.has_value())
    {
        return {};
    }
    const std::expected<MeshData, MeshImportError> imported = ImportMesh(gltfVirtualPath, resolveTextureId);
    if (!imported)
    {
        return {};
    }
    const std::vector<MaterialData> &newTable = imported->Materials;

    std::size_t                              oldCount = 0;
    std::vector<std::optional<MaterialData>> oldMaterials =
        LoadManifestMaterials(*sidecar, resolveMaterialPath, oldCount);

    // A stored GUID per slot, so the diff can report which `.amat` each row is.
    std::vector<Core::AssetId> oldIds(oldCount);
    for (const Core::AssetSubAsset &entry : sidecar->subAssets)
    {
        oldIds[entry.slot] = entry.material;
    }

    MaterialDiff diff;
    diff.valid                = true;
    const std::size_t slotMax = std::max<std::size_t>(oldCount, newTable.size());
    diff.slots.reserve(slotMax);
    for (std::size_t slot = 0; slot < slotMax; ++slot)
    {
        SlotDiff row;
        row.slot = static_cast<std::uint32_t>(slot);
        if (slot < newTable.size())
        {
            row.name = newTable[slot].Name;
        }

        if (slot >= newTable.size())
        {
            // The source no longer has this slot — a stored material with nothing
            // to match against.
            row.change   = SlotChange::Removed;
            row.existing = slot < oldCount ? oldIds[slot] : Core::AssetId{};
        }
        else if (slot >= oldCount || !oldMaterials[slot].has_value())
        {
            // No stored `.amat` for this source slot (appended, or a hole/unreadable
            // entry). Either way there is nothing authored here to conflict with.
            row.change = SlotChange::Added;
        }
        else
        {
            row.existing = oldIds[slot];
            row.change   = SameMaterialFields(newTable[slot], *oldMaterials[slot]) ? SlotChange::Unchanged
                                                                                   : SlotChange::Changed;
        }
        diff.slots.push_back(std::move(row));
    }
    return diff;
}

std::optional<std::size_t>
RegenerateGltfMaterials(std::string_view gltfVirtualPath, const AssetIdResolver &resolveTextureId,
                        const std::function<std::string(const Core::AssetId &)> &resolveMaterialPath)
{
    const std::expected<fs::path, Core::AssetError> gltfAbs = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!gltfAbs)
    {
        return std::nullopt;
    }
    const fs::path                   gltfSidecarPath = SidecarPathOf(*gltfAbs);
    const std::optional<std::string> sidecarText     = ReadWholeFile(gltfSidecarPath);
    if (!sidecarText.has_value())
    {
        return std::nullopt;
    }
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> sidecar = Core::DeserializeSidecar(*sidecarText);
    if (!sidecar.has_value() || sidecar->guid.IsNil())
    {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> currentHash = HashGltfSource(gltfVirtualPath);
    if (!currentHash.has_value())
    {
        return std::nullopt;
    }
    const std::expected<MeshData, MeshImportError> imported = ImportMesh(gltfVirtualPath, resolveTextureId);
    if (!imported)
    {
        return std::nullopt;
    }
    const std::vector<MaterialData> &newTable = imported->Materials;

    // The stored GUID per existing slot, so a surviving slot's file is overwritten
    // in place (identity preserved) rather than re-minted.
    std::size_t oldCount = 0;
    for (const Core::AssetSubAsset &entry : sidecar->subAssets)
    {
        oldCount = std::max<std::size_t>(oldCount, static_cast<std::size_t>(entry.slot) + 1);
    }
    std::vector<Core::AssetId> oldIds(oldCount);
    for (const Core::AssetSubAsset &entry : sidecar->subAssets)
    {
        oldIds[entry.slot] = entry.material;
    }

    const fs::path    parentDir = gltfAbs->parent_path();
    const std::string modelStem = gltfAbs->stem().string();

    // Rebuild the manifest from the fresh table. Slots the source dropped simply
    // do not carry over (their `.amat` files are orphaned on disk, never deleted).
    std::vector<Core::AssetSubAsset> manifest;
    manifest.reserve(newTable.size());
    for (std::size_t slot = 0; slot < newTable.size(); ++slot)
    {
        const MaterialData &material = newTable[slot];

        // A surviving slot with a resolvable stored file: overwrite that file,
        // keeping its GUID so references and level data stay bound.
        fs::path amatAbs;
        if (slot < oldCount && !oldIds[slot].IsNil())
        {
            const std::string existingPath = resolveMaterialPath(oldIds[slot]);
            if (!existingPath.empty())
            {
                const std::expected<fs::path, Core::AssetError> resolved =
                    Core::AssetSystem::Resolve(existingPath);
                if (resolved)
                {
                    amatAbs = *resolved;
                }
            }
        }
        // Appended slot (or a stored file that no longer resolves): a fresh,
        // slot-suffixed file so it can never collide with a surviving slot's name.
        if (amatAbs.empty())
        {
            amatAbs = parentDir / (modelStem + "_" + SanitizeName(material.Name) + "_" + std::to_string(slot) + ".amat");
        }

        const Core::AssetId materialId = OverwriteMaterialFile(amatAbs, material);
        if (materialId.IsNil())
        {
            continue; // write failed -> leave this slot unbound -> fallback
        }
        manifest.push_back(Core::AssetSubAsset{.slot = static_cast<std::uint32_t>(slot), .material = materialId});
    }

    Core::AssetSidecar updated{.guid = sidecar->guid, .subAssets = std::move(manifest), .sourceHash = *currentHash};
    if (!WriteWholeFile(gltfSidecarPath, Core::SerializeSidecar(updated)))
    {
        Core::Log::Warn("RegenerateGltfMaterials: rewrote materials but failed to write the manifest for '{}'.",
                        gltfVirtualPath);
        return std::nullopt;
    }
    return updated.subAssets.size();
}

bool AcceptGltfSource(std::string_view gltfVirtualPath)
{
    const std::expected<fs::path, Core::AssetError> gltfAbs = Core::AssetSystem::Resolve(gltfVirtualPath);
    if (!gltfAbs)
    {
        return false;
    }
    const fs::path                   gltfSidecarPath = SidecarPathOf(*gltfAbs);
    const std::optional<std::string> sidecarText     = ReadWholeFile(gltfSidecarPath);
    if (!sidecarText.has_value())
    {
        return false;
    }
    const std::expected<Core::AssetSidecar, Core::AssetSidecarError> sidecar = Core::DeserializeSidecar(*sidecarText);
    if (!sidecar.has_value())
    {
        return false;
    }
    const std::optional<std::uint64_t> currentHash = HashGltfSource(gltfVirtualPath);
    if (!currentHash.has_value())
    {
        return false;
    }

    Core::AssetSidecar accepted = *sidecar;
    accepted.sourceHash         = *currentHash;
    return WriteWholeFile(gltfSidecarPath, Core::SerializeSidecar(accepted));
}

} /* namespace Assisi::Geometry */
