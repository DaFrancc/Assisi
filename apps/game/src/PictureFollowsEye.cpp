/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "PictureFollowsEye.hpp"

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Runtime/TextureCamera.hpp>

#include <cmath>
#include <optional>

namespace Game
{

namespace
{

/// How far the clear part moves for each unit of the eye's off-axis slope (its
/// sideways distance over its distance in front), in picture heights.
constexpr float kFollowGain = 0.25f;

/// The furthest the clear part moves from the middle, in picture heights, so
/// some of the picture always shows.
constexpr float kMostOffset = 0.3f;

/// The nearest the eye can be to the surface's plane and still be followed,
/// in metres: closer, the slope grows without bound.
constexpr float kLeastDistance = 0.01f;

/// A picture's middle, in its own coordinates.
constexpr glm::vec2 kMiddle{0.5f, 0.5f};

} // namespace

void PictureFollowsEyeSystem(Assisi::App::SystemContext &ctx)
{
    Assisi::ECS::Scene &scene = ctx.world.scene;
    const std::optional<Assisi::App::SceneView> view = Assisi::App::ActiveSceneCamera(scene);
    if (!view)
    {
        return;
    }
    const glm::dvec3 eye = Assisi::ECS::WorldPositionOf(view->world);

    for (auto [entity, surface, world] : scene.Query<Assisi::Runtime::CameraSurface, Assisi::ECS::WorldMatrix>())
    {
        Assisi::Runtime::PictureVignette *const vignette =
            scene.GetMut<Assisi::Runtime::PictureVignette>(surface.camera);
        if (vignette == nullptr)
        {
            continue;
        }
        // The surface's axes, measured from its own middle, without its scale.
        const glm::dvec3 middle = Assisi::ECS::WorldPositionOf(world);
        const glm::mat4 placed = Assisi::ECS::MatrixFrom(world, middle);
        const glm::vec3 right = glm::normalize(glm::vec3(placed[0]));
        const glm::vec3 up = glm::normalize(glm::vec3(placed[1]));
        const glm::vec3 facing = glm::normalize(glm::vec3(placed[2]));
        const glm::vec3 toEye = glm::vec3(eye - middle);
        const float along = std::abs(glm::dot(toEye, facing));
        if (along < kLeastDistance)
        {
            continue;
        }
        // The clear part moves away from the eye's side: left when the eye is
        // to the right, and down the picture, whose coordinates run downward,
        // when the eye is above.
        glm::vec2 offset = glm::vec2(-glm::dot(toEye, right), glm::dot(toEye, up)) * (kFollowGain / along);
        const float length = glm::length(offset);
        if (length > kMostOffset)
        {
            offset *= kMostOffset / length;
        }
        vignette->centre = kMiddle + offset;
    }
}

} // namespace Game
