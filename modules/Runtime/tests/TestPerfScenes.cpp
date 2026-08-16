/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Gate on the committed performance measurement levels in assets/levels/.
///
/// Two things are being defended, and neither is "the serializer works" — that
/// is TestSceneSerializer's job.
///
/// The first is that the scenes load at all, from the tree as committed. Their
/// definition of done is "load from a clean clone", and the failure mode is
/// quiet: a primitive renamed, a component field dropped, a guid changed, and
/// the scene either refuses or silently swaps geometry for the fallback cube.
/// A gate that runs on every merge is the only version of that check anyone
/// actually performs.
///
/// The second is the published contract. The reference scenes state exact
/// triangle, instance and light counts, and every later rendering stage's cost
/// is quoted against them. If those numbers drift without anyone noticing, the
/// whole measurement ledger is quoting a scene that no longer exists. So the
/// numbers are asserted here and must be moved deliberately — in this file, in
/// scripts/make-perf-scenes.py, and in the issue that publishes them.

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::SceneSerializer;

// These read the committed levels straight out of the source tree, which a
// Release build deliberately does not have (the staged copy beside the binary is
// its durable one). So the gate compiles out there rather than failing the
// build, and says so rather than vanishing silently — a suite that quietly drops
// cases in one configuration is how a gate stops being one.
#ifdef ASSISI_SOURCE_ASSET_ROOT

namespace
{

std::filesystem::path LevelPath(const std::string &name)
{
    return std::filesystem::path{ASSISI_SOURCE_ASSET_ROOT} / "levels" / (name + ".alvl");
}

/// What a scene is worth asserting about: the content stats it publishes.
struct SceneStats
{
    int32_t entities   = 0;
    int32_t meshes     = 0;
    int32_t triangles  = 0;
    int32_t pointLights = 0;
    int32_t spotLights  = 0;
    int32_t directionalLights = 0;
    int32_t movers     = 0;
    int32_t cameras    = 0;
};

/// Triangle count of each `prim://` rung at its pinned tessellation. Written out
/// rather than derived from the factories on purpose: this file is the
/// independent statement of the contract, and deriving it from the code under
/// test would make the assertion agree with whatever that code happened to do.
/// TestDefaultMeshes.cpp asserts the same numbers against the real meshes, so
/// the two have to be changed together or one of them fails.
int32_t TrianglesFor(const Core::AssetId &mesh)
{
    if (mesh == Core::BuiltinAssetId::Cube) return 12;
    if (mesh == Core::BuiltinAssetId::SphereLow) return 144;
    if (mesh == Core::BuiltinAssetId::Sphere) return 576;
    if (mesh == Core::BuiltinAssetId::SphereHigh) return 4096;
    if (mesh == Core::BuiltinAssetId::IcosphereLow) return 320;
    if (mesh == Core::BuiltinAssetId::Icosphere) return 1280;
    if (mesh == Core::BuiltinAssetId::IcosphereHigh) return 20480;
    if (mesh == Core::BuiltinAssetId::Cylinder) return 96;
    if (mesh == Core::BuiltinAssetId::CylinderHigh) return 256;
    return -1; // an unreserved mesh: the scenes are primitives-only by design
}

SceneStats LoadStats(const std::string &name)
{
    ECS::Scene scene;
    const Runtime::LevelResult result = SceneSerializer::LoadFromDisk(scene, LevelPath(name));
    REQUIRE_MESSAGE(result.has_value(), "level failed to load: " << name);

    SceneStats stats;
    for (auto [entity, renderer] : scene.Query<Runtime::MeshRenderer>())
    {
        (void)entity;
        ++stats.meshes;
        const int32_t triangles = TrianglesFor(renderer.mesh);
        // A mesh reference the ladder does not cover means the scene picked up
        // geometry from somewhere uncommitted — exactly what "buildable from
        // committed assets only" rules out.
        REQUIRE_MESSAGE(triangles > 0, "scene references a non-primitive mesh: " << name);
        stats.triangles += triangles;
    }
    for (auto [entity, light] : scene.Query<Runtime::PointLight>()) { (void)entity; (void)light; ++stats.pointLights; }
    for (auto [entity, light] : scene.Query<Runtime::SpotLight>()) { (void)entity; (void)light; ++stats.spotLights; }
    for (auto [entity, light] : scene.Query<Runtime::DirectionalLight>())
    {
        (void)entity;
        (void)light;
        ++stats.directionalLights;
    }
    for (auto [entity, mover] : scene.Query<Runtime::Oscillator>()) { (void)entity; (void)mover; ++stats.movers; }
    for (auto [entity, camera] : scene.Query<Runtime::Camera>()) { (void)entity; (void)camera; ++stats.cameras; }

    stats.entities = static_cast<int32_t>(scene.AliveCount());
    return stats;
}

} // namespace

// Every scene's camera must start level and looking at the content.
//
// A capture adopts the level's active Camera, so this rotation *is* the view the
// published numbers are of. It is worth pinning because the failure hides
// itself: an inverted `right` vector rolls the camera 180 degrees, and the
// editor's fly controller re-derives the rotation from yaw and pitch the moment
// anyone touches the mouse — so the scene looks correct to anyone who
// interacts with it, and wrong only to a capture, which never does.
TEST_CASE("Perf scene cameras start level and face the scene")
{
    for (const std::string &name : {std::string{"PerfBlank"}, std::string{"PerfReferenceManyInstances"},
                                    std::string{"PerfReferenceFewInstances"}, std::string{"PerfStress"},
                                    std::string{"PerfGeometryStress"}})
    {
        CAPTURE(name);
        ECS::Scene scene;
        REQUIRE(SceneSerializer::LoadFromDisk(scene, LevelPath(name)).has_value());

        int32_t activeCameras = 0;
        for (auto [entity, camera] : scene.Query<Runtime::Camera>())
        {
            if (!camera.isActive)
            {
                continue;
            }
            ++activeCameras;

            const ECS::Transform *transform = scene.Get<ECS::Transform>(entity);
            REQUIRE(transform != nullptr);

            // Not upside down: the camera's own up must share a hemisphere with
            // world up.
            const glm::vec3 up = transform->rotation * glm::vec3{0.f, 1.f, 0.f};
            CHECK(up.y > 0.5f);

            // No roll: its right must be level, or the horizon tilts.
            const glm::vec3 right = transform->rotation * glm::vec3{1.f, 0.f, 0.f};
            CHECK(std::abs(right.y) < 0.01f);

            // ...and it must actually look at the origin, which is where every
            // scene is built around. -Z is forward in view space.
            const glm::vec3 forward = transform->rotation * glm::vec3{0.f, 0.f, -1.f};
            const glm::vec3 toOrigin = glm::normalize(-transform->position);
            CHECK(glm::dot(forward, toOrigin) > 0.99f);
        }
        CHECK(activeCameras == 1);
    }
}

// The pay-for-what-you-place gate runs here forever: an empty world, so a stage
// that costs anything in this scene costs it when its feature is unused.
TEST_CASE("PerfBlank is empty but for the camera that defines the view")
{
    const SceneStats stats = LoadStats("PerfBlank");
    CHECK(stats.meshes == 0);
    CHECK(stats.triangles == 0);
    CHECK(stats.pointLights == 0);
    CHECK(stats.spotLights == 0);
    CHECK(stats.directionalLights == 0);
    CHECK(stats.cameras == 1);
    CHECK(stats.entities == 1);
}

// The published contract for "reasonable map design", spread thin.
TEST_CASE("PerfReferenceManyInstances matches its published contract")
{
    const SceneStats stats = LoadStats("PerfReferenceManyInstances");
    CHECK(stats.meshes == 300);
    CHECK(stats.triangles == 49500);
    CHECK(stats.pointLights == 8);
    CHECK(stats.spotLights == 4);
    CHECK(stats.directionalLights == 1);
    CHECK(stats.movers == 4);
    CHECK(stats.cameras == 1);
}

// The other half of the controlled pair: the same triangle budget carried by a
// fraction of the instances. Any measured difference between the two scenes is
// per-object cost, which is what the cascade and atlas stages are judged on —
// so the triangle counts staying close is the property that matters, more than
// either number on its own.
TEST_CASE("PerfReferenceFewInstances matches its published contract")
{
    const SceneStats stats = LoadStats("PerfReferenceFewInstances");
    CHECK(stats.meshes == 40);
    CHECK(stats.triangles == 49932);
    CHECK(stats.pointLights == 8);
    CHECK(stats.spotLights == 4);
    CHECK(stats.directionalLights == 1);
    CHECK(stats.movers == 4);
    CHECK(stats.cameras == 1);
}

TEST_CASE("The reference pair isolates instance count from triangle count")
{
    const SceneStats many = LoadStats("PerfReferenceManyInstances");
    const SceneStats few  = LoadStats("PerfReferenceFewInstances");

    // Within 2% on triangles — close enough that the geometry load is not what
    // separates them.
    const double ratio = static_cast<double>(many.triangles) / static_cast<double>(few.triangles);
    CHECK(ratio > 0.98);
    CHECK(ratio < 1.02);

    // ...and far apart on instances, or there is no experiment.
    CHECK(many.meshes > few.meshes * 5);

    // Same lighting on both sides, so lights cannot explain a difference either.
    CHECK(many.pointLights == few.pointLights);
    CHECK(many.spotLights == few.spotLights);
    CHECK(many.directionalLights == few.directionalLights);
}

// 4x the reference scene's geometry and local lights — the no-cliff check.
TEST_CASE("PerfStress is four times the reference scene")
{
    const SceneStats stress    = LoadStats("PerfStress");
    const SceneStats reference = LoadStats("PerfReferenceManyInstances");

    CHECK(stress.meshes == 1185);
    CHECK(stress.triangles == 191052);
    CHECK(stress.pointLights == reference.pointLights * 4);
    CHECK(stress.spotLights == reference.spotLights * 4);
    CHECK(stress.directionalLights == 1);
}

// Millions of triangles from few instances: the geometry throughput limit, kept
// deliberately separate from PerfStress's draw-count scaling so a regression in
// one is not masked by the other.
TEST_CASE("PerfGeometryStress carries millions of triangles")
{
    const SceneStats stats = LoadStats("PerfGeometryStress");
    CHECK(stats.meshes == 151);
    CHECK(stats.triangles == 3072012);
    CHECK(stats.triangles > 3'000'000);
    CHECK(stats.cameras == 1);
}

// Adopted as the light-count degradation case rather than authored for it, so
// what is asserted is only that it still loads and still has the light counts
// the plan quotes it for.
TEST_CASE("Lights.alvl still carries the light counts it is quoted for")
{
    const SceneStats stats = LoadStats("Lights");
    CHECK(stats.pointLights == 270);
    CHECK(stats.spotLights == 29);
    CHECK(stats.directionalLights == 1);
}

#else // !ASSISI_SOURCE_ASSET_ROOT

TEST_CASE("The perf scene gate needs the source asset tree" * doctest::skip())
{
    MESSAGE("Built without ASSISI_SOURCE_ASSET_ROOT (a Release configuration), so the committed "
            "levels are not reachable and the contract gate did not run. Run it in debug or dev.");
}

#endif // ASSISI_SOURCE_ASSET_ROOT
