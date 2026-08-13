/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestColliderPose.cpp
/// @brief The collider overlay draws where the body is, not where its Transform
/// says it is relative to a parent.
///
/// Jolt places bodies in world space, so PhysicsWorld::AddBodyFromDescriptor
/// resolves a parented entity through its parent's world matrix before creating
/// the body. The editor's wireframe used the entity's *local* pose instead, so a
/// rigid body under a Parent — every physics-driven member of a blueprint
/// instance — was outlined at its parent-relative offset while the mesh
/// silhouette drawn in the same loop, which does use the world matrix, sat
/// correctly on the mesh. The collider view is what someone turns on to find out
/// where a body actually is, so the two disagreeing on screen is worse than
/// either being wrong alone.
///
/// The pose is also deliberately scale-free at both ends: a collider's dimensions
/// are absolute world units, so neither the parent's scale nor the body's own may
/// stretch the wireframe. That is what rules out the tempting one-liner of
/// reading Transform::worldMatrix, which carries both.

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Editor/ColliderPose.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

using namespace Assisi;
using Assisi::Editor::ColliderBodyModel;

namespace
{

/// Loose enough for a quaternion round-tripped through two matrix casts, tight
/// enough that applying a 90° parent rotation one time too many cannot pass.
constexpr float kEpsilon = 1e-4f;

bool NearlyEqual(glm::vec3 a, glm::vec3 b, float epsilon = kEpsilon)
{
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(epsilon)));
}

/// |dot| folds the q/-q double cover: two quaternions naming the same orientation
/// may differ in sign.
bool NearlyEqual(glm::quat a, glm::quat b, float epsilon = kEpsilon)
{
    return glm::abs(1.f - glm::abs(glm::dot(a, b))) < epsilon;
}

glm::vec3 TranslationOf(const glm::mat4 &model)
{
    return glm::vec3(model[3]);
}

glm::quat RotationOf(const glm::mat4 &model)
{
    return glm::quat_cast(glm::mat3(model));
}

/// True when the upper 3×3 is a pure rotation — no scale reached the matrix.
bool IsUnscaled(const glm::mat4 &model)
{
    const glm::mat3 basis(model);
    return NearlyEqual(glm::vec3(glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2])),
                       glm::vec3(1.f));
}

/// A parent frame that is neither identity nor axis-aligned with the child's, so
/// a missing conversion cannot coincidentally produce the right answer.
ECS::Transform ParentPose(float scale = 1.f)
{
    return {.position = {10.f, 2.f, -3.f},
            .rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f)),
            .scale    = glm::vec3(scale)};
}

/// Hangs @p local under a parent at @p parentPose and propagates, so the parent's
/// worldMatrix is current — which is the state the editor draws from. Returns the
/// body entity.
ECS::Entity AddParentedBody(ECS::Scene &scene, const ECS::Transform &parentPose, const ECS::Transform &local)
{
    const ECS::Entity parent = scene.Create();
    const ECS::Entity body   = scene.Create();
    REQUIRE(scene.Add(parent, parentPose) != nullptr);
    REQUIRE(scene.Add(body, local) != nullptr);
    REQUIRE(scene.Add(body, Runtime::Parent{.parent = parent}) != nullptr);
    (void)Runtime::PropagateTransforms(scene, 0);
    return body;
}

} // namespace

TEST_CASE("ColliderBodyModel: a parented body is posed in world space")
{
    const ECS::Transform parentPose = ParentPose();
    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    ECS::Scene        scene;
    const ECS::Entity body  = AddParentedBody(scene, parentPose, local);
    const glm::mat4   model = ColliderBodyModel(scene, body, local);

    // ComposeTransform is the engine's other statement of the same composition, so
    // the expectation is not this function marking its own homework.
    const ECS::Transform expected = Runtime::ComposeTransform(parentPose, local);
    CHECK(NearlyEqual(TranslationOf(model), expected.position));
    CHECK(NearlyEqual(RotationOf(model), expected.rotation));

    // The defect this file exists for: the local pose is a different place, and
    // drawing there is what the wireframe used to do.
    CHECK_FALSE(NearlyEqual(TranslationOf(model), local.position));
    CHECK_FALSE(NearlyEqual(RotationOf(model), local.rotation));
}

TEST_CASE("ColliderBodyModel: an unparented body is posed at its own transform")
{
    ECS::Scene           scene;
    const ECS::Transform local{.position = {4.f, -1.f, 2.f},
                               .rotation = glm::angleAxis(glm::radians(30.f), glm::vec3(0.f, 1.f, 0.f))};

    const ECS::Entity body = scene.Create();
    REQUIRE(scene.Add(body, local) != nullptr);
    (void)Runtime::PropagateTransforms(scene, 0);

    const glm::mat4 model = ColliderBodyModel(scene, body, local);
    CHECK(NearlyEqual(TranslationOf(model), local.position));
    CHECK(NearlyEqual(RotationOf(model), local.rotation));
}

TEST_CASE("ColliderBodyModel: a parent's scale moves the body but never stretches it")
{
    const ECS::Transform parentPose = ParentPose(2.f);
    const ECS::Transform local{.position = {1.f, 0.f, 0.f}};

    ECS::Scene        scene;
    const ECS::Entity body  = AddParentedBody(scene, parentPose, local);
    const glm::mat4   model = ColliderBodyModel(scene, body, local);

    // The offset is in the parent's space, so it scales with it...
    CHECK(NearlyEqual(TranslationOf(model), Runtime::ComposeTransform(parentPose, local).position));
    // ...but the collider's dimensions are absolute world units, exactly as
    // PhysicsWorld built the shape, so nothing scales the wireframe itself.
    CHECK(IsUnscaled(model));
}

TEST_CASE("ColliderBodyModel: the body's own scale does not reach the wireframe")
{
    const ECS::Transform parentPose = ParentPose();
    const ECS::Transform local{.position = {1.f, 0.f, 0.f},
                               .rotation = glm::angleAxis(glm::radians(45.f), glm::vec3(0.f, 1.f, 0.f)),
                               .scale    = {3.f, 1.f, 1.f}};

    ECS::Scene        scene;
    const ECS::Entity body  = AddParentedBody(scene, parentPose, local);
    const glm::mat4   model = ColliderBodyModel(scene, body, local);

    // Physics ignores the entity's scale when it builds the body. Non-uniform at
    // that, so composing it in would shear the basis rather than merely stretch
    // it — the rotation read back would not even be the body's.
    CHECK(IsUnscaled(model));
    CHECK(NearlyEqual(RotationOf(model), Runtime::ComposeTransform(parentPose, local).rotation));
}

TEST_CASE("ColliderBodyModel: a parent with no Transform leaves the body where it stands")
{
    ECS::Scene           scene;
    const ECS::Transform local{.position = {1.f, 5.f, 0.f}};

    const ECS::Entity parent = scene.Create();
    const ECS::Entity body   = scene.Create();
    REQUIRE(scene.Add(body, local) != nullptr);
    REQUIRE(scene.Add(body, Runtime::Parent{.parent = parent}) != nullptr);
    (void)Runtime::PropagateTransforms(scene, 0);

    // Nothing to resolve against — the same answer Physics gets from a resolver
    // that returns null, rather than a crash or a zeroed pose.
    const glm::mat4 model = ColliderBodyModel(scene, body, local);
    CHECK(NearlyEqual(TranslationOf(model), local.position));
}
