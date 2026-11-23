/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file Look.cpp
/// @brief The first-person template's look. Look.hpp says what it does, how to
///        drop it in and what to change; the turning and tilting math is here.

#include "Player/Look.hpp"

#include "Player/CameraRig.hpp"

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace Game
{

namespace
{

/// The most parents walked up from a camera, so a hierarchy with a loop in it
/// cannot hang a frame. Far deeper than any rig.
constexpr std::uint32_t kMaxHierarchyDepth = 32;

/// World up, which turning is about.
constexpr glm::vec3 kUp{0.f, 1.f, 0.f};

/// A view's own right, which tilting is about.
constexpr glm::vec3 kRight{1.f, 0.f, 0.f};

/// A view's forward.
constexpr glm::vec3 kForward{0.f, 0.f, -1.f};

/// @p facing turned left by @p radians about world up. About world up rather
/// than its own axis, so pitch and roll cannot leak in and tip it over after
/// enough looking around.
glm::quat Turned(const glm::quat &facing, float radians)
{
    return glm::normalize(glm::angleAxis(radians, kUp) * facing);
}

/// How far @p rotation's forward looks up from level, in radians.
float PitchOf(const glm::quat &rotation)
{
    return std::asin(glm::clamp((rotation * kForward).y, -1.f, 1.f));
}

/// @p maxPitchDegrees in radians, held within kMaxLookPitchDegrees.
float PitchLimit(float maxPitchDegrees)
{
    return glm::radians(glm::clamp(maxPitchDegrees, 0.f, kMaxLookPitchDegrees));
}

/// @p eye, a rotation that only pitches, tilted up by @p radians within
/// @p maxPitchDegrees of level. Rebuilt from the angle rather than multiplied
/// in, so the clamp is a clamp: composing a delta each step and then limiting
/// the result lets it creep past the limit and stick there.
glm::quat Tilted(const glm::quat &eye, float radians, float maxPitchDegrees)
{
    const float limit = PitchLimit(maxPitchDegrees);
    return glm::normalize(glm::angleAxis(glm::clamp(PitchOf(eye) + radians, -limit, limit), kRight));
}

/// @p view, a rotation in the world, turned left by @p yaw and tilted up by
/// @p pitch within @p maxPitchDegrees of level, by the same clamp as Tilted,
/// so a view drawn ahead of the step stops where the step will stop it.
glm::quat Looked(const glm::quat &view, float yaw, float pitch, float maxPitchDegrees)
{
    const glm::quat turned = Turned(view, yaw);
    const float limit = PitchLimit(maxPitchDegrees);
    const float current = PitchOf(turned);
    const float target = glm::clamp(current + pitch, -limit, limit);
    return glm::normalize(turned * glm::angleAxis(target - current, kRight));
}

/// The rotation in the world the last fixed step left @p entity at: its own
/// Transform's and every parent's, composed. Unblended, so a view turned on
/// from it follows the mouse every frame rather than trailing the blend
/// between steps.
glm::quat StepRotation(const Assisi::ECS::Scene &scene, Assisi::ECS::Entity entity)
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    for (std::uint32_t depth = 0; depth < kMaxHierarchyDepth && entity != Assisi::ECS::NullEntity; ++depth)
    {
        if (const Assisi::ECS::Transform *transform = scene.Get<Assisi::ECS::Transform>(entity))
        {
            rotation = transform->rotation * rotation;
        }
        const Assisi::ECS::Parent *parent = scene.Get<Assisi::ECS::Parent>(entity);
        entity = parent != nullptr ? parent->parent : Assisi::ECS::NullEntity;
    }
    return rotation;
}

} // namespace

void LookSystem(Assisi::App::SystemContext &ctx)
{
    using Assisi::App::PlayerInput;
    using Assisi::ECS::Transform;

    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [character, controls, look, rig] : scene.Query<PlayerInput, Look, ViewRig>())
    {
        if (controls.lookYaw != 0.f)
        {
            if (Transform *body = scene.GetMut<Transform>(character))
            {
                body->rotation = Turned(body->rotation, controls.lookYaw);
            }
        }
        if (controls.lookPitch != 0.f)
        {
            if (Transform *view = scene.GetMut<Transform>(rig.camera))
            {
                view->rotation = Tilted(view->rotation, controls.lookPitch, look.maxPitchDegrees);
            }
        }
    }
}

void LookAheadSystem(Assisi::App::SystemContext &ctx)
{
    using Assisi::Runtime::CameraDrawPose;

    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [character, controls, look, rig, local] :
         scene.Query<Assisi::App::PlayerInput, Look, ViewRig, Assisi::App::LocalPlayer>())
    {
        (void)character;
        (void)local;
        if (!scene.IsAlive(rig.camera))
        {
            continue;
        }
        const glm::quat drawn =
            Looked(StepRotation(scene, rig.camera), controls.lookYaw, controls.lookPitch, look.maxPitchDegrees);
        if (CameraDrawPose *pose = scene.GetMut<CameraDrawPose>(rig.camera))
        {
            pose->rotation = drawn;
        }
        else
        {
            (void)scene.Add<CameraDrawPose>(rig.camera, CameraDrawPose{.rotation = drawn, .offset = glm::vec3{0.f}});
        }
    }

    // A character that has stopped being this machine's player keeps no pose
    // from when it was: its camera is drawn as its Transform has it again.
    // Collected first, so no pool changes while a query walks it.
    std::vector<Assisi::ECS::Entity> stale;
    for (auto [character, rig] : scene.Query<ViewRig>())
    {
        if (!scene.Has<Assisi::App::LocalPlayer>(character) && scene.Has<CameraDrawPose>(rig.camera))
        {
            stale.push_back(rig.camera);
        }
    }
    for (const Assisi::ECS::Entity camera : stale)
    {
        (void)scene.Remove<CameraDrawPose>(camera);
    }
}

} // namespace Game
