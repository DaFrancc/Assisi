/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSkinImport.cpp
/// @brief A skinned glTF imports with its skeleton and per-vertex weights, and a
/// skin that cannot form one skeleton is refused by name.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Math/GLM.hpp>

#include "LogCapture.hpp"

using Assisi::Core::AssetSystem;
using Assisi::Geometry::ImportMesh;
using Assisi::Geometry::kNoParent;
using Assisi::Geometry::MeshData;
using Assisi::Geometry::MeshImportError;
using Assisi::Geometry::VertexSkin;

namespace fs = std::filesystem;

namespace
{

/// glTF accessor component types.
constexpr uint32_t kFloat         = 5126;
constexpr uint32_t kUnsignedByte  = 5121;
constexpr uint32_t kUnsignedShort = 5123;

/// glTF requires every accessor to start on a multiple of its component size;
/// four covers every type these fixtures use.
constexpr std::size_t kViewAlignment = 4;

/// The −90° turn about X a Z-up exporter puts on its root joint, as a quaternion.
constexpr float kQuarterTurnComponent = 0.70710678f;

uint32_t ComponentsOf(std::string_view type)
{
    if (type == "MAT4")
    {
        return 16;
    }
    if (type == "VEC4")
    {
        return 4;
    }
    if (type == "VEC3")
    {
        return 3;
    }
    return 1;
}

/// The binary buffer of a fixture glTF and the accessors over it, built in the
/// order a test adds them so the test can name each accessor by the index Add
/// returns.
class GltfBuffer
{
public:
    template <typename T>
    uint32_t Add(const std::vector<T> &values, uint32_t componentType, std::string_view type,
                 std::string_view extraKeys = {})
    {
        while (_bytes.size() % kViewAlignment != 0)
        {
            _bytes.push_back(std::byte{0});
        }
        const std::size_t offset = _bytes.size();
        const std::size_t length = values.size() * sizeof(T);
        _bytes.resize(offset + length);
        std::memcpy(_bytes.data() + offset, values.data(), length);

        const uint32_t index = _accessorCount++;
        const char *separator = index == 0 ? "" : ",";
        _views += std::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}}})", separator, offset, length);
        _accessors += std::format(R"({}{{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})", separator,
                                  index, componentType, values.size() / ComponentsOf(type), type, extraKeys);
        return index;
    }

    /// Writes `skin.gltf` and `skin.bin` into a fresh asset root. @p sceneKeys
    /// holds the "scenes", "nodes", "meshes" and "skins" members.
    fs::path Write(std::string_view sceneKeys) const
    {
        const fs::path root = fs::temp_directory_path() / "assisi_skin_import_test";
        fs::remove_all(root);
        fs::create_directories(root);

        const std::string gltf =
            std::format(R"({{"asset":{{"version":"2.0"}},"scene":0,{},"buffers":[{{"uri":"skin.bin","byteLength":{}}}],)"
                        R"("bufferViews":[{}],"accessors":[{}]}})",
                        sceneKeys, _bytes.size(), _views, _accessors);
        {
            std::ofstream file(root / "skin.gltf", std::ios::binary);
            file.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
        }
        {
            std::ofstream file(root / "skin.bin", std::ios::binary);
            file.write(reinterpret_cast<const char *>(_bytes.data()), static_cast<std::streamsize>(_bytes.size()));
        }
        REQUIRE(AssetSystem::SetRoot(root).has_value());
        return root;
    }

private:
    std::vector<std::byte> _bytes;
    std::string _views;
    std::string _accessors;
    uint32_t _accessorCount = 0;
};

std::vector<float> Floats(const glm::mat4 &matrix)
{
    // glTF stores matrices column-major, as glm indexes them.
    std::vector<float> values;
    for (int32_t column = 0; column < 4; ++column)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            values.push_back(matrix[column][row]);
        }
    }
    return values;
}

/// One triangle: (0,0,0), (1,0,0), (0,1,0).
struct Triangle
{
    uint32_t positions = 0;
    uint32_t indices   = 0;
};

Triangle AddTriangle(GltfBuffer &buffer)
{
    Triangle triangle;
    triangle.positions = buffer.Add(std::vector<float>{0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}, kFloat, "VEC3",
                                    R"(,"min":[0,0,0],"max":[1,1,0])");
    triangle.indices   = buffer.Add(std::vector<uint16_t>{0, 1, 2}, kUnsignedShort, "SCALAR");
    return triangle;
}

std::string SkinnedPrimitive(const Triangle &triangle, uint32_t joints, uint32_t weights)
{
    return std::format(R"({{"attributes":{{"POSITION":{},"JOINTS_0":{},"WEIGHTS_0":{}}},"indices":{}}})",
                       triangle.positions, joints, weights, triangle.indices);
}

/// The fixture most cases share: an "Armature" node, translated, over a root
/// joint carrying an exporter's −90° X turn and a translated child joint, plus a
/// skinned mesh node with a transform of its own. Vertex 0 follows the root,
/// vertex 1 the child, vertex 2 both equally.
struct ArmatureFixture
{
    glm::mat4 rootInverseBind  = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, -1.f));
    glm::mat4 childInverseBind = glm::translate(glm::mat4(1.f), glm::vec3(0.f, -1.f, 0.f));
    bool childListedFirst      = false;
};

fs::path WriteArmature(const ArmatureFixture &fixture)
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);

    // Joint numbers are positions in skin.joints, so they follow the listing order.
    const uint8_t root  = fixture.childListedFirst ? 1 : 0;
    const uint8_t child = fixture.childListedFirst ? 0 : 1;
    const uint32_t joints =
        buffer.Add(std::vector<uint8_t>{root, 0, 0, 0, child, 0, 0, 0, root, child, 0, 0}, kUnsignedByte, "VEC4");
    const uint32_t weights = buffer.Add(
        std::vector<float>{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.5f, 0.5f, 0.f, 0.f}, kFloat, "VEC4");

    std::vector<float> inverseBinds = Floats(fixture.childListedFirst ? fixture.childInverseBind
                                                                      : fixture.rootInverseBind);
    const std::vector<float> second = Floats(fixture.childListedFirst ? fixture.rootInverseBind
                                                                      : fixture.childInverseBind);
    inverseBinds.insert(inverseBinds.end(), second.begin(), second.end());
    const uint32_t inverseBindAccessor = buffer.Add(inverseBinds, kFloat, "MAT4");

    const std::string skinJoints = fixture.childListedFirst ? "[2,1]" : "[1,2]";
    return buffer.Write(std::format(
                            R"("scenes":[{{"nodes":[0]}}],)"
                            R"("nodes":[{{"name":"Armature","translation":[0,0,5],"children":[1,3]}},)"
                            R"({{"name":"root","rotation":[-{},0,0,{}],"children":[2]}},)"
                            R"({{"name":"child","translation":[0,1,0]}},)"
                            R"({{"name":"Body","mesh":0,"skin":0,"translation":[10,0,0]}}],)"
                            R"("meshes":[{{"primitives":[{}]}}],)"
                            R"("skins":[{{"joints":{},"inverseBindMatrices":{}}}])",
                            kQuarterTurnComponent, kQuarterTurnComponent, SkinnedPrimitive(triangle, joints, weights), skinJoints,
                            inverseBindAccessor));
}

void CheckMatrix(const glm::mat4 &actual, const glm::mat4 &expected)
{
    for (int32_t column = 0; column < 4; ++column)
    {
        for (int32_t row = 0; row < 4; ++row)
        {
            CHECK(actual[column][row] == doctest::Approx(expected[column][row]));
        }
    }
}

float WeightSum(const VertexSkin &skin)
{
    return skin.Weights.x + skin.Weights.y + skin.Weights.z + skin.Weights.w;
}

/// A flat skeleton of @p jointCount joints under "Armature", and one skinned
/// triangle whose influences the caller supplies as JOINTS_0/WEIGHTS_0 and,
/// when given, JOINTS_1/WEIGHTS_1.
struct InfluenceFixture
{
    std::vector<uint16_t> joints0;
    std::vector<float> weights0;
    std::vector<uint16_t> joints1;
    std::vector<float> weights1;
    uint32_t jointCount = 1;
};

fs::path WriteInfluences(const InfluenceFixture &fixture)
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const uint32_t joints0  = buffer.Add(fixture.joints0, kUnsignedShort, "VEC4");
    const uint32_t weights0 = buffer.Add(fixture.weights0, kFloat, "VEC4");
    std::string secondSet;
    if (!fixture.joints1.empty())
    {
        const uint32_t joints1  = buffer.Add(fixture.joints1, kUnsignedShort, "VEC4");
        const uint32_t weights1 = buffer.Add(fixture.weights1, kFloat, "VEC4");
        secondSet = std::format(R"(,"JOINTS_1":{},"WEIGHTS_1":{})", joints1, weights1);
    }

    std::string jointNodes;
    std::string jointList;
    std::string children;
    for (uint32_t joint = 0; joint < fixture.jointCount; ++joint)
    {
        const char *separator = joint == 0 ? "" : ",";
        jointNodes += std::format(R"({}{{"name":"joint{}"}})", separator, joint);
        jointList += std::format("{}{}", separator, joint + 1);
        children += std::format("{}{}", separator, joint + 1);
    }
    const uint32_t meshNode = fixture.jointCount + 1;

    return buffer.Write(std::format(
                            R"("scenes":[{{"nodes":[0]}}],)"
                            R"("nodes":[{{"name":"Armature","children":[{},{}]}},{},{{"name":"Body","mesh":0,"skin":0}}],)"
                            R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"JOINTS_0":{},"WEIGHTS_0":{}{}}},"indices":{}}}]}}],)"
                            R"("skins":[{{"joints":[{}]}}])",
                            children, meshNode, jointNodes, triangle.positions, joints0, weights0, secondSet, triangle.indices,
                            jointList));
}

} // namespace

TEST_CASE("ImportMesh: a skinned mesh keeps its skeleton and stays in bind space")
{
    const ArmatureFixture fixture;
    const fs::path root = WriteArmature(fixture);

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE(result.has_value());
    const MeshData &mesh = *result;

    REQUIRE(mesh.Skeleton.JointCount() == 2);
    CHECK(mesh.Skeleton.Names == std::vector<std::string>{"root", "child"});
    CHECK(mesh.Skeleton.Parents == std::vector<int32_t>{kNoParent, 0});

    // The root keeps its own turn; the Armature above it is the skeleton's root
    // transform, not part of the joint.
    CHECK(mesh.Skeleton.RestLocal[0].Rotation.w == doctest::Approx(kQuarterTurnComponent));
    CHECK(mesh.Skeleton.RestLocal[0].Rotation.x == doctest::Approx(-kQuarterTurnComponent));
    CHECK(mesh.Skeleton.RestLocal[1].Translation.y == doctest::Approx(1.f));
    CheckMatrix(mesh.Skeleton.RootTransform, glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 5.f)));
    CheckMatrix(mesh.Skeleton.InverseBind[0], fixture.rootInverseBind);
    CheckMatrix(mesh.Skeleton.InverseBind[1], fixture.childInverseBind);

    // The mesh node's own translation of 10 is not baked: skinned vertices are in
    // bind space, and the joints place them.
    REQUIRE(mesh.Vertices.size() == 3);
    CHECK(mesh.Vertices[1].Position.x == doctest::Approx(1.f));

    REQUIRE(mesh.Skin.size() == mesh.Vertices.size());
    CHECK(mesh.Skin[0].Joints.x == 0);
    CHECK(mesh.Skin[1].Joints.x == 1);
    CHECK(mesh.Skin[2].Weights.x == doctest::Approx(0.5f));
    CHECK(mesh.Skin[2].Weights.y == doctest::Approx(0.5f));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: joints listed child-first come out parent-first, and vertices follow them")
{
    ArmatureFixture fixture;
    fixture.childListedFirst = true;
    const fs::path root      = WriteArmature(fixture);

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE(result.has_value());
    const MeshData &mesh = *result;

    CHECK(mesh.Skeleton.Names == std::vector<std::string>{"root", "child"});
    CHECK(mesh.Skeleton.Parents == std::vector<int32_t>{kNoParent, 0});
    CheckMatrix(mesh.Skeleton.InverseBind[1], fixture.childInverseBind);

    // Vertex 1 is bound to "child" in the file, and still is after the reorder.
    REQUIRE(mesh.Skin.size() == 3);
    CHECK(mesh.Skeleton.Names[mesh.Skin[1].Joints.x] == "child");
    CHECK(mesh.Skeleton.Names[mesh.Skin[0].Joints.x] == "root");

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a vertex bound to more than four joints keeps the four largest, renormalized, and says so once")
{
    InfluenceFixture fixture;
    fixture.jointCount = 5;
    fixture.joints0    = {0, 1, 2, 3, 0, 1, 2, 3, 0, 0, 0, 0};
    fixture.weights0   = {0.4f, 0.3f, 0.2f, 0.05f, 0.4f, 0.3f, 0.2f, 0.05f, 1.f, 0.f, 0.f, 0.f};
    fixture.joints1    = {4, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0};
    fixture.weights1   = {0.04f, 0.f, 0.f, 0.f, 0.04f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const fs::path root = WriteInfluences(fixture);

    const Assisi::Tests::LogCapture log;
    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Skin.size() == 3);

    // Joint 4 carries the smallest weight, so it is the one dropped; the rest are
    // scaled back up to a whole.
    constexpr float kKeptTotal = 0.4f + 0.3f + 0.2f + 0.05f;
    for (uint32_t vertex = 0; vertex < 2; ++vertex)
    {
        CAPTURE(vertex);
        const VertexSkin &skin = result->Skin[vertex];
        CHECK(WeightSum(skin) == doctest::Approx(1.f));
        for (uint32_t slot = 0; slot < 4; ++slot)
        {
            CHECK(skin.Joints[static_cast<int32_t>(slot)] != 4);
            if (skin.Joints[static_cast<int32_t>(slot)] == 0)
            {
                CHECK(skin.Weights[static_cast<int32_t>(slot)] == doctest::Approx(0.4f / kKeptTotal));
            }
        }
    }
    CHECK(log.Count("largest weights are kept") == 1);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a vertex with no weight follows joint 0, and says so once")
{
    InfluenceFixture fixture;
    fixture.jointCount = 2;
    fixture.joints0    = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    fixture.weights0   = {1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const fs::path root = WriteInfluences(fixture);

    const Assisi::Tests::LogCapture log;
    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE(result.has_value());
    REQUIRE(result->Skin.size() == 3);

    CHECK(result->Skin[0].Joints.x == 1);
    for (uint32_t vertex = 1; vertex < 3; ++vertex)
    {
        CAPTURE(vertex);
        CHECK(result->Skin[vertex].Joints.x == 0);
        CHECK(result->Skin[vertex].Weights.x == doctest::Approx(1.f));
        CHECK(WeightSum(result->Skin[vertex]) == doctest::Approx(1.f));
    }
    CHECK(log.Count("no weight") == 1);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a static mesh imports with no skin and no skeleton")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const fs::path root     = buffer.Write(std::format(
                                               R"("scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0,"translation":[10,0,0]}}],)"
                                               R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{}}},"indices":{}}}]}}])",
                                               triangle.positions, triangle.indices));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE(result.has_value());
    CHECK(result->Skin.empty());
    CHECK(result->Skeleton.Empty());
    // A static node is still baked into the scene.
    CHECK(result->Vertices[1].Position.x == doctest::Approx(11.f));

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: skinned and unskinned mesh nodes in one file are refused")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const uint32_t joints   = buffer.Add(std::vector<uint8_t>(12, 0), kUnsignedByte, "VEC4");
    const uint32_t weights  = buffer.Add(std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, kFloat, "VEC4");
    const fs::path root     = buffer.Write(std::format(
                                               R"("scenes":[{{"nodes":[0]}}],)"
                                               R"("nodes":[{{"name":"Armature","children":[1,2,3]}},{{"name":"root"}},{{"mesh":0,"skin":0}},{{"mesh":1}}],)"
                                               R"("meshes":[{{"primitives":[{}]}},{{"primitives":[{{"attributes":{{"POSITION":{}}},"indices":{}}}]}}],)"
                                               R"("skins":[{{"joints":[1]}}])",
                                               SkinnedPrimitive(triangle, joints, weights), triangle.positions, triangle.indices));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::MixedSkinning);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: mesh nodes bound to two different skins are refused")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const uint32_t joints   = buffer.Add(std::vector<uint8_t>(12, 0), kUnsignedByte, "VEC4");
    const uint32_t weights  = buffer.Add(std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, kFloat, "VEC4");
    const fs::path root     = buffer.Write(std::format(
                                               R"("scenes":[{{"nodes":[0]}}],)"
                                               R"("nodes":[{{"name":"Armature","children":[1,2,3]}},{{"name":"root"}},{{"mesh":0,"skin":0}},)"
                                               R"({{"mesh":0,"skin":1}}],)"
                                               R"("meshes":[{{"primitives":[{}]}}],)"
                                               R"("skins":[{{"joints":[1]}},{{"joints":[1]}}])",
                                               SkinnedPrimitive(triangle, joints, weights)));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::MultipleSkins);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a vertex naming a joint its skin does not have is refused")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    // The skin has one joint, so joint 1 does not exist.
    const uint32_t joints  = buffer.Add(std::vector<uint8_t>{0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, kUnsignedByte, "VEC4");
    const uint32_t weights = buffer.Add(std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, kFloat, "VEC4");
    const fs::path root    = buffer.Write(std::format(
                                              R"("scenes":[{{"nodes":[0]}}],)"
                                              R"("nodes":[{{"name":"Armature","children":[1,2]}},{{"name":"root"}},{{"mesh":0,"skin":0}}],)"
                                              R"("meshes":[{{"primitives":[{}]}}],)"
                                              R"("skins":[{{"joints":[1]}}])",
                                              SkinnedPrimitive(triangle, joints, weights)));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::JointOutOfRange);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: two joints with the same name are refused")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const uint32_t joints   = buffer.Add(std::vector<uint8_t>(12, 0), kUnsignedByte, "VEC4");
    const uint32_t weights  = buffer.Add(std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, kFloat, "VEC4");
    const fs::path root     = buffer.Write(std::format(
                                               R"("scenes":[{{"nodes":[0]}}],)"
                                               R"("nodes":[{{"name":"Armature","children":[1,3]}},{{"name":"spine","children":[2]}},{{"name":"spine"}},)"
                                               R"({{"mesh":0,"skin":0}}],)"
                                               R"("meshes":[{{"primitives":[{}]}}],)"
                                               R"("skins":[{{"joints":[1,2]}}])",
                                               SkinnedPrimitive(triangle, joints, weights)));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::DuplicateJointName);

    fs::remove_all(root);
}

TEST_CASE("ImportMesh: a skinned primitive without weights is refused")
{
    GltfBuffer buffer;
    const Triangle triangle = AddTriangle(buffer);
    const uint32_t joints   = buffer.Add(std::vector<uint8_t>(12, 0), kUnsignedByte, "VEC4");
    const fs::path root     = buffer.Write(std::format(
                                               R"("scenes":[{{"nodes":[0]}}],)"
                                               R"("nodes":[{{"name":"Armature","children":[1,2]}},{{"name":"root"}},{{"mesh":0,"skin":0}}],)"
                                               R"("meshes":[{{"primitives":[{{"attributes":{{"POSITION":{},"JOINTS_0":{}}},"indices":{}}}]}}],)"
                                               R"("skins":[{{"joints":[1]}}])",
                                               triangle.positions, joints, triangle.indices));

    const std::expected<MeshData, MeshImportError> result = ImportMesh("skin.gltf");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == MeshImportError::InvalidSkin);

    fs::remove_all(root);
}
