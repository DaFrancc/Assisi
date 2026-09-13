/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMeshBounds.cpp
/// @brief The submesh bound fitters in MeshData.hpp against index data that a
/// hand-authored or hostile glTF can actually carry.
///
/// The fitters take an index *range* and dereference through it, so they have
/// two separate things to validate: that the range lies inside `Indices`, and
/// that each index it names lies inside `Vertices`. Only the first is checked
/// today — see the out-of-range cases at the bottom.

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
///
/// The `reserve` is load-bearing and is a concession, not a detail: the
/// out-of-range cases at the bottom index past `size()`, and the spare capacity
/// keeps that read inside the vector's own allocation so a test documenting the
/// missing check does not take the runner down with it. It also costs something
/// — see the note down there — because a real glTF's index array has no such
/// cushion.
MeshData TwoVertexMesh()
{
    MeshData mesh;
    mesh.Vertices.reserve(64);
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

    // The half that is checked today: `indexOffset + indexCount > Indices.size()`.
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

// ── An index that names no vertex ────────────────────────────────────────────
//
// Open. MeshData.hpp:86 and :127 validate the index *range* against
// Indices.size(); nothing validates the indices themselves, and neither the
// check nor the loop ever looks at Vertices.size(). So `Vertices[Indices[i]]`
// (:95, :138) runs off the vertex array for any glTF whose index array outruns
// it — which is exactly the file an importer has to survive.
//
// Which face the defect shows depends on the build, and all three were checked
// rather than assumed:
//
//   * Unoptimised (`make gd`, and every ASan preset — they all inherit the debug
//     one): libstdc++ turns on _GLIBCXX_ASSERTIONS, so `operator[]` aborts the
//     process. A malformed asset takes the importer down outright. That is the
//     starkest demonstration, but an abort is not something doctest can report a
//     case on, so the cases below are not compiled there.
//   * Optimised, on a real glTF (`make gs`, no spare capacity): a
//     heap-buffer-overflow read past the vertex array.
//   * Optimised, on the mesh below (spare capacity from `reserve`): the read
//     stays inside the allocation and simply returns the wrong bounds. That is
//     what the cases assert on, and it is the only face that can be reported as
//     a failing test rather than a dead process.
//
// Note what that costs: because every ASan preset inherits the *unoptimised*
// one, ASan never sees this — the libstdc++ assertion fires first — and the
// `reserve` above would hide it from ASan even if a preset did combine the two.
// Verified separately outside the build: the same access at -O2 with
// -fsanitize=address and no reserve reports a clean heap-buffer-overflow.
//
// None of the three is acceptable and all have the same one-line fix, so the
// coverage is gated rather than dropped.
#if !defined(_GLIBCXX_ASSERTIONS) && (!defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL == 0)

TEST_CASE("ComputeAabb: an index that names no vertex returns the zero box" * doctest::should_fail())
{
    // The zero box is the assertion because it is what the doc comment already
    // promises for "out-of-range or empty input"; refusing the whole fit is the
    // only answer that keeps a submesh's bounds meaningful.
    //
    // should_fail until the index check lands; the fix removes this decorator.
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 3u, 1u}; // 3 names no vertex — the mesh has two

    const Aabb box = ComputeAabb(mesh, 0, 3);
    CHECK(box.min.x == doctest::Approx(0.f));
    CHECK(box.max.x == doctest::Approx(0.f));
}

TEST_CASE("ComputeBoundingSphere: an index that names no vertex returns the zero sphere" *
          doctest::should_fail())
{
    // The same defect in the sibling fitter, which repeats the range-only check
    // and the unchecked dereference. Both are called per submesh at import, so a
    // fix has to cover the pair.
    //
    // should_fail until the index check lands; the fix removes this decorator.
    MeshData mesh = TwoVertexMesh();
    mesh.Indices  = {0u, 3u, 1u};

    const BoundingSphere sphere = ComputeBoundingSphere(mesh, 0, 3);
    CHECK(sphere.radius == doctest::Approx(0.f));
    CHECK(sphere.center.x == doctest::Approx(0.f));
}

#endif // unhardened operator[]
