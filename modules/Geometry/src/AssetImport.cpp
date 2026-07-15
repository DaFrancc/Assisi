/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/AssetImport.hpp>

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
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/MaterialFile.hpp> // SerializeMaterial
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
///        or malformed. Used both to read the glTF's own id and to reuse the id
///        of a `.amat` that already exists (reconcile-not-clobber).
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
    const fs::path parentDir     = gltfAbs->parent_path();
    const fs::path gltfSidecar   = SidecarPathOf(*gltfAbs);
    const std::string modelStem  = gltfAbs->stem().string();

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

        const fs::path amatAbs     = parentDir / fileName;
        const fs::path amatSidecar = SidecarPathOf(amatAbs);

        // Reconcile-not-clobber: a `.amat` that already exists (a prior run, or a
        // hand-authored file) keeps its bytes and its minted id; we only reuse it.
        Core::AssetId materialId = fs::exists(amatSidecar) ? ExistingSidecarId(amatSidecar) : Core::AssetId{};
        if (materialId.IsNil())
        {
            const std::expected<std::string, MaterialFileError> amatText = SerializeMaterial(material);
            if (!amatText)
            {
                Core::Log::Error("ExplodeGltfMaterials: cannot serialize material for '{}' slot {} ({}); slot will "
                                 "fall back to the default material.",
                                 gltfVirtualPath, slot, ToString(amatText.error()));
                continue; // leave this slot out of the manifest -> nil -> fallback
            }

            materialId = Core::MintAssetId();
            if (!WriteWholeFile(amatAbs, *amatText) ||
                !WriteWholeFile(amatSidecar, Core::SerializeSidecar(Core::AssetSidecar{.guid = materialId})))
            {
                Core::Log::Warn("ExplodeGltfMaterials: failed to write '{}'; slot {} falls back to the default "
                                "material.",
                                amatAbs.generic_string(), slot);
                continue;
            }
        }

        manifest.push_back(Core::AssetSubAsset{.slot = static_cast<std::uint32_t>(slot), .material = materialId});
    }

    // Record the slot→material bindings into the glTF's sidecar, preserving its
    // id. This is additive relationship data, not a clobber of identity — the
    // one deliberate write to an existing sidecar the reconcile rule allows.
    const Core::AssetSidecar gltfSidecarData{.guid = gltfId, .subAssets = std::move(manifest)};
    if (!WriteWholeFile(gltfSidecar, Core::SerializeSidecar(gltfSidecarData)))
    {
        Core::Log::Warn("ExplodeGltfMaterials: wrote '{}' materials but failed to write its manifest.",
                        gltfVirtualPath);
        return std::unexpected(MeshImportError::ReadFailed);
    }

    return gltfSidecarData.subAssets.size();
}

} /* namespace Assisi::Geometry */
