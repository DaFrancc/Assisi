/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
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
    constexpr int32_t kDepth = 64;
    std::vector<ECS::Entity> chain;
    chain.reserve(kDepth);
    for (int32_t i = 0; i < kDepth; ++i)
    {
        const ECS::Entity e = scene.Create();
        REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
        if (i > 0)
            REQUIRE(scene.Add(e, Parent{.parent = chain[static_cast<std::size_t>(i - 1)]}) != nullptr);
        chain.push_back(e);
    }

    PropagateTransforms(scene, 0);

    // Each link adds 1 on x, so node i sits at cumulative world x = i + 1.
    for (int32_t i = 0; i < kDepth; ++i)
        CHECK(scene.Get<Transform>(chain[static_cast<std::size_t>(i)])->worldMatrix[3][0] ==
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

// A parent loop must not recurse until the stack overflows: the cycle guard
// breaks it and leaves finite matrices.
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

// Attaching a Parent after the first propagation must dirty the child: Scene::Add
// does not stamp the child's Transform, so only Parent being ACOMP(tracked) — and
// PropagateTransforms keying on its change tick too — makes the child recompose.
TEST_CASE("PropagateTransforms: attaching a Parent after propagation dirties the child")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);
    REQUIRE(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(1.f)); // still a root

    // Attach the child to the parent AFTER the first pass, without touching its
    // Transform. The next propagation must recompose the child against the parent.
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);
    PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(11.f)); // 10 + 1
}

TEST_CASE("PropagateTransforms: detaching a Parent after propagation dirties the child" *
          doctest::should_fail())
{
    // Open, and the mirror of the attach case above. Attaching is caught
    // because Parent is ACOMP(tracked) and Add stamps its change tick; detaching
    // through Remove<Parent> stamps nothing and leaves no component behind to
    // carry a tick, so Hierarchy.cpp:81's
    //   Changed<Transform>(e, lastTick) || Changed<Parent>(e, lastTick)
    // is false for the child on the next pass and it keeps the world matrix it
    // had while it was still parented. The comment there already admits the gap
    // and claims "no such site today"; that claim is what went unverified.
    //
    // The child below is left believing it sits at x = 11 after being detached
    // from a parent at x = 10 — one full parent offset out, for as long as
    // nothing else happens to move it.
    //
    // should_fail until detach dirties the child; the fix removes this decorator.
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);
    REQUIRE(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(11.f)); // 10 + 1

    // Detach without touching the child's Transform — the exact counterpart of
    // the Add above.
    scene.Remove<Parent>(child);
    PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(1.f)); // a root again
}

// Reparenting an already-parented child — through the stamping GetMut<Parent>, the
// way a real reparent must — has to follow the new parent. The child's own
// Transform is never touched, so only the Parent change tick carries the move.
TEST_CASE("PropagateTransforms: reparenting a child to a different parent follows the new parent")
{
    ECS::Scene scene;
    const ECS::Entity p1    = scene.Create();
    const ECS::Entity p2    = scene.Create();
    const ECS::Entity child = scene.Create();
    REQUIRE(scene.Add(p1, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(p2, Transform{.position = {20.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = p1}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);
    REQUIRE(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(11.f)); // 10 + 1

    // Reparent to p2 through GetMut<Parent>, which stamps the Parent change tick.
    scene.GetMut<Parent>(child)->parent = p2;
    PropagateTransforms(scene, tick);

    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(21.f)); // 20 + 1
}

// ── Propagation must not stamp its own output ────────────────────────────────
// PropagateTransforms writes Transform::worldMatrix, and Transform is
// ACOMP(tracked) — but that write goes through the non-stamping Scene::Get and the
// enumerating loop is deliberately a plain Query, not QueryMut. Both choices are
// load-bearing rather than oversights, and both are easy to "fix" into a bug, so
// they are pinned here.
//
// If either stamped, every entity the pass touched would carry a tick newer than
// the one it returns, so the next pass would find the whole scene dirty forever
// (the dirty-skip becomes a no-op) — and network delta replication, filtering on
// the same signal, would ship every Transform every tick. Safe because worldMatrix
// is derived output: no AFIELD, never serialized, never replicated, and a peer
// rebuilds it from the local TRS in its own propagation pass.

TEST_CASE("PropagateTransforms: a pass burns no change ticks and does not re-dirty itself")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);

    // A real move of the parent through the stamping GetMut — the only write in
    // this pass that is allowed to advance the tick.
    scene.GetMut<Transform>(parent)->position.x = 100.f;
    const uint64_t afterMove = scene.CurrentChangeTick();
    REQUIRE(afterMove > tick);

    const uint64_t tick2 = PropagateTransforms(scene, tick);
    REQUIRE(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(101.f)); // recomputed

    // Nothing in the pass stamped: the tick it returns is the one the move left.
    CHECK(tick2 == afterMove);
    CHECK(scene.CurrentChangeTick() == afterMove);

    // And so a pass resuming from tick2 finds nothing dirty — no self-retrigger.
    // Proven by the value, not just the tick: poke worldMatrix to a sentinel and
    // check the skipping pass leaves it alone.
    scene.Get<Transform>(child)->worldMatrix[3][0] = -1.f;
    const uint64_t tick3 = PropagateTransforms(scene, tick2);
    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(-1.f)); // skipped
    CHECK(tick3 == tick2);
}

TEST_CASE("PropagateTransforms: moving a parent leaves the child's local Transform unchanged")
{
    ECS::Scene scene;
    const ECS::Entity parent = scene.Create();
    const ECS::Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {10.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    const uint64_t tick = PropagateTransforms(scene, 0);

    scene.GetMut<Transform>(parent)->position.x = 100.f;
    PropagateTransforms(scene, tick);

    // The child's world position moved, so a naive reading says "the child
    // changed" — but what is tracked (and replicated) is the *local* TRS, and that
    // genuinely did not change. Only the parent reports changed, which is exactly
    // the minimal delta: a peer applies the parent's new TRS and its own
    // propagation rebuilds the child. Stamping the child here would be redundant
    // traffic that grows with subtree size.
    CHECK(scene.Changed<Transform>(parent, tick));
    CHECK_FALSE(scene.Changed<Transform>(child, tick));
    CHECK(scene.Get<Transform>(child)->worldMatrix[3][0] == doctest::Approx(101.f));
}
