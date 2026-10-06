/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCollisionImport.cpp
/// @brief Collision authored as prefixed nodes in a model: kept out of what is
/// drawn, welded and split into its parts, fitted where it asks for a primitive,
/// and carried through the cooked mesh.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Geometry/CollisionData.hpp>
#include <Assisi/Geometry/CookedMesh.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>

#include "LogCapture.hpp"

using namespace Assisi;
using Geometry::CollisionPieceKind;

namespace fs = std::filesystem;

namespace
{

/// The tolerance a fitted dimension is compared at.
constexpr double kFitEpsilon = 1e-4;

/// One node of a test model, with a mesh of its own.
struct TestNode
{
    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 translation{0.f};
    glm::vec3 scale{1.f};
};

/// A cube as an exporter writes a flat-shaded one: four corners per face, so
/// each corner appears three times, and two triangles per face.
void AppendFlatCube(TestNode &node, glm::vec3 centre, glm::vec3 half)
{
    // Each face: the axis it faces along and its sign.
    constexpr std::int32_t kFaces = 6;
    for (std::int32_t face = 0; face < kFaces; ++face)
    {
        const std::int32_t axis = face / 2;
        const float sign = face % 2 == 0 ? 1.f : -1.f;
        const std::int32_t u = (axis + 1) % 3;
        const std::int32_t v = (axis + 2) % 3;
        const std::uint32_t base = static_cast<std::uint32_t>(node.positions.size());
        for (const glm::vec2 corner : {glm::vec2(-1.f, -1.f), glm::vec2(1.f, -1.f), glm::vec2(1.f, 1.f),
                                       glm::vec2(-1.f, 1.f)})
        {
            glm::vec3 offset{0.f};
            offset[axis] = sign;
            offset[u] = corner.x;
            offset[v] = corner.y;
            node.positions.push_back(centre + offset * half);
        }
        for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
        {
            node.indices.push_back(base + index);
        }
    }
}

TestNode FlatCube(std::string name, glm::vec3 half)
{
    TestNode node;
    node.name = std::move(name);
    AppendFlatCube(node, glm::vec3(0.f), half);
    return node;
}

std::string Vec(glm::vec3 v)
{
    return std::format("[{}, {}, {}]", v.x, v.y, v.z);
}

/// Writes @p nodes as `model.gltf` and `model.bin` in a fresh asset root, and
/// points AssetSystem at it.
fs::path WriteModel(std::string_view directory, std::span<const TestNode> nodes)
{
    const fs::path root = fs::temp_directory_path() / directory;
    fs::remove_all(root);
    fs::create_directories(root);

    std::vector<std::byte> bin;
    std::string nodeJson;
    std::string meshJson;
    std::string viewJson;
    std::string accessorJson;
    std::string sceneNodes;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const TestNode &node = nodes[i];
        const std::string separator = i == 0 ? "" : ",";
        glm::vec3 min{1e30f};
        glm::vec3 max{-1e30f};
        for (const glm::vec3 &position : node.positions)
        {
            min = glm::min(min, position);
            max = glm::max(max, position);
        }

        const std::size_t positionOffset = bin.size();
        const std::size_t positionBytes = node.positions.size() * sizeof(glm::vec3);
        bin.resize(positionOffset + positionBytes);
        std::memcpy(bin.data() + positionOffset, node.positions.data(), positionBytes);
        const std::size_t indexOffset = bin.size();
        const std::size_t indexBytes = node.indices.size() * sizeof(std::uint32_t);
        bin.resize(indexOffset + indexBytes);
        std::memcpy(bin.data() + indexOffset, node.indices.data(), indexBytes);

        sceneNodes += std::format("{}{}", separator, i);
        nodeJson += std::format(R"({}{{ "mesh": {}, "name": "{}", "translation": {}, "rotation": [{}, {}, {}, {}],
                                "scale": {} }})",
                                separator, i, node.name, Vec(node.translation), node.rotation.x, node.rotation.y,
                                node.rotation.z, node.rotation.w, Vec(node.scale));
        meshJson += std::format(R"({}{{ "primitives": [ {{ "attributes": {{ "POSITION": {} }}, "indices": {} }} ] }})",
                                separator, 2 * i, 2 * i + 1);
        viewJson += std::format(R"({}{{ "buffer": 0, "byteOffset": {}, "byteLength": {} }},
                                {{ "buffer": 0, "byteOffset": {}, "byteLength": {} }})",
                                separator, positionOffset, positionBytes, indexOffset, indexBytes);
        accessorJson += std::format(
            R"({}{{ "bufferView": {}, "componentType": 5126, "count": {}, "type": "VEC3", "min": {}, "max": {} }},
            {{ "bufferView": {}, "componentType": 5125, "count": {}, "type": "SCALAR" }})",
            separator, 2 * i, node.positions.size(), Vec(min), Vec(max), 2 * i + 1, node.indices.size());
    }

    const std::string gltf = std::format(
        R"({{ "asset": {{ "version": "2.0" }}, "scene": 0, "scenes": [ {{ "nodes": [ {} ] }} ],
           "nodes": [ {} ], "meshes": [ {} ],
           "buffers": [ {{ "uri": "model.bin", "byteLength": {} }} ],
           "bufferViews": [ {} ], "accessors": [ {} ] }})",
        sceneNodes, nodeJson, meshJson, bin.size(), viewJson, accessorJson);

    std::ofstream(root / "model.gltf", std::ios::binary).write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
    std::ofstream(root / "model.bin", std::ios::binary)
    .write(reinterpret_cast<const char *>(bin.data()), static_cast<std::streamsize>(bin.size()));

    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    return root;
}

/// Imports @p nodes as a model.
std::expected<Geometry::MeshData, Geometry::MeshImportError> Import(std::string_view directory,
                                                                    std::span<const TestNode> nodes)
{
    const fs::path root = WriteModel(directory, nodes);
    std::expected<Geometry::MeshData, Geometry::MeshImportError> imported = Geometry::ImportMesh("model.gltf");
    fs::remove_all(root);
    return imported;
}

void CheckVec(glm::vec3 actual, glm::vec3 expected)
{
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(kFitEpsilon));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(kFitEpsilon));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(kFitEpsilon));
}

/// Where @p rotation sends local Y.
glm::vec3 AxisOf(glm::quat rotation)
{
    return rotation * glm::vec3(0.f, 1.f, 0.f);
}

} // namespace

TEST_CASE("CollisionPrefixKind: reads Unreal's prefixes in any case, and nothing else")
{
    CHECK(Geometry::CollisionPrefixKind("UCX_Chair") == CollisionPieceKind::Hull);
    CHECK(Geometry::CollisionPrefixKind("ubx_seat") == CollisionPieceKind::Box);
    CHECK(Geometry::CollisionPrefixKind("USP_Ball") == CollisionPieceKind::Sphere);
    CHECK(Geometry::CollisionPrefixKind("UCP_Leg") == CollisionPieceKind::Capsule);
    CHECK(Geometry::CollisionPrefixKind("UCY_Post") == CollisionPieceKind::Cylinder);
    CHECK_FALSE(Geometry::CollisionPrefixKind("Chair").has_value());
    CHECK_FALSE(Geometry::CollisionPrefixKind("Chair_UCX_").has_value());
    CHECK_FALSE(Geometry::CollisionPrefixKind("UCX").has_value());
}

TEST_CASE("ImportMesh: a UCX_ node is collision, not drawn")
{
    const std::vector<TestNode> nodes{FlatCube("Cube", glm::vec3(0.5f)), FlatCube("UCX_Cube", glm::vec3(0.6f))};
    const std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_strip", nodes);
    REQUIRE(mesh.has_value());

    // Only the visual cube is drawn.
    CHECK(mesh->Vertices.size() == 24);
    CHECK(mesh->LocalAabb.max.x == doctest::Approx(0.5f));

    // The collision cube's 24 written corners are its 8 real ones.
    REQUIRE(mesh->Collision.pieces.size() == 1);
    const Geometry::CollisionPiece &hull = mesh->Collision.pieces.front();
    CHECK(hull.kind == CollisionPieceKind::Hull);
    CHECK(hull.name == "UCX_Cube");
    CHECK(hull.pointCount == 8);
    CHECK(mesh->Collision.points.size() == 8);
}

TEST_CASE("ImportMesh: one UCX_ node holding two separate blocks becomes two hulls, in authored order")
{
    TestNode two;
    two.name = "UCX_Two";
    AppendFlatCube(two, glm::vec3(-2.f, 0.f, 0.f), glm::vec3(0.5f));
    AppendFlatCube(two, glm::vec3(2.f, 0.f, 0.f), glm::vec3(0.5f));
    const std::vector<TestNode> nodes{FlatCube("Cube", glm::vec3(0.5f)), two};

    const std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_split", nodes);
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->Collision.pieces.size() == 2);

    const Geometry::CollisionPiece &first = mesh->Collision.pieces[0];
    const Geometry::CollisionPiece &second = mesh->Collision.pieces[1];
    CHECK(first.name == "UCX_Two_1");
    CHECK(second.name == "UCX_Two_2");
    CHECK(first.pointCount == 8);
    CHECK(second.pointCount == 8);

    // The first block written is the first piece.
    for (std::uint32_t i = 0; i < first.pointCount; ++i)
    {
        CHECK(mesh->Collision.points[first.firstPoint + i].x < 0.f);
        CHECK(mesh->Collision.points[second.firstPoint + i].x > 0.f);
    }
}

TEST_CASE("ImportMesh: a UBX_ node becomes a box fitted to it, placed by the node")
{
    TestNode box = FlatCube("UBX_Seat", glm::vec3(0.5f, 0.1f, 0.5f));
    box.translation = glm::vec3(1.f, 2.f, 3.f);
    box.rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f));
    box.scale = glm::vec3(2.f, 1.f, 1.f);
    const std::vector<TestNode> nodes{FlatCube("Chair", glm::vec3(0.5f)), box};

    const std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_box", nodes);
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->Collision.pieces.size() == 1);
    const Geometry::CollisionPiece &piece = mesh->Collision.pieces.front();
    CHECK(piece.kind == CollisionPieceKind::Box);
    CHECK(piece.pointCount == 0);
    CheckVec(piece.halfExtents, glm::vec3(1.f, 0.1f, 0.5f));
    CheckVec(piece.position, glm::vec3(1.f, 2.f, 3.f));
    CHECK(glm::abs(glm::dot(piece.rotation, box.rotation)) == doctest::Approx(1.f).epsilon(kFitEpsilon));
}

TEST_CASE("ImportMesh: sphere, capsule and cylinder nodes are fitted along the part's longest axis")
{
    TestNode sphere = FlatCube("USP_Ball", glm::vec3(0.5f));
    sphere.translation = glm::vec3(0.f, 5.f, 0.f);
    TestNode capsule = FlatCube("UCP_Leg", glm::vec3(0.25f, 1.f, 0.25f));
    TestNode cylinder = FlatCube("UCY_Beam", glm::vec3(2.f, 0.3f, 0.2f));
    const std::vector<TestNode> nodes{FlatCube("Chair", glm::vec3(0.5f)), sphere, capsule, cylinder};

    const std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_round", nodes);
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->Collision.pieces.size() == 3);

    const Geometry::CollisionPiece &ball = mesh->Collision.pieces[0];
    CHECK(ball.kind == CollisionPieceKind::Sphere);
    CheckVec(ball.position, glm::vec3(0.f, 5.f, 0.f));
    // The sphere encloses the cube: out to its corners.
    CHECK(ball.radius == doctest::Approx(std::sqrt(0.75f)).epsilon(kFitEpsilon));

    const Geometry::CollisionPiece &leg = mesh->Collision.pieces[1];
    CHECK(leg.kind == CollisionPieceKind::Capsule);
    CheckVec(AxisOf(leg.rotation), glm::vec3(0.f, 1.f, 0.f));
    CHECK(leg.radius == doctest::Approx(0.25f).epsilon(kFitEpsilon));
    // The caps take the radius off each end of the straight part.
    CHECK(leg.halfHeight == doctest::Approx(0.75f).epsilon(kFitEpsilon));

    const Geometry::CollisionPiece &beam = mesh->Collision.pieces[2];
    CHECK(beam.kind == CollisionPieceKind::Cylinder);
    CheckVec(glm::abs(AxisOf(beam.rotation)), glm::vec3(1.f, 0.f, 0.f));
    CHECK(beam.radius == doctest::Approx(0.3f).epsilon(kFitEpsilon));
    CHECK(beam.halfHeight == doctest::Approx(2.f).epsilon(kFitEpsilon));
}

TEST_CASE("ImportMesh: a flat UCX_ part fails the import, naming the part")
{
    TestNode flat;
    flat.name = "UCX_Flat";
    flat.positions = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 0.f, 1.f}, {0.f, 0.f, 1.f}};
    flat.indices = {0, 1, 2, 0, 2, 3};
    const std::vector<TestNode> nodes{FlatCube("Cube", glm::vec3(0.5f)), flat};

    const Tests::LogCapture log;
    const std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_flat", nodes);
    REQUIRE_FALSE(mesh.has_value());
    CHECK(mesh.error() == Geometry::MeshImportError::InvalidCollision);
    CHECK(log.Mentions("UCX_Flat"));
}

TEST_CASE("CollisionModelOf: the first level's triangles, with exporter-duplicated corners merged")
{
    const std::vector<TestNode> nodes{FlatCube("Cube", glm::vec3(0.5f))};
    std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_model", nodes);
    REQUIRE(mesh.has_value());
    Geometry::EnsureSubMeshTables(*mesh);

    const Geometry::CollisionModel model = Geometry::CollisionModelOf(*mesh);
    CHECK(model.positions.size() == 8);
    CHECK(model.indices.size() == 36);
}

TEST_CASE("Cooked mesh: the collision pieces read back as written")
{
    TestNode two;
    two.name = "UCX_Two";
    AppendFlatCube(two, glm::vec3(-2.f, 0.f, 0.f), glm::vec3(0.5f));
    AppendFlatCube(two, glm::vec3(2.f, 0.f, 0.f), glm::vec3(0.5f));
    TestNode leg = FlatCube("UCP_Leg", glm::vec3(0.25f, 1.f, 0.25f));
    leg.translation = glm::vec3(0.f, -1.f, 0.f);
    const std::vector<TestNode> nodes{FlatCube("Cube", glm::vec3(0.5f)), two, leg};
    std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Import("assisi_collision_cook", nodes);
    REQUIRE(mesh.has_value());
    Geometry::EnsureSubMeshTables(*mesh);

    Core::BitWriter writer;
    const std::vector<Core::AssetId> slots(mesh->Materials.size());
    Geometry::WriteCookedMesh(writer, *mesh, slots);
    const std::expected<Geometry::CookedMesh, Geometry::CookedMeshError> cooked =
        Geometry::ReadCookedMesh(writer.Data());
    REQUIRE(cooked.has_value());

    const Geometry::CollisionData &written = mesh->Collision;
    const Geometry::CollisionData &read = cooked->mesh.Collision;
    REQUIRE(read.pieces.size() == written.pieces.size());
    CHECK(read.points == written.points);
    for (std::size_t i = 0; i < read.pieces.size(); ++i)
    {
        CHECK(read.pieces[i].name == written.pieces[i].name);
        CHECK(read.pieces[i].kind == written.pieces[i].kind);
        CHECK(read.pieces[i].rotation == written.pieces[i].rotation);
        CHECK(read.pieces[i].position == written.pieces[i].position);
        CHECK(read.pieces[i].halfExtents == written.pieces[i].halfExtents);
        CHECK(read.pieces[i].radius == written.pieces[i].radius);
        CHECK(read.pieces[i].halfHeight == written.pieces[i].halfHeight);
        CHECK(read.pieces[i].firstPoint == written.pieces[i].firstPoint);
        CHECK(read.pieces[i].pointCount == written.pieces[i].pointCount);
    }
}
