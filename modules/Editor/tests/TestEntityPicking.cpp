/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEntityPicking.cpp
/// @brief A viewport click hits the mesh, not a stand-in cube around it (ENG-125).
///
/// Testing every meshed entity against a unit cube — ±0.5 in model space — picks
/// anything not roughly unit-sized wrong in both directions: a large mesh cannot
/// be clicked over most of its body, and a small one owns a click target far
/// bigger than what is drawn. The mesh's own bounds sit one member away on its
/// MeshBuffer, which is what FocusCameraOn already frames with.
///
/// So the assertions are about *which entity comes back*: a mesh whose local
/// bounds are clearly not unit-sized, hit from a direction that misses the cube,
/// and a small mesh that must stay silent for a click landing inside the cube but
/// outside the mesh. Each of those rays is chosen so the cube and the real bounds
/// disagree — a ray straight down the middle would pass either way and prove
/// nothing.
///
/// The bounds lookup is injected because a MeshBuffer only carries bounds after a
/// GPU upload, so a headless test cannot give an entity real ones. MeshPickBounds
/// — the lookup the editor itself passes — is covered separately for the parts
/// that need no device.

#include <doctest/doctest.h>

#include <optional>
#include <unordered_map>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/ScenePick.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

using namespace Assisi;
using Assisi::Editor::MeshPickBounds;
using Assisi::Editor::PickableBounds;
using Assisi::Editor::PickEntityInScene;
using Assisi::Editor::PickRay;

namespace
{

/// The icon half-size the editor picks placement-only entities with — half of
/// Render::kEntityIconWorldSize, as EditorApp::PickEntity computes it.
constexpr float kIconHalf = 0.25f;

constexpr float kEpsilon = 1e-4f;

/// Local bounds per entity, standing in for the MeshBuffer a headless test has no
/// device to build. Keyed by entity because the lookup PickEntityInScene takes is
/// a plain function pointer and so cannot capture.
std::unordered_map<ECS::Entity, Geometry::Aabb> g_bounds;

std::optional<Geometry::Aabb> LookupBounds(const ECS::Scene &, ECS::Entity entity)
{
    const auto it = g_bounds.find(entity);
    if (it == g_bounds.end())
    {
        return std::nullopt; // no mesh — picked by its icon quad
    }
    return it->second;
}

/// A camera at +Z looking down -Z, offset by @p offset in the view plane. The
/// basis matches BuildPickRay's, so the icon quad a click is tested against is
/// oriented the way the renderer draws it.
PickRay RayAt(glm::vec3 offset)
{
    return PickRay{.origin      = glm::vec3(0.f, 0.f, 20.f) + offset,
                   .direction   = glm::vec3(0.f, 0.f, -1.f),
                   .cameraRight = glm::vec3(1.f, 0.f, 0.f),
                   .cameraUp    = glm::vec3(0.f, 1.f, 0.f),
                   .valid       = true};
}

struct Fixture
{
    Fixture() { g_bounds.clear(); }
    ~Fixture() { g_bounds.clear(); }

    /// An entity at @p position with local bounds of half-size @p halfExtent and a
    /// uniform @p scale — the two ways a pick volume can end up unlike a unit cube.
    ECS::Entity SpawnMeshed(glm::vec3 position, float halfExtent, float scale = 1.f)
    {
        const ECS::Entity entity = scene.Create();
        REQUIRE(scene.Add(entity, ECS::Transform{.position = position, .scale = glm::vec3(scale)}) != nullptr);
        g_bounds[entity] = Geometry::Aabb{.min = glm::vec3(-halfExtent), .max = glm::vec3(halfExtent)};
        Propagate();
        return entity;
    }

    /// A placement-only entity: no bounds, so it is picked by its icon.
    ECS::Entity SpawnIconOnly(glm::vec3 position)
    {
        const ECS::Entity entity = scene.Create();
        REQUIRE(scene.Add(entity, ECS::Transform{.position = position}) != nullptr);
        Propagate();
        return entity;
    }

    void Propagate() { (void)Runtime::PropagateTransforms(scene, 0); }

    ECS::Entity Pick(const PickRay &ray, float &tOut)
    {
        return PickEntityInScene(scene, ray, kIconHalf, &LookupBounds, tOut);
    }

    ECS::Entity Pick(const PickRay &ray)
    {
        float ignored = 0.f;
        return Pick(ray, ignored);
    }

    ECS::Scene scene;
};

} // namespace

TEST_CASE("PickEntityInScene: a large mesh is clickable over its whole body")
{
    Fixture fixture;
    // Half-extent 4, so the ray at y = 3 is well inside the mesh and well outside
    // a ±0.5 cube.
    const ECS::Entity big = fixture.SpawnMeshed(glm::vec3(0.f), 4.f);

    float t = 0.f;
    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 3.f, 0.f)), t) == big);
    // The near face, not the far one: 20 back from the origin, box out to z = 4.
    CHECK(t == doctest::Approx(16.f).epsilon(kEpsilon));
}

TEST_CASE("PickEntityInScene: a small mesh does not own the space around it")
{
    Fixture fixture;
    // Half-extent 0.05 — a click 0.3 above it lands inside a unit cube and must
    // still miss, or the entity is answering for space it does not occupy.
    fixture.SpawnMeshed(glm::vec3(0.f), 0.05f);

    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 0.3f, 0.f))) == ECS::NullEntity);
    // ...while a click on the mesh itself still lands.
    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 0.02f, 0.f))) != ECS::NullEntity);
}

TEST_CASE("PickEntityInScene: local bounds and the world matrix compose")
{
    Fixture fixture;
    // Half-extent 2 under a 2x scale reaches 4 in world space; the unit cube under
    // the same scale reaches only 1, so a ray at y = 3 separates the two.
    const ECS::Entity scaled = fixture.SpawnMeshed(glm::vec3(0.f), 2.f, 2.f);

    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 3.f, 0.f))) == scaled);
    // Beyond the scaled bounds, nothing is hit — the volume is not unbounded.
    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 5.f, 0.f))) == ECS::NullEntity);
}

TEST_CASE("PickEntityInScene: the nearest hit wins")
{
    Fixture fixture;
    const ECS::Entity behind  = fixture.SpawnMeshed(glm::vec3(0.f, 3.f, -6.f), 4.f);
    const ECS::Entity inFront = fixture.SpawnMeshed(glm::vec3(0.f, 3.f, 6.f), 4.f);

    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 3.f, 0.f))) == inFront);
    CHECK(behind != inFront);
}

TEST_CASE("PickEntityInScene: a placement-only entity is picked by its icon alone")
{
    Fixture fixture;
    const ECS::Entity icon = fixture.SpawnIconOnly(glm::vec3(0.f));

    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 0.1f, 0.f))) == icon);
    // Outside the drawn quad it is not clickable — it does not swallow the clicks
    // a whole cube around it would.
    CHECK(fixture.Pick(RayAt(glm::vec3(0.f, 1.f, 0.f))) == ECS::NullEntity);
}

TEST_CASE("PickEntityInScene: an invalid ray picks nothing")
{
    Fixture fixture;
    fixture.SpawnMeshed(glm::vec3(0.f), 4.f);

    PickRay ray = RayAt(glm::vec3(0.f));
    ray.valid   = false;
    CHECK(fixture.Pick(ray) == ECS::NullEntity);
}

TEST_CASE("MeshPickBounds: no mesh means no box")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{}) != nullptr);

    CHECK_FALSE(MeshPickBounds(scene, entity).has_value());
}

TEST_CASE("MeshPickBounds: a mesh whose buffer is not resident yet keeps the unit cube")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, ECS::Transform{}) != nullptr);
    REQUIRE(scene.Add(entity, Runtime::MeshRenderer{}) != nullptr);

    const std::optional<Geometry::Aabb> bounds = MeshPickBounds(scene, entity);
    REQUIRE(bounds.has_value());
    CHECK(bounds->min.x == doctest::Approx(-0.5f).epsilon(kEpsilon));
    CHECK(bounds->max.x == doctest::Approx(0.5f).epsilon(kEpsilon));
}

TEST_CASE("PickableBounds: a mesh's own box is used as it is")
{
    const Geometry::Aabb mesh{.min = glm::vec3(-3.f, -1.f, -2.f), .max = glm::vec3(3.f, 1.f, 2.f)};
    const Geometry::Aabb picked = PickableBounds(mesh);

    CHECK(picked.min.x == doctest::Approx(mesh.min.x).epsilon(kEpsilon));
    CHECK(picked.max.z == doctest::Approx(mesh.max.z).epsilon(kEpsilon));
}

TEST_CASE("PickableBounds: a flat mesh keeps a thickness to be clicked on")
{
    // A ground quad has no extent on Y. Passed through unpadded it is a razor slab
    // only an exactly edge-on ray can hit.
    const Geometry::Aabb quad{.min = glm::vec3(-5.f, 0.f, -5.f), .max = glm::vec3(5.f, 0.f, 5.f)};
    const Geometry::Aabb picked = PickableBounds(quad);

    CHECK(picked.max.y > picked.min.y);
    // The axes that had extent are untouched.
    CHECK(picked.min.x == doctest::Approx(-5.f).epsilon(kEpsilon));
    CHECK(picked.max.x == doctest::Approx(5.f).epsilon(kEpsilon));
}

TEST_CASE("PickableBounds: an empty box falls back to the unit cube")
{
    const Geometry::Aabb empty{};
    const Geometry::Aabb picked = PickableBounds(empty);

    CHECK(picked.min.y == doctest::Approx(-0.5f).epsilon(kEpsilon));
    CHECK(picked.max.y == doctest::Approx(0.5f).epsilon(kEpsilon));
}
