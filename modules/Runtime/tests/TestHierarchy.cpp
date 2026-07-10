/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

using namespace Assisi;
using Assisi::Runtime::ParentComponent;
using Assisi::Runtime::PropagateTransforms;
using Assisi::Runtime::TransformComponent;

TEST_CASE("PropagateTransforms: a root's world matrix equals its local translation")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, TransformComponent{.position = {10.f, 0.f, 0.f}}) != nullptr);

    PropagateTransforms(scene);

    const auto *t = scene.Get<TransformComponent>(e);
    REQUIRE(t != nullptr);
    CHECK(t->worldMatrix[3][0] == doctest::Approx(10.f)); // translation lives in column 3
    CHECK(t->worldMatrix[3][1] == doctest::Approx(0.f));
}

TEST_CASE("PropagateTransforms: a child composes its parent's world transform")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, TransformComponent{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, ParentComponent{.parent = parent}) != nullptr);

    PropagateTransforms(scene);

    const auto *ct = scene.Get<TransformComponent>(child);
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
    REQUIRE(scene.Add(gp, TransformComponent{.position = {100.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(par, TransformComponent{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(par, ParentComponent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(child, ParentComponent{.parent = par}) != nullptr);

    PropagateTransforms(scene);

    CHECK(scene.Get<TransformComponent>(child)->worldMatrix[3][0] == doctest::Approx(111.f));
}

TEST_CASE("PropagateTransforms: siblings sharing a parent each compose correctly")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity c1     = scene.Create();
    const ECS::Entity c2     = scene.Create();
    REQUIRE(scene.Add(parent, TransformComponent{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c1, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c2, TransformComponent{.position = {2.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(c1, ParentComponent{.parent = parent}) != nullptr);
    REQUIRE(scene.Add(c2, ParentComponent{.parent = parent}) != nullptr);

    PropagateTransforms(scene);

    CHECK(scene.Get<TransformComponent>(c1)->worldMatrix[3][0] == doctest::Approx(11.f));
    CHECK(scene.Get<TransformComponent>(c2)->worldMatrix[3][0] == doctest::Approx(12.f));
}

TEST_CASE("PropagateTransforms: a parent without a TransformComponent acts as identity")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create(); // no TransformComponent
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(child, TransformComponent{.position = {3.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, ParentComponent{.parent = parent}) != nullptr);

    PropagateTransforms(scene);

    // Parent contributes an identity transform, so the child keeps its local.
    CHECK(scene.Get<TransformComponent>(child)->worldMatrix[3][0] == doctest::Approx(3.f));
}

TEST_CASE("PropagateTransforms: an explicit NullEntity parent is treated as a root")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, TransformComponent{.position = {4.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(e, ParentComponent{.parent = ECS::NullEntity}) != nullptr);

    PropagateTransforms(scene);

    CHECK(scene.Get<TransformComponent>(e)->worldMatrix[3][0] == doctest::Approx(4.f));
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
        REQUIRE(scene.Add(e, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
        if (i > 0)
            REQUIRE(scene.Add(e, ParentComponent{.parent = chain[i - 1]}) != nullptr);
        chain.push_back(e);
    }

    PropagateTransforms(scene);

    // Each link adds 1 on x, so node i sits at cumulative world x = i + 1.
    for (int i = 0; i < kDepth; ++i)
        CHECK(scene.Get<TransformComponent>(chain[i])->worldMatrix[3][0] ==
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
    REQUIRE(scene.Add(gp, TransformComponent{.position = {100.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, TransformComponent{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(b, TransformComponent{.position = {20.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(la, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(lb, TransformComponent{.position = {2.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, ParentComponent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(b, ParentComponent{.parent = gp}) != nullptr);
    REQUIRE(scene.Add(la, ParentComponent{.parent = a}) != nullptr);
    REQUIRE(scene.Add(lb, ParentComponent{.parent = b}) != nullptr);

    PropagateTransforms(scene);

    CHECK(scene.Get<TransformComponent>(la)->worldMatrix[3][0] == doctest::Approx(111.f)); // 100+10+1
    CHECK(scene.Get<TransformComponent>(lb)->worldMatrix[3][0] == doctest::Approx(122.f)); // 100+20+2
    // The grandparent's own visit reads back the same value its descendants
    // composed against, rather than recomputing a divergent one.
    CHECK(scene.Get<TransformComponent>(gp)->worldMatrix[3][0] == doctest::Approx(100.f));
}

// Regression for the missing cycle guard: a parent loop must not recurse until
// the stack overflows. The guard breaks the cycle and leaves finite matrices.
TEST_CASE("PropagateTransforms: a parent cycle does not overflow the stack")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    REQUIRE(scene.Add(a, TransformComponent{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(b, TransformComponent{.position = {0.f, 1.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(a, ParentComponent{.parent = b}) != nullptr); // A -> B
    REQUIRE(scene.Add(b, ParentComponent{.parent = a}) != nullptr); // B -> A (cycle)

    PropagateTransforms(scene); // must return rather than recurse to death

    const auto *at = scene.Get<TransformComponent>(a);
    const auto *bt = scene.Get<TransformComponent>(b);
    REQUIRE(at != nullptr);
    REQUIRE(bt != nullptr);
    CHECK(std::isfinite(at->worldMatrix[3][0]));
    CHECK(std::isfinite(bt->worldMatrix[3][1]));
}
