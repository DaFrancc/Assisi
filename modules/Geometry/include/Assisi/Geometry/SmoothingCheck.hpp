/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SmoothingCheck.hpp
/// @brief Where a mesh is smooth-shaded over corners too sharp for its shadows,
///        and how much it matters.
///
/// Near the edge between light and dark, the lit pass makes a shadow lookup
/// from the curved surface a smooth-shaded mesh's vertex normals describe
/// rather than from its flat triangles, which is what keeps a low-poly sphere
/// from shadowing itself in stripes. That surface is a fair stand-in for the
/// shape only while each corner's normal stays near the triangles around it.
/// Where one bends far from a triangle, such as a cone's single tip vertex, a
/// cube smoothed all round, or a cylinder whose caps share the side's normals,
/// the surface bulges far off the triangles, and shadows there slide off their
/// casters or break up. The fix is in the content, so the importer says so.
///
/// A sharp corner on a small part bulges by little, and its shadows are off by
/// less than anyone sees. How much the surface bulges at the bent corners,
/// against the size of the whole model, is what decides whether it is worth a
/// warning.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Geometry
{

/// @brief The most a corner's normal may bend from its own triangle's, in
/// degrees, before the curve it describes stands in for the wrong shape. A
/// cylinder of five sides bends its corners 36 degrees and passes; one of four
/// bends them 45, as far as a cube's faces from their shared edge, and does
/// not. The editor's Smoothing debug view turns red at the same angle.
inline constexpr float kMaxSmoothingBendDegrees = 40.f;

/// @brief The bulge, as a fraction of the model's size, below which bent
/// corners go unreported. Below it they belong to a small part of the model,
/// such as a 2 cm spike on a 2 m figure (0.2%), and the shadows they put wrong
/// are a sliver inside the dim band where the light grazes.
inline constexpr float kMinorSmoothingBulge = 0.01f;

/// @brief The bulge, as a fraction of the model's size, from which the shadows
/// bent corners put wrong cover a part of the model anyone would notice. A cube
/// smoothed all round bulges by a third of its size, a cone smoothed through
/// its tip by a quarter, and a four-sided cylinder by a sixth.
inline constexpr float kMajorSmoothingBulge = 0.05f;

/// @brief One submesh with corners bent past kMaxSmoothingBendDegrees that
/// bulge by at least kMinorSmoothingBulge.
struct SharpSmoothing
{
    /// The furthest the curve stands off any triangle with a bent corner, over
    /// the model's size: the largest extent of its bounding box.
    float bulge = 0.f;
    uint32_t subMesh = 0; ///< Index into MeshData::SubMeshes.
};

/// @brief Every submesh of @p mesh worth a warning, in submesh order.
///
/// A submesh whose material turns ShadowsFollowSmoothing off is skipped: its
/// shadows never follow the curve. A degenerate triangle, or a corner with no
/// normal, has no bend to measure and is skipped too. Expects the explicit
/// submesh tables (EnsureSubMeshTables).
[[nodiscard]] std::vector<SharpSmoothing> FindSharpSmoothing(const MeshData &mesh);

/// @brief Gives every corner of @p mesh bent past kMaxSmoothingBendDegrees its
/// own normal, the way marking the edges there sharp would.
///
/// The new normal averages the faces around the corner's position that are
/// within kMaxSmoothingBendDegrees of the corner's own triangle, weighted by
/// area: a cone's tip takes the slope of the sides beside it, a cylinder's rim
/// splits into the side's normal and the cap's, a box smoothed all round
/// becomes flat-faced. Each such corner gets a vertex of its own, shared with
/// the other corners there that take the same normal; every other corner keeps
/// the normal it was authored with. Afterwards FindSharpSmoothing finds
/// nothing. Skips the same submeshes FindSharpSmoothing does, and expects the
/// same explicit tables.
///
/// @return How many corners were given a normal of their own.
std::size_t SplitSharpSmoothing(MeshData &mesh);

} // namespace Assisi::Geometry
