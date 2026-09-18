/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CookedMesh.hpp
/// @brief A mesh as a cooked blob: the arrays the GPU path uploads, and the
///        material each slot is bound to.
///
/// The writer and the reader live together so the layout has one definition. The
/// cooker writes through WriteCookedMesh; a load reads through ReadCookedMesh and
/// runs no import.
///
/// The slot table is ids rather than the import's MaterialData. What a slot is
/// bound to lives in the mesh's sidecar after import, and the cook bakes that in,
/// so a shipped game needs no sidecar to know which material a submesh draws with.

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief Version of the mesh payload's layout, separate from the blob envelope's.
///
/// Part of the mesh cooker's cache key, so bumping it re-cooks every mesh.
inline constexpr std::uint8_t kMeshPayloadVersion = 1;

/// @brief A cooked mesh, read back.
struct CookedMesh
{
    /// Vertex and index arrays, submesh and LOD tables, and bounds. `Materials`
    /// holds one default entry per slot so slot indices stay in range; the real
    /// binding is @ref slotMaterials.
    MeshData mesh;

    /// The material each slot draws with, by slot index. Nil where the sidecar
    /// bound nothing.
    std::vector<Core::AssetId> slotMaterials;
};

/// @brief Why bytes did not read as a cooked mesh.
enum class CookedMeshError : std::uint8_t
{
    NotAMesh,           ///< Not a cooked blob, or a blob of another kind.
    UnsupportedVersion, ///< A mesh layout this build does not read.
    Truncated,          ///< The bytes end, or a count claims more than is left.
    Invalid,            ///< Framed correctly, but the tables disagree (see ValidateMesh).
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedMeshError error) noexcept;

/// @brief Write @p mesh as a complete cooked blob, envelope included.
///
/// @p slotMaterials has one id per entry of `mesh.Materials`.
void WriteCookedMesh(Core::BitWriter &writer, const MeshData &mesh, std::span<const Core::AssetId> slotMaterials);

/// @brief Read a blob written by WriteCookedMesh.
///
/// Every count is checked against the bytes left before anything is allocated,
/// and the result is validated, so a forged blob can neither exhaust memory nor
/// hand the GPU an index past the vertex array.
[[nodiscard]] std::expected<CookedMesh, CookedMeshError> ReadCookedMesh(std::span<const std::byte> bytes);

} // namespace Assisi::Geometry
