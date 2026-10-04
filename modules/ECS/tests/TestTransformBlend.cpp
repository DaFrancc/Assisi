/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestTransformBlend.cpp
/// @brief The render blend: a Transform written during a fixed step is drawn
/// between its pose before the step and its pose after it, by the scene's
/// blend alpha. Transform itself always holds the pose after the step.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Math/Matrix.hpp>

using namespace Assisi;
using Assisi::ECS::FixedStepScope;
using Assisi::ECS::Parent;
using Assisi::ECS::PropagateTransforms;
using Assisi::ECS::RenderOffset;
using Assisi::ECS::Transform;

namespace
{

/// Where the last propagation drew @p entity.
glm::vec3 Drawn(const ECS::Scene &scene, ECS::Entity entity)
{
    return Math::TranslationOf(scene.Get<ECS::WorldMatrix>(entity)->matrix);
}

/// An entity at @p position.
ECS::Entity Spawn(ECS::Scene &scene, glm::vec3 position)
{
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add(entity, Transform{.position = position}) != nullptr);
    return entity;
}

/// One fixed step that moves @p entity to @p position.
void StepTo(ECS::Scene &scene, ECS::Entity entity, glm::vec3 position)
{
    const FixedStepScope step(scene);
    scene.GetMut<Transform>(entity)->position = position;
}

/// A quarter turn about +Y.
const glm::quat kQuarterTurn = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));

/// How many random quaternion pairs the unit-length check blends.
constexpr int32_t kBlendSamples = 100;

/// Largest deviation from unit length a blended rotation may have. A raw
/// reciprocal square root is about 1e-4 off, which visibly skews a mesh.
constexpr float kUnitTolerance = 2e-6f;

} // namespace

TEST_CASE("TransformBlend: a Transform written in a fixed step is drawn alpha of the way from its old pose")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, e, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == doctest::Approx(2.5f));
    // The component holds the step's pose; only the drawing is blended.
    CHECK(scene.Get<Transform>(e)->position.x == 10.f);
}

TEST_CASE("TransformBlend: a write through QueryMut in a fixed step blends like one through GetMut")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    {
        const FixedStepScope step(scene);
        for (auto [entity, transform] : scene.QueryMut<Transform>())
        {
            transform->position = glm::vec3(10.f, 0.f, 0.f);
        }
    }
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == doctest::Approx(5.f));
}

TEST_CASE("TransformBlend: a second write in the same step keeps the pose from before the step")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    {
        const FixedStepScope step(scene);
        scene.GetMut<Transform>(e)->position = glm::vec3(5.f, 0.f, 0.f);
        scene.GetMut<Transform>(e)->position = glm::vec3(10.f, 0.f, 0.f);
    }
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == doctest::Approx(5.f));
}

TEST_CASE("TransformBlend: the blend moves every frame between steps, though nothing is written")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, e, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);
    ECS::SetBlendAlpha(scene, 0.75f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == doctest::Approx(7.5f));
}

TEST_CASE("TransformBlend: a write outside a fixed step shows at once while the step's motion still blends")
{
    // A look system turns a character every frame while physics moves it once
    // a step: the turn must not lag, and the move must still be smooth.
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, e, {10.f, 0.f, 0.f});
    scene.GetMut<Transform>(e)->rotation = kQuarterTurn;
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    const glm::mat4 &world = scene.Get<ECS::WorldMatrix>(e)->matrix;
    CHECK(Math::TranslationOf(world).x == doctest::Approx(5.f));
    const glm::vec3 back = Math::ColumnOf(world, Math::MatrixColumn::Back);
    const glm::vec3 turned = kQuarterTurn * glm::vec3(0.f, 0.f, 1.f);
    CHECK(back.x == doctest::Approx(turned.x));
    CHECK(back.z == doctest::Approx(turned.z));
}

TEST_CASE("TransformBlend: an entity whose pose did not change is drawn exactly where it was")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e,
                      Transform{.position = {1.234f, -5.678f, 9.1011f},
                                .rotation = glm::normalize(glm::quat(0.3f, 0.5f, -0.2f, 0.7f)),
                                .scale = {1.5f, 0.75f, 2.f}}) != nullptr);
    uint64_t tick = PropagateTransforms(scene, 0);
    const glm::mat4 before = scene.Get<ECS::WorldMatrix>(e)->matrix;

    {
        const FixedStepScope step(scene);
        (void)scene.GetMut<Transform>(e);
    }
    ECS::SetBlendAlpha(scene, 0.37f);
    tick = PropagateTransforms(scene, tick);

    CHECK(scene.Get<ECS::WorldMatrix>(e)->matrix == before);
}

TEST_CASE("TransformBlend: an entity the next step leaves alone is drawn exactly where it stopped")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, e, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);
    REQUIRE(Drawn(scene, e).x == doctest::Approx(5.f));

    {
        const FixedStepScope step(scene);
    }
    ECS::SetBlendAlpha(scene, 0.3f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == 10.f);
}

TEST_CASE("TransformBlend: two steps with no propagation between still land the first step's mover")
{
    ECS::Scene scene;
    const ECS::Entity a = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity b = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, a, {10.f, 0.f, 0.f});
    StepTo(scene, b, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, a).x == 10.f);
    CHECK(Drawn(scene, b).x == doctest::Approx(5.f));
}

TEST_CASE("TransformBlend: a child follows its parent's blend from one frame to the next")
{
    ECS::Scene scene;
    const ECS::Entity parent = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity child = Spawn(scene, {1.f, 0.f, 0.f});
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, parent, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);
    CHECK(Drawn(scene, child).x == doctest::Approx(3.5f));

    ECS::SetBlendAlpha(scene, 0.75f);
    tick = PropagateTransforms(scene, tick);
    CHECK(Drawn(scene, child).x == doctest::Approx(8.5f));
}

TEST_CASE("TransformBlend: a snapped entity is drawn at its new pose with no slide")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    {
        const FixedStepScope step(scene);
        scene.GetMut<Transform>(e)->position = glm::vec3(10.f, 0.f, 0.f);
        ECS::SnapTransform(scene, e);
    }
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == 10.f);
}

TEST_CASE("TransformBlend: a write after a snap in the same step blends from the snapped pose")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    {
        const FixedStepScope step(scene);
        scene.GetMut<Transform>(e)->position = glm::vec3(10.f, 0.f, 0.f);
        ECS::SnapTransform(scene, e);
        scene.GetMut<Transform>(e)->position = glm::vec3(20.f, 0.f, 0.f);
    }
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == doctest::Approx(15.f));
}

TEST_CASE("TransformBlend: settling draws every entity exactly, whatever the alpha")
{
    // A paused world steps no more, but the alpha goes on cycling.
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, e, {10.f, 0.f, 0.f});
    ECS::SettleTransforms(scene);
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == 10.f);
}

TEST_CASE("TransformBlend: a spawn inside a fixed step appears where it was placed")
{
    ECS::Scene scene;
    const ECS::Entity anchor = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);
    (void)anchor;

    ECS::Entity e = ECS::NullEntity;
    {
        const FixedStepScope step(scene);
        e = Spawn(scene, {10.f, 0.f, 0.f});
    }
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).x == 10.f);
}

TEST_CASE("TransformBlend: removing an entity keeps a mover's blend with the mover")
{
    // The blend's marks sit in a lane beside the pool's dense array, and a
    // removal swaps the last entry into the gap.
    ECS::Scene scene;
    const ECS::Entity a = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity b = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity c = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, c, {10.f, 0.f, 0.f});
    scene.Destroy(a);
    scene.FlushDestroyed();
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, b).x == 0.f);
    CHECK(Drawn(scene, c).x == doctest::Approx(5.f));
}

TEST_CASE("TransformBlend: a destroyed mover is skipped, and its index's next entity draws exactly")
{
    ECS::Scene scene;
    const ECS::Entity a = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, a, {10.f, 0.f, 0.f});
    scene.Destroy(a);
    scene.FlushDestroyed();
    const ECS::Entity reused = Spawn(scene, {3.f, 0.f, 0.f});
    REQUIRE(reused.index == a.index);
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, reused).x == 3.f);
}

TEST_CASE("TransformBlend: a rotation blends along the short arc")
{
    // q and -q are the same rotation; blending toward the far one would swing
    // the long way round.
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    const glm::quat tenDegrees = glm::angleAxis(glm::radians(10.f), glm::vec3(0.f, 1.f, 0.f));
    {
        const FixedStepScope step(scene);
        scene.GetMut<Transform>(e)->rotation = -tenDegrees;
    }
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    const glm::mat4 &world = scene.Get<ECS::WorldMatrix>(e)->matrix;
    const glm::vec3 right = glm::normalize(Math::ColumnOf(world, Math::MatrixColumn::Right));
    const float degrees = glm::degrees(std::atan2(-right.z, right.x));
    CHECK(degrees == doctest::Approx(5.f).epsilon(0.01));
}

TEST_CASE("TransformBlend: a blended rotation stays unit length")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    for (int32_t sample = 0; sample < kBlendSamples; ++sample)
    {
        const float angle = 0.07f * static_cast<float>(sample);
        const glm::vec3 axis = glm::normalize(glm::vec3(std::sin(angle), 1.f, std::cos(angle * 3.f)));
        {
            const FixedStepScope step(scene);
            scene.GetMut<Transform>(e)->rotation = glm::angleAxis(angle, axis);
        }
        ECS::SetBlendAlpha(scene, 0.5f);
        tick = PropagateTransforms(scene, tick);

        const glm::mat4 &world = scene.Get<ECS::WorldMatrix>(e)->matrix;
        const doctest::Approx unit = doctest::Approx(1.f).epsilon(kUnitTolerance);
        CHECK(glm::length(Math::ColumnOf(world, Math::MatrixColumn::Right)) == unit);
        CHECK(glm::length(Math::ColumnOf(world, Math::MatrixColumn::Up)) == unit);
    }
}

TEST_CASE("TransformBlend: a render offset moves the drawn pose after the blend, and children follow it")
{
    ECS::Scene scene;
    const ECS::Entity parent = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity child = Spawn(scene, {1.f, 0.f, 0.f});
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, parent, {10.f, 0.f, 0.f});
    REQUIRE(scene.Add(parent, RenderOffset{.position = {0.f, 5.f, 0.f}}) != nullptr);
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, parent).x == doctest::Approx(5.f));
    CHECK(Drawn(scene, parent).y == doctest::Approx(5.f));
    CHECK(Drawn(scene, child).x == doctest::Approx(6.f));
    CHECK(Drawn(scene, child).y == doctest::Approx(5.f));
    // The offset is drawing only; the simulation pose is untouched.
    CHECK(scene.Get<Transform>(parent)->position.y == 0.f);
}

TEST_CASE("TransformBlend: removing a render offset draws the entity back where it is")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    REQUIRE(scene.Add(e, RenderOffset{.position = {0.f, 5.f, 0.f}}) != nullptr);
    uint64_t tick = PropagateTransforms(scene, 0);
    REQUIRE(Drawn(scene, e).y == doctest::Approx(5.f));

    REQUIRE(scene.Remove<RenderOffset>(e) == ECS::RemoveResult::Removed);
    tick = PropagateTransforms(scene, tick);

    CHECK(Drawn(scene, e).y == 0.f);
}

TEST_CASE("PropagateTransforms: a pass over an unchanged scene resolves nothing")
{
    ECS::Scene scene;
    for (int32_t i = 0; i < kBlendSamples; ++i)
    {
        (void)Spawn(scene, {static_cast<float>(i), 0.f, 0.f});
    }
    const uint64_t tick = PropagateTransforms(scene, 0);
    REQUIRE(ECS::LastPropagationResolved(scene) == static_cast<uint32_t>(kBlendSamples));

    (void)PropagateTransforms(scene, tick);

    CHECK(ECS::LastPropagationResolved(scene) == 0u);
}

TEST_CASE("PropagateTransforms: a second pass at the same alpha resolves nothing, a new alpha the mover and its child")
{
    ECS::Scene scene;
    const ECS::Entity parent = Spawn(scene, {0.f, 0.f, 0.f});
    const ECS::Entity child = Spawn(scene, {1.f, 0.f, 0.f});
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);
    (void)Spawn(scene, {5.f, 0.f, 0.f});
    uint64_t tick = PropagateTransforms(scene, 0);

    StepTo(scene, parent, {10.f, 0.f, 0.f});
    ECS::SetBlendAlpha(scene, 0.25f);
    tick = PropagateTransforms(scene, tick);
    tick = PropagateTransforms(scene, tick);
    CHECK(ECS::LastPropagationResolved(scene) == 0u);

    ECS::SetBlendAlpha(scene, 0.5f);
    tick = PropagateTransforms(scene, tick);
    CHECK(ECS::LastPropagationResolved(scene) == 2u);
}

TEST_CASE("Mut: writing three fields through one proxy stamps one tick")
{
    ECS::Scene scene;
    const ECS::Entity e = Spawn(scene, {0.f, 0.f, 0.f});
    const uint64_t before = scene.CurrentChangeTick();

    for (auto [entity, transform] : scene.QueryMut<Transform>())
    {
        transform->position = glm::vec3(1.f);
        transform->rotation = kQuarterTurn;
        transform->scale = glm::vec3(2.f);
    }

    CHECK(scene.CurrentChangeTick() == before + 1u);
    CHECK(scene.ChangeTick<Transform>(e) == before + 1u);
}
