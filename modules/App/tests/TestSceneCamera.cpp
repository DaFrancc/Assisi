/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for App::ActiveSceneCamera — the one answer to "which camera does this
/// scene nominate", shared by the game's only view and the editor's play view.

#include <doctest/doctest.h>

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Components.hpp>

#include "LogCapture.hpp"

using namespace Assisi::App;

namespace
{
// A camera standing well away from the origin, so a pose that lost its world
// matrix is not mistaken for one that kept it.
constexpr glm::vec3 kCameraPosition{4.f, 3.f, 12.f};

Assisi::ECS::Entity AddCamera(Assisi::ECS::Scene &scene, bool active, const glm::vec3 &position)
{
    const Assisi::ECS::Entity entity = scene.Create();

    Assisi::ECS::Transform *transform = scene.Add<Assisi::ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position    = position;
    transform->worldMatrix = glm::translate(glm::mat4(1.f), position);

    Assisi::Runtime::Camera *camera = scene.Add<Assisi::Runtime::Camera>(entity);
    REQUIRE(camera != nullptr);
    camera->isActive = active;

    return entity;
}
} // namespace

TEST_CASE("ActiveSceneCamera returns the active camera with its world matrix intact")
{
    // The world matrix is the whole of what the view is derived from, so a copy
    // that drops it renders from the origin looking down -Z — a scene that looks
    // empty rather than one that looks wrong.
    Assisi::ECS::Scene scene;
    AddCamera(scene, /*active=*/ false, glm::vec3{-9.f, -9.f, -9.f});
    AddCamera(scene, /*active=*/ true, kCameraPosition);

    const std::optional<SceneView> view = ActiveSceneCamera(scene);

    REQUIRE(view.has_value());
    CHECK(view->pose.position == kCameraPosition);
    CHECK(view->pose.worldMatrix == glm::translate(glm::mat4(1.f), kCameraPosition));
}

TEST_CASE("ActiveSceneCamera is empty when the scene nominates none")
{
    // Not an error: a level that has not composed a camera yet is something both
    // hosts have a fallback for, and each one's differs.
    Assisi::ECS::Scene scene;
    AddCamera(scene, /*active=*/ false, kCameraPosition);

    CHECK_FALSE(ActiveSceneCamera(scene).has_value());
}

TEST_CASE("ActiveSceneCamera says so when two cameras are active")
{
    // The first found wins, and which that is depends on entity order — so the
    // line is the only thing standing between an author and a viewport looking
    // through a camera they did not mean.
    const Assisi::Tests::LogCapture log;

    Assisi::ECS::Scene scene;
    AddCamera(scene, /*active=*/ true, kCameraPosition);
    AddCamera(scene, /*active=*/ true, glm::vec3{-1.f, -2.f, -3.f});

    const std::optional<SceneView> view = ActiveSceneCamera(scene);

    REQUIRE(view.has_value());
    CHECK(view->pose.position == kCameraPosition); // the first, not the last
    CHECK(log.Mentions("more than one Camera is marked active"));
}
