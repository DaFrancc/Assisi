/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for App::ActiveSceneCamera — the one answer to "which camera does this
/// scene nominate", shared by the game's only view and the editor's play view.

#include <doctest/doctest.h>

#include <cmath>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
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
    transform->position = position;

    Assisi::Runtime::Camera *camera = scene.Add<Assisi::Runtime::Camera>(entity);
    REQUIRE(camera != nullptr);
    camera->isActive = active;

    return entity;
}

/// Whether @p a and @p b agree to within float noise, element by element.
bool Near(const glm::mat4 &a, const glm::mat4 &b)
{
    constexpr float kEpsilon = 1e-5f;
    for (glm::length_t column = 0; column < 4; ++column)
    {
        for (glm::length_t row = 0; row < 4; ++row)
        {
            if (std::abs(a[column][row] - b[column][row]) > kEpsilon)
            {
                return false;
            }
        }
    }
    return true;
}

/// A quarter turn left, far from anything a Transform here starts with.
const glm::quat kDrawnRotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
constexpr glm::vec3 kDrawnOffset{0.f, 0.1f, 0.05f};
} // namespace

TEST_CASE("ActiveSceneCamera returns the active camera with its world matrix intact")
{
    // The world matrix is the whole of what the view is derived from, so a copy
    // that drops it renders from the origin looking down -Z — a scene that looks
    // empty rather than one that looks wrong.
    Assisi::ECS::Scene scene;
    AddCamera(scene, /*active=*/ false, glm::vec3{-9.f, -9.f, -9.f});
    AddCamera(scene, /*active=*/ true, kCameraPosition);
    (void)Assisi::ECS::PropagateTransforms(scene, 0);

    const std::optional<SceneView> view = ActiveSceneCamera(scene);

    REQUIRE(view.has_value());
    CHECK(glm::vec3(view->pose.position) == kCameraPosition);
    CHECK(view->world.inCell == glm::translate(glm::mat4(1.f), kCameraPosition));
    CHECK(view->world.cell == glm::ivec3(0));
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
    CHECK(glm::vec3(view->pose.position) == kCameraPosition); // the first, not the last
    CHECK(log.Mentions("more than one Camera is marked active"));
}

TEST_CASE("A camera with a draw pose is drawn turned and moved by it, and its Transform is left alone")
{
    // What a game moves the view with every frame, such as head bob or its own
    // look, without moving anything simulated or sent.
    Assisi::ECS::Scene scene;
    const Assisi::ECS::Entity camera = AddCamera(scene, /*active=*/ true, kCameraPosition);
    (void)scene.Add<Assisi::Runtime::CameraDrawPose>(
        camera, Assisi::Runtime::CameraDrawPose{.rotation = kDrawnRotation, .offset = kDrawnOffset});
    (void)Assisi::ECS::PropagateTransforms(scene, 0);

    const std::optional<SceneView> view = ActiveSceneCamera(scene);

    REQUIRE(view.has_value());
    CHECK(Near(view->world.inCell,
               glm::translate(glm::mat4(1.f), kCameraPosition + kDrawnOffset) * glm::mat4_cast(kDrawnRotation)));
    // The pose it stands at, and the scene's own copy, are as the Transform says.
    CHECK(glm::vec3(view->pose.position) == kCameraPosition);
    const Assisi::ECS::Transform *transform = scene.Get<Assisi::ECS::Transform>(camera);
    CHECK(glm::vec3(transform->position) == kCameraPosition);
    CHECK(transform->rotation == glm::quat(1.f, 0.f, 0.f, 0.f));
}

TEST_CASE("A draw pose on the player's camera takes the place of the turning drawn ahead of the step")
{
    // A game that draws its own look ahead must not be turned a second time by
    // the engine's.
    Assisi::ECS::Scene scene;
    const Assisi::ECS::Entity player = scene.Create();
    (void)scene.Add<Assisi::ECS::Transform>(player);
    (void)scene.Add<LocalPlayer>(player);
    PlayerInput *controls = scene.Add<PlayerInput>(player);
    REQUIRE(controls != nullptr);
    controls->lookYaw = 0.7f;
    const Assisi::ECS::Entity camera = AddCamera(scene, /*active=*/ true, glm::vec3{0.f});
    (void)scene.Add<Assisi::ECS::Parent>(camera, Assisi::ECS::Parent{player});
    (void)Assisi::ECS::PropagateTransforms(scene, 0);

    // Without one, the turning not yet stepped shows: the case below means something.
    const std::optional<SceneView> turned = ActiveSceneCamera(scene);
    REQUIRE(turned.has_value());
    CHECK_FALSE(Near(turned->world.inCell, glm::mat4(1.f)));

    (void)scene.Add<Assisi::Runtime::CameraDrawPose>(
        camera, Assisi::Runtime::CameraDrawPose{.rotation = kDrawnRotation, .offset = glm::vec3{0.f}});
    const std::optional<SceneView> drawn = ActiveSceneCamera(scene);
    REQUIRE(drawn.has_value());
    CHECK(Near(drawn->world.inCell, glm::mat4_cast(kDrawnRotation)));
}
