/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Renderer.hpp>
#include <Assisi/Runtime/TextureCamera.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

using namespace Assisi;
using Runtime::CameraSurfaceBounds;
using Runtime::TextureCameraCandidate;
using Runtime::TextureCameraUpdate;

namespace
{
/// How far in front of or behind a camera a test surface stands.
constexpr float kSurfaceDistance = 10.f;
/// A test surface's radius: well inside a frustum it stands in.
constexpr float kSurfaceRadius = 1.f;
/// Where the second test camera stands, off to the side, out of the main
/// camera's view.
constexpr glm::vec3 kAside{100.f, 0.f, 0.f};

/// What a camera at @p eye looking along @p forward sees.
Render::Frustum FrustumOf(const glm::vec3 &eye, const glm::vec3 &forward)
{
    const glm::mat4 world = glm::inverse(glm::lookAt(eye, eye + forward, glm::vec3(0.f, 1.f, 0.f)));
    return Render::Frustum::FromViewProjection(Runtime::ProjectionMatrix(Runtime::Camera{}, 1.f) *
                                               Runtime::ViewMatrix(world));
}

/// The main camera: at the origin, looking down -Z.
Render::Frustum MainFrustum()
{
    return FrustumOf(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f));
}

/// A surface in front of the main camera showing @p camera.
CameraSurfaceBounds InFrontOfMain(ECS::Entity camera)
{
    return CameraSurfaceBounds{.bounds = {.center = glm::vec3(0.f, 0.f, -kSurfaceDistance), .radius = kSurfaceRadius},
                               .camera = camera};
}

/// A surface behind the main camera showing @p camera.
CameraSurfaceBounds BehindMain(ECS::Entity camera)
{
    return CameraSurfaceBounds{.bounds = {.center = glm::vec3(0.f, 0.f, kSurfaceDistance), .radius = kSurfaceRadius},
                               .camera = camera};
}

/// A surface in front of the camera at kAside looking +X, out of the main
/// camera's view, showing @p camera.
CameraSurfaceBounds InFrontOfAside(ECS::Entity camera)
{
    return CameraSurfaceBounds{
        .bounds = {.center = kAside + glm::vec3(kSurfaceDistance, 0.f, 0.f), .radius = kSurfaceRadius},
        .camera = camera};
}

/// A candidate standing at kAside looking +X.
TextureCameraCandidate Aside(ECS::Entity camera, TextureCameraUpdate update)
{
    return TextureCameraCandidate{.frustum = FrustumOf(kAside, glm::vec3(1.f, 0.f, 0.f)),
                                  .camera = camera,
                                  .update = update};
}

/// A candidate standing behind the main camera looking +Z, away from what it
/// sees.
TextureCameraCandidate Behind(ECS::Entity camera, TextureCameraUpdate update)
{
    return TextureCameraCandidate{.frustum = FrustumOf(glm::vec3(0.f), glm::vec3(0.f, 0.f, 1.f)),
                                  .camera = camera,
                                  .update = update};
}

std::vector<ECS::Entity> Order(std::span<const TextureCameraCandidate> cameras,
                               std::span<const CameraSurfaceBounds> surfaces)
{
    std::vector<ECS::Entity> order;
    Runtime::OrderTextureCameras(MainFrustum(), cameras, surfaces, order);
    return order;
}

constexpr ECS::Entity kFirst{.index = 1, .generation = 1};
constexpr ECS::Entity kSecond{.index = 2, .generation = 1};
} // namespace

TEST_CASE("A camera drawn while seen draws when a surface showing it is in view, and not otherwise")
{
    const std::vector<TextureCameraCandidate> cameras{Aside(kFirst, TextureCameraUpdate::WhileSeen)};

    const std::vector<CameraSurfaceBounds> seen{InFrontOfMain(kFirst)};
    CHECK(Order(cameras, seen) == std::vector<ECS::Entity>{kFirst});

    const std::vector<CameraSurfaceBounds> unseen{BehindMain(kFirst)};
    CHECK(Order(cameras, unseen).empty());
    CHECK(Order(cameras, {}).empty());
}

TEST_CASE("A camera seen only through another camera's picture draws, and draws first")
{
    // The second camera's surface is in front of the main camera; the first
    // camera's is only in front of the second camera.
    const std::vector<TextureCameraCandidate> cameras{Behind(kSecond, TextureCameraUpdate::WhileSeen),
                                                      Aside(kFirst, TextureCameraUpdate::WhileSeen)};
    const std::vector<CameraSurfaceBounds> surfaces{InFrontOfMain(kSecond),
                                                    CameraSurfaceBounds{.bounds = BehindMain(kFirst).bounds,
                                                                        .camera = kFirst}};
    CHECK(Order(cameras, surfaces) == std::vector<ECS::Entity>{kFirst, kSecond});
}

TEST_CASE("Each mode draws when it says, whether or not anything shows it")
{
    SUBCASE("Always draws with no surface")
    {
        const std::vector<TextureCameraCandidate> cameras{Aside(kFirst, TextureCameraUpdate::Always)};
        CHECK(Order(cameras, {}) == std::vector<ECS::Entity>{kFirst});
    }
    SUBCASE("While on draws only while on, even in view")
    {
        TextureCameraCandidate camera = Aside(kFirst, TextureCameraUpdate::WhileOn);
        const std::vector<CameraSurfaceBounds> seen{InFrontOfMain(kFirst)};
        CHECK(Order(std::vector<TextureCameraCandidate>{camera}, {}) == std::vector<ECS::Entity>{kFirst});
        camera.on = false;
        CHECK(Order(std::vector<TextureCameraCandidate>{camera}, seen).empty());
    }
    SUBCASE("When asked draws only when asked, even in view")
    {
        TextureCameraCandidate camera = Aside(kFirst, TextureCameraUpdate::WhenAsked);
        const std::vector<CameraSurfaceBounds> seen{InFrontOfMain(kFirst)};
        CHECK(Order(std::vector<TextureCameraCandidate>{camera}, seen).empty());
        camera.asked = true;
        CHECK(Order(std::vector<TextureCameraCandidate>{camera}, {}) == std::vector<ECS::Entity>{kFirst});
    }
}

TEST_CASE("A camera that must redraw draws whatever its mode says")
{
    TextureCameraCandidate camera = Aside(kFirst, TextureCameraUpdate::WhileOn);
    camera.on = false;
    camera.redraw = true;
    CHECK(Order(std::vector<TextureCameraCandidate>{camera}, {}) == std::vector<ECS::Entity>{kFirst});
}

TEST_CASE("A camera drawn regardless makes what it sees draw too")
{
    // The second camera draws always; the first camera's surface is only in
    // front of the second.
    const std::vector<TextureCameraCandidate> cameras{Aside(kSecond, TextureCameraUpdate::Always),
                                                      Behind(kFirst, TextureCameraUpdate::WhileSeen)};
    const std::vector<CameraSurfaceBounds> surfaces{InFrontOfAside(kFirst)};
    CHECK(Order(cameras, surfaces) == std::vector<ECS::Entity>{kFirst, kSecond});
}

TEST_CASE("Two cameras showing each other both draw, once each")
{
    const std::vector<TextureCameraCandidate> cameras{Aside(kFirst, TextureCameraUpdate::Always),
                                                      Behind(kSecond, TextureCameraUpdate::WhileSeen)};
    // The first sees the second's surface; the second sees the first's.
    const std::vector<CameraSurfaceBounds> surfaces{InFrontOfAside(kSecond),
                                                    CameraSurfaceBounds{.bounds = BehindMain(kFirst).bounds,
                                                                        .camera = kFirst}};
    const std::vector<ECS::Entity> order = Order(cameras, surfaces);
    REQUIRE(order.size() == 2u);
    CHECK(order[0] != order[1]);
}

TEST_CASE("A surface is left out of the view of the camera it shows, and only that one")
{
    ECS::Scene scene;
    const ECS::Entity camera = scene.Create();
    const ECS::Entity other = scene.Create();
    const ECS::Entity mirror = scene.Create();
    (void)scene.Add(mirror, Runtime::MeshRenderer{});
    (void)scene.Add(mirror, Runtime::CameraSurface{.camera = camera});
    const Runtime::MeshRenderer &mesh = *scene.Get<Runtime::MeshRenderer>(mirror);

    CHECK(Runtime::LeftOutOfView(scene, mirror, mesh, ECS::InstanceId{}, camera));
    CHECK_FALSE(Runtime::LeftOutOfView(scene, mirror, mesh, ECS::InstanceId{}, other));
    CHECK_FALSE(Runtime::LeftOutOfView(scene, mirror, mesh, ECS::InstanceId{}, ECS::NullEntity));
}

TEST_CASE("Switching a camera the way it already is changes nothing")
{
    ECS::Scene scene;
    const ECS::Entity camera = scene.Create();
    (void)scene.Add(camera, Runtime::Camera{});
    (void)scene.Add(camera, Runtime::TextureCamera{.update = TextureCameraUpdate::WhileOn, .on = false});

    Runtime::SwitchTextureCamera(scene, camera, Runtime::TextureCameraSwitch::Start);
    Runtime::SwitchTextureCamera(scene, camera, Runtime::TextureCameraSwitch::Start);
    CHECK(scene.Get<Runtime::TextureCamera>(camera)->on);
    Runtime::SwitchTextureCamera(scene, camera, Runtime::TextureCameraSwitch::Stop);
    Runtime::SwitchTextureCamera(scene, camera, Runtime::TextureCameraSwitch::Stop);
    CHECK_FALSE(scene.Get<Runtime::TextureCamera>(camera)->on);

    const ECS::Entity plain = scene.Create();
    Runtime::SwitchTextureCamera(scene, plain, Runtime::TextureCameraSwitch::Start);
    Runtime::RequestTextureCameraPicture(scene, plain);
    CHECK_FALSE(scene.Has<Runtime::TextureCamera>(plain));
    CHECK_FALSE(scene.Has<Runtime::TextureCameraRequest>(plain));

    Runtime::RequestTextureCameraPicture(scene, camera);
    Runtime::RequestTextureCameraPicture(scene, camera);
    CHECK(scene.Has<Runtime::TextureCameraRequest>(camera));
}
