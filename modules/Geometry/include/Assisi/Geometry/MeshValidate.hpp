/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshValidate.hpp
/// @brief Whether a MeshData is one the renderer can draw.
///
/// The importer builds these tables from whatever a glTF happened to contain,
/// and nothing between there and the draw call checks that the result addresses
/// itself. An index past the vertex array, a LOD naming submeshes that are not
/// there, or a submesh naming a material slot the table does not have all reach
/// the GPU as a read of memory that belongs to something else.
///
/// Cook time is where this runs, so a failure is a build failure with the asset
/// named rather than a corrupt draw on a player's machine. It lives in Geometry
/// rather than in the cooker because the editor imports the same meshes through
/// the same importer, and a rule implemented twice is a rule that disagrees with
/// itself.

#include <cstdint>
#include <expected>
#include <string_view>

#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief What is wrong with a mesh.
enum class MeshValidationError : std::uint8_t
{
    NoGeometry,           ///< No vertices, or no indices.
    IndexCountNotTriangles, ///< The index count is not a multiple of three.
    IndexOutOfRange,      ///< An index names a vertex the array does not have.
    SubMeshOutOfRange,    ///< A submesh's index range runs past the index array.
    EmptySubMesh,         ///< A submesh covers no indices, so it draws nothing.
    MaterialSlotOutOfRange, ///< A submesh names a material slot the table lacks.
    NoLods,               ///< Submeshes exist but no LOD addresses them.
    LodOutOfRange,        ///< A LOD's submesh range runs past the submesh array.
    EmptyLod,             ///< A LOD covers no submeshes, so that level draws nothing.
    LodsNotContiguous,    ///< The LOD ranges do not tile the submesh array in order.
    LodThresholdNotDescending, ///< An authored screen-size threshold does not fall across the chain.
};

/// @brief A short human-readable description, for the line a cook failure prints.
[[nodiscard]] std::string_view ToString(MeshValidationError error) noexcept;

/// @brief Whether @p mesh addresses itself consistently.
///
/// Expects the explicit tables — call EnsureSubMeshTables first. The degenerate
/// "no submeshes means one implicit whole-mesh submesh" form is a convenience
/// for factory meshes, and validating it would mean encoding that rule a second
/// time here.
///
/// Thresholds are checked only where they are authored: a zero means the asset
/// carried none and DefaultLodScreenSize supplies one, which descends by
/// construction. A chain that authors some and not others is still checked
/// across the ones it authored, because those are the numbers selection will
/// actually compare.
[[nodiscard]] std::expected<void, MeshValidationError> ValidateMesh(const MeshData &mesh);

} // namespace Assisi::Geometry
