/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMeshBounds.cpp
/// @brief The submesh bound fitters in MeshData.hpp against index data that a
/// hand-authored or hostile glTF can actually carry.
///
/// The fitters take an index *range* and dereference through it, so they have
/// two separate things to validate: that the range lies inside `Indices`, and
/// that each index it names lies inside `Vertices`.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>

using Assisi::Geometry::Aabb;
using Assisi::Geometry::BoundingSphere;
using Assisi::Geometry::ComputeAabb;
using Assisi::Geometry::ComputeBoundingSphere;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::Vertex;

namespace
{

/// Two vertices well away from the origin, so a fit that ran over them is
/// distinguishable from the zero box the out-of-range contract promises.
MeshData TwoVertexMesh()
{
    MeshData mesh;
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(10.f, 10.f, 10.f)});
    mesh.Vertices.push_back(Vertex{.Position = glm::vec3(20.f, 20.f, 20.f)});
    return mesh;
}

} // namespace

TEST_CASE("ComputeAabb: an in-range submesh fits the vertices it names")
{
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 1u, 0u};

    const Aabb box = ComputeAabb(mesh, 0, 3);
    CHECK(box.min.x == doctest::Approx(10.f));
    CHECK(box.max.x == doctest::Approx(20.f));
}

TEST_CASE("ComputeAabb: a range past the end of the index array returns the zero box")
{
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 1u};

    CHECK(ComputeAabb(mesh, 0, 5).max.x == doctest::Approx(0.f));
    CHECK(ComputeAabb(mesh, 0, 0).max.x == doctest::Approx(0.f));
}

TEST_CASE("ComputeBoundingSphere: a range past the end of the index array returns the zero sphere")
{
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 1u};

    CHECK(ComputeBoundingSphere(mesh, 0, 5).radius == doctest::Approx(0.f));
    CHECK(ComputeBoundingSphere(mesh, 0, 0).radius == doctest::Approx(0.f));
}

TEST_CASE("ComputeAabb: an index that names no vertex returns the zero box")
{
    // The zero box is what the doc comment promises for out-of-range input;
    // refusing the whole fit is the only answer that keeps a submesh's bounds
    // meaningful.
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 3u, 1u}; // 3 names no vertex — the mesh has two

    const Aabb box = ComputeAabb(mesh, 0, 3);
    CHECK(box.min.x == doctest::Approx(0.f));
    CHECK(box.max.x == doctest::Approx(0.f));
}

TEST_CASE("ComputeBoundingSphere: an index that names no vertex returns the zero sphere")
{
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 3u, 1u};

    const BoundingSphere sphere = ComputeBoundingSphere(mesh, 0, 3);
    CHECK(sphere.radius == doctest::Approx(0.f));
    CHECK(sphere.center.x == doctest::Approx(0.f));
}
