/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

using namespace Assisi;
using Assisi::Runtime::Parent;
using Assisi::Runtime::PropagateTransforms;
using Assisi::Runtime::Transform;

TEST_CASE("PropagateTransforms: a root's world matrix equals its local translation")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);

    PropagateTransforms(scene, 0);

    const auto *t = scene.Get<Transform>(e);
    REQUIRE(t != nullptr);
    CHECK(t->worldMatrix[3][0] == doctest::Approx(10.f)); // translation lives in column 3
    CHECK(t->worldMatrix[3][1] == doctest::Approx(0.f));
}

TEST_CASE("PropagateTransforms: a child composes its parent's world transform")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    PropagateTransforms(scene, 0);

    const auto *ct = scene.Get<Transform>(child);
    REQUIRE(ct != nullptr);
    CHECK(ct->worldMatrix[3][0] == doctest::Approx(11.f)); // 10 (parent) + 1 (child)
    CHECK(ct->worldMatrix[3][1] == doctest::Approx(0.f));
}

TEST_CASE("PropagateTransforms: a three-deep chain composes transitively")
{
    ECS::Scene scene;
    const ECS::Entity gp    = scene.Create();
    const ECS::Entity par   = scene.Create();
    const ECS::Entity child = scene.Create();
    REQUIRE(scene.Add(gp, Transform{.position = {100.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(par, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(par, Parent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = par}) != nullptr);

    PropagateTransforms(scene, 0);

    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(111.f));
}

TEST_CASE("PropagateTransforms: siblings sharing a parent each compose correctly")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity c1     = scene.Create();
    const ECS::Entity c2     = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c1, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c2, Transform{.position = {2.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c1, Parent{.parent = parent}) != nullptr);
    REQUIRE(scene.Add(c2, Parent{.parent = parent}) != nullptr);

    PropagateTransforms(scene, 0);

    CHECK(scene.Get<Transform>(c1)->worldMatrix[3][0] == doctest::Approx(11.f));
    CHECK(scene.Get<Transform>(c2)->worldMatrix[3][0] == doctest::Approx(12.f));
}

TEST_CASE("PropagateTransforms: a parent without a Transform acts as identity")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create(); // no Transform
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(child, Transform{.position = {3.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    PropagateTransforms(scene, 0);

    // Parent contributes an identity transform, so the child keeps its local.
    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(3.f));
}

TEST_CASE("PropagateTransforms: an explicit NullEntity parent is treated as a root")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {4.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(e, Parent{.parent = ECS::NullEntity}) != nullptr);

    PropagateTransforms(scene, 0);

    CHECK(scene.Get<Transform>(e)->worldMatrix[3][0] == doctest::Approx(4.f));
}

// Deep nesting: a long parent chain must compose transitively without losing a
// level, and — because the recursion memoises each ancestor as a side effect —
// every intermediate node's world matrix must be individually correct, not just
// the leaf's. This exercises far more recursion depth than the three-deep case.
TEST_CASE("PropagateTransforms: a deep chain composes correctly at every level")
{
    ECS::Scene scene;
    constexpr int kDepth = 64;
    std::vector<ECS::Entity> chain;
    chain.reserve(kDepth);
    for (int i = 0; i < kDepth; ++i)
    {
        const ECS::Entity e = scene.Create();
        REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
        if (i > 0)
            REQUIRE(scene.Add(e, Parent{.parent = chain[i - 1]}) != nullptr);
        chain.push_back(e);
    }

    PropagateTransforms(scene, 0);

    // Each link adds 1 on x, so node i sits at cumulative world x = i + 1.
    for (int i = 0; i < kDepth; ++i)
        CHECK(scene.Get<Transform>(chain[i])->worldMatrix[3][0] ==
              doctest::Approx(static_cast<float>(i + 1)));
}

// Memoisation correctness: a shared ancestor is computed once — cached the first
// time any descendant forces its resolution — and reused for every other branch
// and for the ancestor's own query visit. Two subtrees off one grandparent must
// both compose against the same cached transform and land in the right place.
TEST_CASE("PropagateTransforms: a shared ancestor is memoised correctly across branches")
{
    ECS::Scene scene;
    const ECS::Entity gp = scene.Create();
    const ECS::Entity a  = scene.Create(); // branch A root
    const ECS::Entity b  = scene.Create(); // branch B root
    const ECS::Entity la = scene.Create(); // leaf under A
    const ECS::Entity lb = scene.Create(); // leaf under B
    REQUIRE(scene.Add(gp, Transform{.position = {100.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(b, Transform{.position = {20.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(la, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(lb, Transform{.position = {2.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, Parent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(b, Parent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(la, Parent{.parent = a}) != nullptr);
    REQUIRE(scene.Add(lb, Parent{.parent = b}) != nullptr);

    PropagateTransforms(scene, 0);

    CHECK(scene.Get<Transform>(la)->worldMatrix[3][0] == doctest::Approx(111.f)); // 100+10+1
    CHECK(scene.Get<Transform>(lb)->worldMatrix[3][0] == doctest::Approx(122.f)); // 100+20+2
    // The grandparent's own visit reads back the same value its descendants
    // composed against, rather than recomputing a divergent one.
    CHECK(scene.Get<Transform>(gp)->worldMatrix[3][0] == doctest::Approx(100.f));
}

// Regression for the missing cycle guard: a parent loop must not recurse until
// the stack overflows. The guard breaks the cycle and leaves finite matrices.
TEST_CASE("PropagateTransforms: a parent cycle does not overflow the stack")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    REQUIRE(scene.Add(a, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(b, Transform{.position = {0.f, 1.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, Parent{.parent = b}) != nullptr); // A -> B
    REQUIRE(scene.Add(b, Parent{.parent = a}) != nullptr); // B -> A (cycle)

    PropagateTransforms(scene, 0); // must return rather than recurse to death

    const auto *at = scene.Get<Transform>(a);
    const auto *bt = scene.Get<Transform>(b);
    REQUIRE(at != nullptr);
    REQUIRE(bt != nullptr);
    CHECK(std::isfinite(at->worldMatrix[3][0]));
    CHECK(std::isfinite(bt->worldMatrix[3][1]));
}

// ── Change-detection dirty-skip (Transform is ACOMP(tracked)) ─────────────────

TEST_CASE("PropagateTransforms: an unchanged entity is not recomputed")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);
    CHECK(scene.Get<Transform>(e)->worldMatrix[3][0] == doctest::Approx(10.f));

    // Mutate the local position through the NON-stamping Get, so no change tick is
    // recorded. A propagation resuming from `tick` must treat the entity as
    // unchanged and leave worldMatrix at its old value (proving the skip).
    scene.Get<Transform>(e)->position.x = 999.f;
    const uint64_t tick2 = PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(e)->worldMatrix[3][0] == doctest::Approx(10.f)); // skipped, not 999
    CHECK(tick2 == tick);                                                       // no new writes
}

TEST_CASE("PropagateTransforms: a change via GetMut is recomputed")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);

    // GetMut stamps a change tick, so the resuming propagation must recompute.
    scene.GetMut<Transform>(e)->position.x = 42.f;
    PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(e)->worldMatrix[3][0] == doctest::Approx(42.f));
}

TEST_CASE("PropagateTransforms: moving a parent recomputes an unchanged child")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);
    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(11.f));

    // Move only the parent; the child's own Transform is untouched. The child must
    // still be recomputed because its ancestor changed.
    scene.GetMut<Transform>(parent)->position.x = 100.f;
    PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(101.f)); // 100 + 1
}
