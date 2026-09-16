/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedScene.cpp
/// @brief The binary level format: what a scene cooks to, and that decoding the
/// blocks rebuilds the scene it came from.
///
/// The round trip is the only honest test of a writer nothing reads yet. Cooking
/// a scene, decoding every block back onto fresh entities, and comparing the
/// result against the source is what would catch an off-by-one in the name
/// table, a field the codec dropped, or a reference wired to the wrong entity —
/// none of which any framing check can see.

#include <doctest/doctest.h>

#include <ostream>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/CookedScene.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::CookedScene;
using Assisi::Runtime::DecodeCookedScene;
using Assisi::Runtime::LevelError;
using Assisi::Runtime::LevelHeader;
using Assisi::Runtime::Name;
using Assisi::Runtime::Parent;
using Assisi::Runtime::PointLight;
using Assisi::Runtime::SaveCookedScene;
using Assisi::Runtime::Transform;

namespace
{

/// Three entities, one of which points at another, with values no default
/// constructor produces — so a field the codec dropped comes back as a default
/// and the comparison fails.
///
/// Fills a caller's scene rather than returning one: ECS::Scene is deliberately
/// neither copyable nor movable, since entity handles name slots inside it.
void FillScene(ECS::Scene &scene)
{
    const ECS::Entity body = scene.Create();
    (void)scene.Add<Name>(body, Name{Core::EntityName{"body"}});
    Transform bodyTransform;
    bodyTransform.position = {1.5f, -2.25f, 3.125f};
    bodyTransform.scale    = {2.f, 2.f, 2.f};
    (void)scene.Add<Transform>(body, bodyTransform);

    const ECS::Entity lamp = scene.Create();
    (void)scene.Add<Name>(lamp, Name{Core::EntityName{"lamp"}});
    Transform lampTransform;
    lampTransform.position = {0.f, 4.f, 0.f};
    (void)scene.Add<Transform>(lamp, lampTransform);
    PointLight light;
    light.intensity = 250.f;
    light.radius    = 12.5f;
    (void)scene.Add<PointLight>(lamp, light);

    // Points at an entity created before it, and — once the map sorts — possibly
    // written after it. Either way the reference has to survive.
    const ECS::Entity wheel = scene.Create();
    (void)scene.Add<Name>(wheel, Name{Core::EntityName{"wheel"}});
    (void)scene.Add<Transform>(wheel, Transform{});
    (void)scene.Add<Parent>(wheel, Parent{.parent = body});
}

/// Rebuilds a scene from a decoded blob: one fresh entity per cooked entity, in
/// order, then every block decoded onto it with references resolved back through
/// the name table.
///
/// This is what a loader will do. Written here rather than shipped because
/// nothing serves cooked bytes yet, and a loader with no provider behind it is a
/// path no caller exercises.
void RebuildFrom(const CookedScene &cooked, ECS::Scene &scene)
{
    std::vector<ECS::Entity> byNameIndex(cooked.names.size(), ECS::NullEntity);
    std::vector<ECS::Entity> created;
    created.reserve(cooked.entities.size());

    // Every entity first, so a reference pointing forward resolves.
    for (const Runtime::CookedEntity &entity : cooked.entities)
    {
        const ECS::Entity e = scene.Create();
        created.push_back(e);
        byNameIndex[entity.nameIndex] = e;
    }

    Core::Reflect::CodecContext codec;
    codec.entityFromWire = [&byNameIndex](std::uint64_t wire) -> std::uint64_t
                           {
                               const auto index = static_cast<std::size_t>(wire);
                               if (index >= byNameIndex.size())
                               {
                                   return (static_cast<std::uint64_t>(ECS::NullEntity.generation) << 32) |
                                          ECS::NullEntity.index;
                               }
                               const ECS::Entity target = byNameIndex[index];
                               return (static_cast<std::uint64_t>(target.generation) << 32) | target.index;
                           };

    auto &registry = Core::Reflect::ComponentRegistry::Instance();
    for (std::size_t i = 0; i < cooked.entities.size(); ++i)
    {
        const ECS::Entity e = created[i];
        for (const Runtime::CookedComponent &component : cooked.entities[i].components)
        {
            const Core::Reflect::ComponentMeta *meta = registry.ById(component.id);
            REQUIRE(meta != nullptr);

            void *instance = meta->construct(&scene, e.index, e.generation);
            REQUIRE(instance != nullptr);

            Core::BitReader reader{component.block};
            (void)Core::Reflect::ReadComponentId(reader); // the block leads with it
            REQUIRE(Core::Reflect::ReadComponent(*meta, instance, reader, nullptr, &codec));
        }

        // The name is the entity's Name, which the blob carries as the name table
        // rather than as a component.
        (void)scene.Add<Name>(e, Name{Core::EntityName{cooked.names[cooked.entities[i].nameIndex].c_str()}});
    }
}

/// An asset root holding nothing but what a test wrote into it.
std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_cooked_" + name);
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    // The definition cache outlives a root change, so a file from a previous
    // case would otherwise answer for this one's.
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &root, const std::string &name, const nlohmann::json &document)
{
    std::ofstream out(root / name, std::ios::binary);
    out << document.dump(2);
    REQUIRE(out.good());
}

/// car.abp: a body carrying a Camera, and a wheel parented to it. Camera stands
/// in for "a component with more than one field", which is what an override that
/// stays a patch is actually about.
nlohmann::json CarFile()
{
    return {{"version", 2},
            {"entities", nlohmann::json::array(
                 {{{"name", "body"},
                   {"components", {{"Camera", {{"fovDegrees", 60.f}, {"isActive", true}}}}}},
                  {{"name", "wheel_fl"}, {"components", {{"Parent", {{"parent", "body"}}}}}}})}};
}

} // namespace

TEST_CASE("A cooked scene is a Scene blob this build's protocol matches")
{
    ECS::Scene scene;
    FillScene(scene);

    const auto bytes = SaveCookedScene(scene, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());

    Core::BitReader reader{*bytes};
    const auto kind = Core::ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    CHECK(*kind == Core::CookedKind::Scene);
    CHECK(reader.ReadBits64(64) == Core::Reflect::ProtocolHash());
}

TEST_CASE("Every entity, component and value survives the cooked round trip")
{
    ECS::Scene source;
    FillScene(source);

    const auto bytes = SaveCookedScene(source, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());

    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    CHECK(cooked->entities.size() == 3);

    ECS::Scene rebuilt;
    RebuildFrom(*cooked, rebuilt);

    // Compared through Save, which is the one description of a scene that already
    // covers every reflected field of every component — including any added after
    // this test was written.
    const nlohmann::json before = Runtime::SceneSerializer::Save(source);
    const nlohmann::json after  = Runtime::SceneSerializer::Save(rebuilt);
    CHECK(before["entities"] == after["entities"]);
}

TEST_CASE("A reference survives the cooked round trip as a reference")
{
    ECS::Scene source;
    FillScene(source);

    const auto bytes = SaveCookedScene(source, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());
    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());

    ECS::Scene rebuilt;
    RebuildFrom(*cooked, rebuilt);

    // Found by name rather than by handle: the rebuild allocates its own, and the
    // point is that the *wiring* came back, not that the numbers did.
    ECS::Entity wheel = ECS::NullEntity;
    ECS::Entity body  = ECS::NullEntity;
    for (auto [entity, name] : rebuilt.Query<Name>())
    {
        if (name.value.View() == "wheel")
        {
            wheel = entity;
        }
        if (name.value.View() == "body")
        {
            body = entity;
        }
    }
    REQUIRE(wheel != ECS::NullEntity);
    REQUIRE(body != ECS::NullEntity);

    const Parent *parent = rebuilt.Get<Parent>(wheel);
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == body);
}

TEST_CASE("The systems list survives")
{
    ECS::Scene scene;
    FillScene(scene);

    LevelHeader header;
    header.systems = {"Bounce", "Spin"};

    const auto bytes = SaveCookedScene(scene, header, nullptr);
    REQUIRE(bytes.has_value());

    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    CHECK(cooked->systems == header.systems);
}

TEST_CASE("Every name the file spells is in the table exactly once")
{
    ECS::Scene scene;
    FillScene(scene);

    const auto bytes = SaveCookedScene(scene, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());
    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());

    CHECK(cooked->names.size() == 3);
    std::vector<std::string> sorted = cooked->names;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == std::vector<std::string>{"body", "lamp", "wheel"});

    // Each entity's index is its own, so two entities sharing one would show up
    // here rather than as a mysterious duplicate after a load.
    std::vector<std::uint32_t> indices;
    for (const Runtime::CookedEntity &entity : cooked->entities)
    {
        indices.push_back(entity.nameIndex);
    }
    std::sort(indices.begin(), indices.end());
    CHECK(indices == std::vector<std::uint32_t>{0, 1, 2});
}

TEST_CASE("Cooking the same scene twice produces identical bytes")
{
    // Determinism is the issue's hard requirement, and the trap is any unordered
    // iteration: the component registry, the entity pools, and the name map are
    // all hash containers somewhere behind this call.
    ECS::Scene scene;
    FillScene(scene);

    const auto first = SaveCookedScene(scene, LevelHeader{}, nullptr);
    const auto second = SaveCookedScene(scene, LevelHeader{}, nullptr);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}

TEST_CASE("A blob of another kind is refused")
{
    Core::BitWriter writer;
    Core::WriteCookedHeader(writer, Core::CookedKind::Mesh);
    writer.WriteUInt64(Core::Reflect::ProtocolHash());

    const std::span<const std::byte> bytes = writer.Data();
    const auto cooked = DecodeCookedScene(bytes);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error() == LevelError::MalformedBlob);
}

TEST_CASE("A blob written against another component table is refused whole")
{
    // Not field by field: the codec names a component by a dense id the registry
    // assigns by sorting names, so one component added anywhere renumbers the
    // rest and every block after it would decode into its neighbour.
    ECS::Scene scene;
    FillScene(scene);
    const auto bytes = SaveCookedScene(scene, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());

    std::vector<std::byte> tampered = *bytes;
    Core::BitWriter header;
    Core::WriteCookedHeader(header, Core::CookedKind::Scene);
    const std::size_t hashOffset = header.Data().size();
    // Flip one bit of the stored protocol hash.
    tampered[hashOffset] ^= std::byte{0x01};

    const auto cooked = DecodeCookedScene(tampered);
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error() == LevelError::ProtocolMismatch);
}

TEST_CASE("A truncated blob is refused rather than half-read")
{
    ECS::Scene scene;
    FillScene(scene);
    const auto bytes = SaveCookedScene(scene, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());

    for (const std::size_t fraction : {2u, 4u, 8u})
    {
        const std::vector<std::byte> cut{bytes->begin(),
                                         bytes->begin() + static_cast<std::ptrdiff_t>(bytes->size() / fraction)};
        const auto cooked = DecodeCookedScene(cut);
        CHECK_FALSE(cooked.has_value());
    }
}

TEST_CASE("An instance survives with its placement, its overrides and its removals")
{
    const std::filesystem::path root = FreshRoot("instance");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances",
            nlohmann::json::array(
                {{{"name", "car_3"},
                  {"source", "car.abp"},
                  {"transform", {{"position", {1.f, 2.f, 3.f}}}},
                  {"overrides", {{"body", {{"Camera", {{"fovDegrees", 90.f}}}}}}},
                  {"removed", nlohmann::json::array({"wheel_fl"})}}})}});

    ECS::Scene scene;
    Runtime::InstanceTable table;
    Runtime::LevelHeader header;
    REQUIRE(Runtime::SceneSerializer::LoadFromFile(scene, "main.alvl",
                                                   {.header = &header, .instances = &table}));

    const auto bytes = SaveCookedScene(scene, header, &table);
    REQUIRE(bytes.has_value());

    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    REQUIRE(cooked->instances.size() == 1);

    const Runtime::CookedInstance &instance = cooked->instances.front();
    CHECK(instance.name == "car_3");
    CHECK(instance.source == "car.abp");
    CHECK(instance.transform.position.x == doctest::Approx(1.f));
    CHECK(instance.transform.position.z == doctest::Approx(3.f));
    REQUIRE(instance.removed.size() == 1);
    CHECK(instance.removed.front() == "wheel_fl");
    REQUIRE(instance.overrides.size() == 1);
}

TEST_CASE("An override is a masked block naming only the field the author set")
{
    // The property the whole format rests on. A block carrying full state would
    // freeze isActive at whatever the blueprint held on the day it was cooked,
    // and a later edit to car.abp would stop reaching this instance — which is
    // "fix it once, fixed everywhere" quietly ceasing to be true.
    const std::filesystem::path root = FreshRoot("mask");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"overrides",
                                                  {{"body", {{"Camera", {{"fovDegrees", 90.f}}}}}}}}})}});

    ECS::Scene scene;
    Runtime::InstanceTable table;
    Runtime::LevelHeader header;
    REQUIRE(Runtime::SceneSerializer::LoadFromFile(scene, "main.alvl",
                                                   {.header = &header, .instances = &table}));

    const auto bytes = SaveCookedScene(scene, header, &table);
    REQUIRE(bytes.has_value());
    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    REQUIRE(cooked->instances.size() == 1);
    REQUIRE(cooked->instances.front().overrides.size() == 1);

    const Runtime::CookedOverride &claim = cooked->instances.front().overrides.front();
    CHECK_FALSE(claim.absent);
    CHECK(cooked->names.at(claim.memberNameIndex) == "body");
    CHECK(cooked->componentNames.at(claim.componentNameIndex) == "Camera");

    // Decode the block and read the mask back: exactly one bit, and it is the
    // one standing for the field the level named.
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find("Camera");
    REQUIRE(meta != nullptr);

    Core::BitReader reader{claim.block};
    (void)Core::Reflect::ReadComponentId(reader);

    Runtime::Camera camera;
    Core::Reflect::FieldMask applied = 0;
    REQUIRE(Core::Reflect::ReadComponent(*meta, &camera, reader, &applied, nullptr));
    CHECK(std::popcount(applied) == 1);
    CHECK(camera.fovDegrees == doctest::Approx(90.f));

    // And the mask bit is fovDegrees', not whichever field happens to be first.
    std::size_t codecIndex = 0;
    std::size_t fovIndex   = 0;
    for (const Core::Reflect::FieldMeta &field : meta->fields)
    {
        if (!Core::Reflect::IsWireField(field))
        {
            continue;
        }
        if (field.name == "fovDegrees")
        {
            fovIndex = codecIndex;
        }
        ++codecIndex;
    }
    CHECK(applied == Core::Reflect::FieldMaskBit(fovIndex));
}

TEST_CASE("An override naming a field the component does not have is refused")
{
    // An override that silently did nothing reads to its author as a fix that
    // did not take, which is worse than a cook that stops and says so.
    const std::filesystem::path root = FreshRoot("badfield");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"overrides",
                                                  {{"body", {{"Camera", {{"noSuchField", 1.f}}}}}}}}})}});

    ECS::Scene scene;
    Runtime::InstanceTable table;
    Runtime::LevelHeader header;
    REQUIRE(Runtime::SceneSerializer::LoadFromFile(scene, "main.alvl",
                                                   {.header = &header, .instances = &table}));

    const auto bytes = SaveCookedScene(scene, header, &table);
    REQUIRE_FALSE(bytes.has_value());
    CHECK(bytes.error() == LevelError::MalformedComponent);
}

TEST_CASE("A member entity is described by its instance, not written as an entity")
{
    // The same rule Save follows. Writing members as entities too would bake a
    // copy of the blueprint into the level and undo the whole point of one.
    const std::filesystem::path root = FreshRoot("members");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({{{"name", "ground"}}})},
           {"instances",
            nlohmann::json::array({{{"name", "car_3"}, {"source", "car.abp"}}})}});

    ECS::Scene scene;
    Runtime::InstanceTable table;
    Runtime::LevelHeader header;
    REQUIRE(Runtime::SceneSerializer::LoadFromFile(scene, "main.alvl",
                                                   {.header = &header, .instances = &table}));

    const auto bytes = SaveCookedScene(scene, header, &table);
    REQUIRE(bytes.has_value());
    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());

    // One entity: the level's own. The car's body and wheel are the instance's.
    REQUIRE(cooked->entities.size() == 1);
    CHECK(cooked->names.at(cooked->entities.front().nameIndex) == "ground");
    CHECK(cooked->instances.size() == 1);
}

TEST_CASE("An empty scene cooks and comes back empty")
{
    ECS::Scene empty;

    const auto bytes = SaveCookedScene(empty, LevelHeader{}, nullptr);
    REQUIRE(bytes.has_value());

    const auto cooked = DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    CHECK(cooked->entities.empty());
    CHECK(cooked->names.empty());
    CHECK(cooked->instances.empty());
}
