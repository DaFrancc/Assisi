/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DefaultMeshes.hpp
/// @brief Factory functions for built-in primitive meshes, plus tangent generation.

#include <Assisi/Geometry/MeshData.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Assisi::Geometry
{

/// @brief Computes and fills the Tangent field of every vertex in @p mesh.
///
/// Uses Lengyel's method: derives tangent/bitangent from edge positions and UV
/// deltas, accumulates per triangle, then orthogonalises each vertex tangent
/// against its normal (Gram-Schmidt) and stores the bitangent handedness in w.
///
/// @pre @p mesh must have valid Positions, Normals, TextureCoordinates, and
///      a triangle-list index buffer (Indices.size() % 3 == 0).
inline void ComputeTangents(MeshData &mesh)
{
    const size_t vertCount = mesh.Vertices.size();
    std::vector<glm::vec3> tan1(vertCount, glm::vec3(0.0f));
    std::vector<glm::vec3> tan2(vertCount, glm::vec3(0.0f));

    for (size_t i = 0; i < mesh.Indices.size(); i += 3)
    {
        const uint32_t i0 = mesh.Indices[i];
        const uint32_t i1 = mesh.Indices[i + 1];
        const uint32_t i2 = mesh.Indices[i + 2];

        const glm::vec3 &p0 = mesh.Vertices[i0].Position;
        const glm::vec3 &p1 = mesh.Vertices[i1].Position;
        const glm::vec3 &p2 = mesh.Vertices[i2].Position;

        const glm::vec2 &uv0 = mesh.Vertices[i0].TextureCoordinates;
        const glm::vec2 &uv1 = mesh.Vertices[i1].TextureCoordinates;
        const glm::vec2 &uv2 = mesh.Vertices[i2].TextureCoordinates;

        const glm::vec3 e1  = p1 - p0;
        const glm::vec3 e2  = p2 - p0;
        const float du1 = uv1.x - uv0.x;
        const float dv1 = uv1.y - uv0.y;
        const float du2 = uv2.x - uv0.x;
        const float dv2 = uv2.y - uv0.y;

        const float det = du1 * dv2 - du2 * dv1;
        if (!std::isfinite(det) || det == 0.0f)
        {
            continue;
        }

        const float r    = 1.0f / det;
        const glm::vec3 sdir = (dv2 * e1 - dv1 * e2) * r;
        const glm::vec3 tdir = (du1 * e2 - du2 * e1) * r;

        tan1[i0] += sdir;
        tan1[i1] += sdir;
        tan1[i2] += sdir;
        tan2[i0] += tdir;
        tan2[i1] += tdir;
        tan2[i2] += tdir;
    }

    // Below this the Gram-Schmidt result is noise rather than a direction, so the
    // fallback is taken instead of normalising it. Squared length, against a
    // tangent accumulated from unnormalised edge vectors.
    constexpr float kMinTangentLengthSquared = 1e-12f;

    for (size_t i = 0; i < vertCount; ++i)
    {
        const glm::vec3 &n = mesh.Vertices[i].Normal;
        const glm::vec3 &t = tan1[i];

        // Gram-Schmidt orthogonalise
        glm::vec3 tangent           = t - glm::dot(t, n) * n;
        const float lengthSquared = glm::dot(tangent, tangent);
        if (std::isfinite(lengthSquared) && lengthSquared > kMinTangentLengthSquared)
        {
            tangent /= std::sqrt(lengthSquared);
        }
        else
        {
            // Every triangle at this vertex was degenerate in UV space, so there
            // is no tangent to recover — a UV sphere's poles are exactly that,
            // their quads collapsing to zero UV area. Normalising here would
            // write NaN into the vertex buffer and take the whole TBN frame with
            // it. Any vector perpendicular to the normal is finite, orthonormal,
            // and correct wherever the normal map is flat; a real normal map at
            // a pole is undefined regardless of what is chosen.
            const glm::vec3 axis = std::abs(n.x) > 0.9f ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(1.f, 0.f, 0.f);
            tangent          = glm::normalize(axis - glm::dot(axis, n) * n);
        }

        // Bitangent handedness: +1 if right-handed, -1 if mirrored
        const float w = (glm::dot(glm::cross(n, t), tan2[i]) < 0.0f) ? -1.0f : 1.0f;

        mesh.Vertices[i].Tangent = glm::vec4(tangent, w);
    }
}


/// @brief Returns a unit cube mesh centered at the origin.
///
/// Uses 24 vertices (4 per face) so each face has its own normals and UVs.
/// Produces 36 indices (6 faces × 2 triangles × 3 indices).
inline MeshData CreateUnitCubeMesh()
{
    MeshData mesh;

    /* 24 vertices: 4 per face so normals and UVs are correct per-face. UVs use a
     * top-left origin (uv (0,0) = top-left texel, matching stb_image / Vulkan and
     * the ImGui path), and every face is oriented so the texture reads upright and
     * un-mirrored when viewed from outside: side faces put texture-up along world
     * +Y; the top (+Y) and bottom (-Y) faces put texture-up along world -Z. Each
     * face's u x v points along -normal, i.e. no face is mirrored. */
    mesh.Vertices = {
        /* +X face (up = +Y) */
        {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* -X face (up = +Y) */
        {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

        /* +Y face (up = -Z) */
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},

        /* -Y face (up = -Z) */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},

        /* +Z face (up = +Y) */
        {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},

        /* -Z face (up = +Y) */
        {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
    };

    /* 36 indices (6 faces × 2 triangles × 3 indices). */
    mesh.Indices = {0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
                    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};

    ComputeTangents(mesh);

    return mesh;
}

/// @brief Returns a solid unit sphere (radius 1, centred at the origin) as a
/// UV-sphere with @p slices longitudinal and @p stacks latitudinal divisions.
///
/// Watertight, so its silhouette is a filled disc from any angle — the editor
/// scales it by a collider's radius to draw a clean round outline. Positions and
/// (radial) normals are set; UVs are the sphere parameterisation. The default
/// tessellation is high because a coarse sphere reads as a faceted polygon in a
/// silhouette outline rather than a circle.
inline MeshData CreateUnitSphereMesh(uint32_t slices = 48, uint32_t stacks = 24)
{
    constexpr float kPi = 3.14159265358979323846f;
    MeshData mesh;

    for (uint32_t i = 0; i <= stacks; ++i)
    {
        const float v   = static_cast<float>(i) / static_cast<float>(stacks);
        const float phi = v * kPi; // 0 at +Y pole, π at −Y pole
        const float y   = std::cos(phi);
        const float ring = std::sin(phi);
        for (uint32_t j = 0; j <= slices; ++j)
        {
            const float u     = static_cast<float>(j) / static_cast<float>(slices);
            const float theta = u * 2.0f * kPi;
            const glm::vec3 p(ring * std::cos(theta), y, ring * std::sin(theta));
            mesh.Vertices.push_back({p, p, {u, v}});
        }
    }

    // Wound counter-clockwise seen from outside, which is what MeshPass
    // rasterises as front-facing. Stacks run from the +Y pole downwards and
    // slices anticlockwise about +Y, so the quad (a, a+1, b, b+1) faces outward
    // as (a, a+1, b) + (a+1, b+1, b).
    const uint32_t stride = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
    {
        for (uint32_t j = 0; j < slices; ++j)
        {
            const uint32_t a = i * stride + j;
            const uint32_t b = a + stride;
            mesh.Indices.insert(mesh.Indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }

    ComputeTangents(mesh);

    return mesh;
}

/// @brief Returns a solid unit cylinder (radius 1, spanning y ∈ [−1, 1]) with
/// @p slices around its axis, capped at both ends so it is watertight.
///
/// The editor scales it by (radius, halfHeight, radius) for a cylinder collider,
/// and reuses it (with two unit spheres at the ends) to build a capsule's
/// silhouette. Only positions matter for the silhouette; normals/UVs are set
/// plausibly.
inline MeshData CreateUnitCylinderMesh(uint32_t slices = 48)
{
    constexpr float kPi = 3.14159265358979323846f;
    MeshData mesh;

    // Side rim vertices, top (y=+1) then bottom (y=−1), with radial normals.
    for (uint32_t j = 0; j <= slices; ++j)
    {
        const float u     = static_cast<float>(j) / static_cast<float>(slices);
        const float theta = u * 2.0f * kPi;
        const float x     = std::cos(theta);
        const float z     = std::sin(theta);
        mesh.Vertices.push_back({{x, 1.0f, z}, {x, 0.0f, z}, {u, 0.0f}});
        mesh.Vertices.push_back({{x, -1.0f, z}, {x, 0.0f, z}, {u, 1.0f}});
    }
    // Counter-clockwise from outside, as on the sphere above.
    for (uint32_t j = 0; j < slices; ++j)
    {
        const uint32_t top = j * 2;
        const uint32_t bot = top + 1;
        mesh.Indices.insert(mesh.Indices.end(), {top, top + 2, bot, top + 2, bot + 2, bot});
    }

    // Cap fans: a centre vertex for each end, fanned over its own rim ring.
    const auto addCap = [&mesh, slices](float y, float ny) { // kPi is constexpr — no capture needed
                            const uint32_t center = static_cast<uint32_t>(mesh.Vertices.size());
                            mesh.Vertices.push_back({{0.0f, y, 0.0f}, {0.0f, ny, 0.0f}, {0.5f, 0.5f}});
                            const uint32_t first = static_cast<uint32_t>(mesh.Vertices.size());
                            for (uint32_t j = 0; j <= slices; ++j)
                            {
                                const float theta = static_cast<float>(j) / static_cast<float>(slices) * 2.0f * kPi;
                                const float x     = std::cos(theta);
                                const float z     = std::sin(theta);
                                mesh.Vertices.push_back({{x, y, z}, {0.0f, ny, 0.0f}, {0.5f + 0.5f * x, 0.5f + 0.5f * z}});
                            }
                            // The two caps face opposite ways, so one fan has to
                            // run the other way round to stay outward-facing.
                            // The rim is generated anticlockwise about +Y, which
                            // is already correct seen from below (-Y).
                            for (uint32_t j = 0; j < slices; ++j)
                            {
                                if (ny > 0.0f)
                                {
                                    mesh.Indices.insert(mesh.Indices.end(), {center, first + j + 1, first + j});
                                }
                                else
                                {
                                    mesh.Indices.insert(mesh.Indices.end(), {center, first + j, first + j + 1});
                                }
                            }
                        };
    addCap(1.0f, 1.0f);
    addCap(-1.0f, -1.0f);

    ComputeTangents(mesh);

    return mesh;
}

/// @brief Returns a unit icosphere (radius 1, centred at the origin): an
/// icosahedron whose triangles are @p subdivisions times split in four and
/// re-projected onto the sphere.
///
/// Triangle count is 20 * 4^subdivisions, so it is not freely tunable the way
/// CreateUnitSphereMesh's is. What it buys is uniform triangle density and no
/// pole singularity — a UV sphere crowds triangles at its poles and leaves them
/// degenerate in UV space. The cost is the UVs: a sphere has no seamless
/// unwrap, and the spherical parameterisation used here wraps at the ±Z
/// meridian, so a textured icosphere shows a seam. Prefer CreateUnitSphereMesh
/// wherever the surface is textured.
inline MeshData CreateIcosphereMesh(uint32_t subdivisions = 2)
{
    constexpr float kPi = 3.14159265358979323846f;
    MeshData mesh;

    // The 12 icosahedron vertices are the corners of three mutually
    // perpendicular golden-ratio rectangles, normalised onto the unit sphere.
    const float phi = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<glm::vec3> positions = {
        {-1.0f, phi, 0.0f},  {1.0f, phi, 0.0f},  {-1.0f, -phi, 0.0f}, {1.0f, -phi, 0.0f},
        {0.0f, -1.0f, phi},  {0.0f, 1.0f, phi},  {0.0f, -1.0f, -phi}, {0.0f, 1.0f, -phi},
        {phi, 0.0f, -1.0f},  {phi, 0.0f, 1.0f},  {-phi, 0.0f, -1.0f}, {-phi, 0.0f, 1.0f},
    };
    for (glm::vec3 &position : positions)
    {
        position = glm::normalize(position);
    }

    std::vector<uint32_t> indices = {0, 11, 5, 0, 5,  1,  0,  1,  7,  0,  7,  10, 0,  10, 11,
                                     1, 5,  9, 5, 11, 4,  11, 10, 2,  10, 7,  6,  7,  1,  8,
                                     3, 9,  4, 3, 4,  2,  3,  2,  6,  3,  6,  8,  3,  8,  9,
                                     4, 9,  5, 2, 4,  11, 6,  2,  10, 8,  6,  7,  9,  8,  1};

    // Each pass splits every triangle into four, projecting the three new edge
    // midpoints back onto the sphere. The midpoint cache keys on the ordered
    // edge so the two triangles sharing it get the same vertex — without it the
    // mesh would come apart into unwelded triangles and the vertex count would
    // grow four times faster than it needs to.
    for (uint32_t pass = 0; pass < subdivisions; ++pass)
    {
        std::unordered_map<uint64_t, uint32_t> midpoints;
        std::vector<uint32_t> refined;
        refined.reserve(indices.size() * 4);

        const auto midpoint = [&positions, &midpoints](uint32_t a, uint32_t b)
                              {
                                  const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) |
                                                       static_cast<uint64_t>(std::max(a, b));
                                  const std::unordered_map<uint64_t, uint32_t>::iterator found = midpoints.find(key);
                                  if (found != midpoints.end())
                                  {
                                      return found->second;
                                  }
                                  const uint32_t index = static_cast<uint32_t>(positions.size());
                                  positions.push_back(glm::normalize(positions[a] + positions[b]));
                                  midpoints.emplace(key, index);
                                  return index;
                              };

        for (size_t i = 0; i < indices.size(); i += 3)
        {
            const uint32_t v0 = indices[i];
            const uint32_t v1 = indices[i + 1];
            const uint32_t v2 = indices[i + 2];
            const uint32_t a  = midpoint(v0, v1);
            const uint32_t b  = midpoint(v1, v2);
            const uint32_t c  = midpoint(v2, v0);
            refined.insert(refined.end(), {v0, a, c, v1, b, a, v2, c, b, a, b, c});
        }
        indices = std::move(refined);
    }

    // Spherical UVs, and the normal is the position. The wrap discontinuity at
    // the ±Z meridian is left in the shared vertices rather than split, since
    // this primitive exists for uniform geometry rather than for texturing.
    mesh.Vertices.reserve(positions.size());
    for (const glm::vec3 &position : positions)
    {
        const float u = 0.5f + std::atan2(position.z, position.x) / (2.0f * kPi);
        const float v = 0.5f - std::asin(position.y) / kPi;
        mesh.Vertices.push_back({position, position, {u, v}});
    }
    mesh.Indices = std::move(indices);

    ComputeTangents(mesh);

    return mesh;
}

/// @brief The tessellation each `prim://` primitive is registered at.
///
/// Pinned here rather than taken from the factory defaults above, which are
/// tuned for editor collider silhouettes and are deliberately fine so a
/// wireframe outline reads as a circle rather than a polygon. Retuning an
/// outline must not silently move the triangle counts the perf reference scenes
/// publish as their contract, so the two sets of numbers are kept apart.
///
/// This is a small fixed ladder of engine built-ins, because a `prim://` path
/// needs a reserved compile-time AssetId (Core/AssetId.hpp) and those cannot be
/// minted for arbitrary parameters. Authoring a sphere at any density is the
/// procedural-mesh asset's job; these are the presets the engine ships.
///
/// The unsuffixed name of each shape is its sensible default; `-low` and
/// `-high` bracket it. Cylinders stop at two rungs — CreateUnitCylinderMesh
/// subdivides only around the axis, so slices buy far fewer triangles than a
/// sphere's slices x stacks and a third rung would not be a different order of
/// cost.
namespace PrimitiveTessellation
{
inline constexpr uint32_t kSphereLowSlices  = 12; ///< prim://sphere-low — 144 triangles.
inline constexpr uint32_t kSphereLowStacks  = 6;
inline constexpr uint32_t kSphereSlices     = 24; ///< prim://sphere — 576 triangles.
inline constexpr uint32_t kSphereStacks     = 12;
inline constexpr uint32_t kSphereHighSlices = 64; ///< prim://sphere-high — 4096 triangles.
inline constexpr uint32_t kSphereHighStacks = 32;

inline constexpr uint32_t kIcosphereLowSubdivisions  = 2; ///< prim://icosphere-low — 320 triangles.
inline constexpr uint32_t kIcosphereSubdivisions     = 3; ///< prim://icosphere — 1280 triangles.
inline constexpr uint32_t kIcosphereHighSubdivisions = 5; ///< prim://icosphere-high — 20480 triangles.

inline constexpr uint32_t kCylinderSlices     = 24; ///< prim://cylinder — 96 triangles.
inline constexpr uint32_t kCylinderHighSlices = 64; ///< prim://cylinder-high — 256 triangles.
} // namespace PrimitiveTessellation

} /* namespace Assisi::Geometry */
