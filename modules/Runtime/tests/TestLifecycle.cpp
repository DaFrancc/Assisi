/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Lifecycle.hpp>

using namespace Assisi;

// DestroyTag is an ACOMP(transient) id-only component: it carries no
// serialization hooks but must still be storable in a Scene, which now indexes
// its pools by Core::Reflect::ComponentId. These cases fail (a contract assert
// in Scene::Add) if the id-only registration is missing or mis-wired.
TEST_CASE("Lifecycle: DestroyTag can be added, queried, and drives DestroyMarked")
{
    ECS::Scene scene;

    const ECS::Entity keep   = scene.Create();
    const ECS::Entity doomed = scene.Create();

    SUBCASE("an id-only component is storable and queryable")
    {
        // Add returns non-null: the pool resolved via ComponentId and stored it.
        REQUIRE(scene.Add<Runtime::DestroyTag>(doomed) != nullptr);
        CHECK(scene.Has<Runtime::DestroyTag>(doomed));
        CHECK_FALSE(scene.Has<Runtime::DestroyTag>(keep));

        int seen = 0;
        for (auto [e, tag] : scene.Query<Runtime::DestroyTag>())
        {
            (void)tag;
            CHECK(e == doomed);
            ++seen;
        }
        CHECK(seen == 1);
    }

    SUBCASE("DestroyMarked removes exactly the tagged entities after a flush")
    {
        REQUIRE(scene.Add<Runtime::DestroyTag>(doomed) != nullptr);

        Runtime::DestroyMarked(scene); // queues Scene::Destroy (deferred)
        scene.FlushDestroyed();        // applies the removal

        CHECK(scene.IsAlive(keep));
        CHECK_FALSE(scene.IsAlive(doomed));
    }
}
