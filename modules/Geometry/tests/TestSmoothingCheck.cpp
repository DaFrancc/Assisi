/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/SmoothingCheck.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using Assisi::Geometry::CreateUnitCylinderMesh;
using Assisi::Geometry::EnsureSubMeshTables;
using Assisi::Geometry::FindSharpSmoothing;
using Assisi::Geometry::kMajorSmoothingBulge;
using Assisi::Geometry::kMinorSmoothingBulge;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::SharpSmoothing;
using Assisi::Geometry::SubMesh;
using Assisi::Geometry::Vertex;

namespace
{
/// The test cone: sides around, a tip one unit up, a base of radius one half.
constexpr uint32_t kConeSides = 16;
constexpr float kConeHeight = 1.f;
constexpr float kConeRadius = 0.5f;

/// The floor the cone is stood on to make it a part of a larger model, and the
/// cone's heights on it. Its bulge is about a quarter of its height, so an
/// eighth of its height against the floor: 0.25% at 2 cm, 2.5% at 20 cm, 12.5%
/// at 1 m.
constexpr float kFloorSize = 2.f;
constexpr float kTinyConeScale = 0.02f;
constexpr float kSmallConeScale = 0.2f;
constexpr float kLargeConeScale = 1.f;

glm::vec3 RingPoint(uint32_t side)
{
    const float angle = glm::two_pi<float>() * static_cast<float>(side) / static_cast<float>(kConeSides);
    return glm::vec3(kConeRadius * std::cos(angle), 0.f, kConeRadius * std::sin(angle));
}

/// The outward normal of the cone's side where it passes @p angle around.
glm::vec3 SlantNormal(float angle)
{
    return glm::normalize(glm::vec3(kConeHeight * std::cos(angle), kConeRadius, kConeHeight * std::sin(angle)));
}

/// Appends the triangle @p a, @p b, @p c, turned to face away from @p inside.
void AddTriangle(MeshData &mesh, const Vertex &a, const Vertex &b, const Vertex &c, const glm::vec3 &inside)
{
    const uint32_t first = static_cast<uint32_t>(mesh.Vertices.size());
    const glm::vec3 face = glm::cross(b.Position - a.Position, c.Position - a.Position);
    const bool outward = glm::dot(face, a.Position - inside) > 0.f;
    mesh.Vertices.push_back(a);
    mesh.Vertices.push_back(outward ? b : c);
    mesh.Vertices.push_back(outward ? c : b);
    mesh.Indices.insert(mesh.Indices.end(), {first, first + 1, first + 2});
}

/// A cone the way an exporter writes one when every vertex is shared and
/// smoothed: one tip vertex pointing straight up, and base corners whose
/// normal averages the side's and the base's.
MeshData AveragedCone()
{
    MeshData mesh;
    const glm::vec3 inside(0.f, kConeHeight / 4.f, 0.f);
    const Vertex tip{.Position = glm::vec3(0.f, kConeHeight, 0.f), .Normal = glm::vec3(0.f, 1.f, 0.f)};
    const Vertex centre{.Position = glm::vec3(0.f), .Normal = glm::vec3(0.f, -1.f, 0.f)};
    for (uint32_t side = 0; side < kConeSides; ++side)
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(side) / static_cast<float>(kConeSides);
        const float next = glm::two_pi<float>() * static_cast<float>(side + 1) / static_cast<float>(kConeSides);
        const Vertex here{.Position = RingPoint(side),
                          .Normal = glm::normalize(SlantNormal(angle) + glm::vec3(0.f, -1.f, 0.f))};
        const Vertex there{.Position = RingPoint(side + 1),
                           .Normal = glm::normalize(SlantNormal(next) + glm::vec3(0.f, -1.f, 0.f))};
        AddTriangle(mesh, tip, here, there, inside);
        AddTriangle(mesh, centre, here, there, inside);
    }
    EnsureSubMeshTables(mesh);
    return mesh;
}

/// The same cone built to be smooth-shaded: a tip vertex per side facing that
/// side, the side's corners facing along the slant, and a flat base of its own.
MeshData SplitCone()
{
    MeshData mesh;
    const glm::vec3 inside(0.f, kConeHeight / 4.f, 0.f);
    const glm::vec3 down(0.f, -1.f, 0.f);
    const Vertex centre{.Position = glm::vec3(0.f), .Normal = down};
    for (uint32_t side = 0; side < kConeSides; ++side)
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(side) / static_cast<float>(kConeSides);
        const float next = glm::two_pi<float>() * static_cast<float>(side + 1) / static_cast<float>(kConeSides);
        const Vertex tip{.Position = glm::vec3(0.f, kConeHeight, 0.f), .Normal = SlantNormal((angle + next) / 2.f)};
        AddTriangle(mesh, tip, Vertex{.Position = RingPoint(side), .Normal = SlantNormal(angle)},
                    Vertex{.Position = RingPoint(side + 1), .Normal = SlantNormal(next)}, inside);
        AddTriangle(mesh, centre, Vertex{.Position = RingPoint(side), .Normal = down},
                    Vertex{.Position = RingPoint(side + 1), .Normal = down}, inside);
    }
    EnsureSubMeshTables(mesh);
    return mesh;
}

/// A cube smoothed all round: eight shared corners, each normal pointing out
/// along its diagonal.
MeshData SmoothCube()
{
    MeshData mesh;
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 position((corner & 1u) != 0u ? 1.f : -1.f, (corner & 2u) != 0u ? 1.f : -1.f,
                                 (corner & 4u) != 0u ? 1.f : -1.f);
        mesh.Vertices.push_back(Vertex{.Position = position, .Normal = glm::normalize(position)});
    }
    // Two triangles per face, counter-clockwise seen from outside.
    mesh.Indices = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                    2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
    EnsureSubMeshTables(mesh);
    return mesh;
}

MeshData Cylinder(uint32_t sides)
{
    MeshData mesh = CreateUnitCylinderMesh(sides);
    EnsureSubMeshTables(mesh);
    return mesh;
}

/// The averaged cone scaled by @p scale, standing on a 2 m floor in the same
/// mesh and submesh.
MeshData ConeOnFloor(float scale)
{
    MeshData mesh = AveragedCone();
    for (Vertex &vertex : mesh.Vertices)
    {
        vertex.Position *= scale;
    }
    const glm::vec3 up(0.f, 1.f, 0.f);
    const uint32_t first = static_cast<uint32_t>(mesh.Vertices.size());
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(-kFloorSize / 2.f, 0.f, -kFloorSize / 2.f), .Normal = up});
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(-kFloorSize / 2.f, 0.f, kFloorSize / 2.f), .Normal = up});
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(kFloorSize / 2.f, 0.f, -kFloorSize / 2.f), .Normal = up});
    mesh.Indices.insert(mesh.Indices.end(), {first, first + 1, first + 2});
    mesh.SubMeshes[0].IndexCount = static_cast<uint32_t>(mesh.Indices.size());
    return mesh;
}
} // namespace

TEST_CASE("FindSharpSmoothing: a cone smoothed through its tip bulges badly")
{
    // The single tip vertex points straight up while every side it ends leans
    // out by the cone's slope, about 63 degrees off each; the base corners
    // average the side's normal with the base's. The curve those describe
    // stands about a quarter of the cone's height off its sides.
    const std::vector<SharpSmoothing> found = FindSharpSmoothing(AveragedCone());
    REQUIRE(found.size() == 1);
    CHECK(found[0].subMesh == 0);
    CHECK(found[0].bulge >= kMajorSmoothingBulge);
}

TEST_CASE("FindSharpSmoothing: the same cone with a tip per side and its own base passes")
{
    CHECK(FindSharpSmoothing(SplitCone()).empty());
}

TEST_CASE("FindSharpSmoothing: a cube smoothed all round bulges badly")
{
    const std::vector<SharpSmoothing> found = FindSharpSmoothing(SmoothCube());
    REQUIRE(found.size() == 1);
    CHECK(found[0].bulge >= kMajorSmoothingBulge);
}

TEST_CASE("FindSharpSmoothing: a five-sided cylinder passes and a four-sided one does not")
{
    // Each side's corners bend half a side's angle from it: 36 degrees at five
    // sides, 45 at four. The caps have their own flat normals either way.
    CHECK(FindSharpSmoothing(Cylinder(5)).empty());
    CHECK(FindSharpSmoothing(Cylinder(4)).size() == 1);
}

TEST_CASE("FindSharpSmoothing: sharp corners on a part too small to matter go unreported")
{
    // The same bad cone, 2 cm tall on a 2 m floor: its shadows go wrong over a
    // sliver of the model, so it is not worth a warning.
    CHECK(FindSharpSmoothing(ConeOnFloor(kTinyConeScale)).empty());

    // Alone, at the same size, it is the whole model, and is reported.
    MeshData alone = AveragedCone();
    for (Vertex &vertex : alone.Vertices)
    {
        vertex.Position *= kTinyConeScale;
    }
    CHECK(FindSharpSmoothing(alone).size() == 1);
}

TEST_CASE("FindSharpSmoothing: the bulge against the whole model sets how bad it is")
{
    const std::vector<SharpSmoothing> small = FindSharpSmoothing(ConeOnFloor(kSmallConeScale));
    REQUIRE(small.size() == 1);
    CHECK(small[0].bulge >= kMinorSmoothingBulge);
    CHECK(small[0].bulge < kMajorSmoothingBulge);

    const std::vector<SharpSmoothing> large = FindSharpSmoothing(ConeOnFloor(kLargeConeScale));
    REQUIRE(large.size() == 1);
    CHECK(large[0].bulge >= kMajorSmoothingBulge);
}

TEST_CASE("FindSharpSmoothing: a material whose shadows do not follow its smoothing is skipped")
{
    // Foliage cards bend their normals far from their faces on purpose, and the
    // lit pass never lifts their lookups.
    MeshData cone = AveragedCone();
    cone.Materials[cone.SubMeshes[0].MaterialSlot].ShadowsFollowSmoothing = false;
    CHECK(FindSharpSmoothing(cone).empty());
}

TEST_CASE("FindSharpSmoothing: each submesh is reported on its own")
{
    // A flat triangle in one submesh ahead of the bad cone in another: only the
    // cone's submesh is named.
    MeshData mesh = AveragedCone();
    const uint32_t coneIndices = static_cast<uint32_t>(mesh.Indices.size());
    const glm::vec3 up(0.f, 1.f, 0.f);
    const uint32_t first = static_cast<uint32_t>(mesh.Vertices.size());
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(0.f, 0.f, 0.f), .Normal = up});
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(0.f, 0.f, 1.f), .Normal = up});
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(1.f, 0.f, 0.f), .Normal = up});
    mesh.Indices.insert(mesh.Indices.end(), {first, first + 1, first + 2});
    SubMesh flat;
    flat.IndexOffset = coneIndices;
    flat.IndexCount = 3;
    SubMesh cone;
    cone.IndexCount = coneIndices;
    mesh.SubMeshes = {flat, cone};

    const std::vector<SharpSmoothing> found = FindSharpSmoothing(mesh);
    REQUIRE(found.size() == 1);
    CHECK(found[0].subMesh == 1);
}

