/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>

using namespace Assisi::App;

namespace
{
// Drive the render phase: it uses the same TopoSort as the game phases but its
// RenderContext needs no InputContext/window, so it's the cleanest way to test
// dependency ordering in isolation.
RenderContext MakeCtx(Assisi::ECS::Scene &scene)
{
    return RenderContext{scene, 0.0f, glm::mat4(1.0f), glm::mat4(1.0f)};
}

std::size_t IndexOf(const std::vector<std::string> &order, const std::string &name)
{
    return static_cast<std::size_t>(std::find(order.begin(), order.end(), name) - order.begin());
}
} // namespace

TEST_CASE("SystemRegistry: After/Before constraints determine execution order")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    // Registration order is deliberately "wrong"; constraints must fix it.
    systems.RegisterRender("Draw", record("Draw")).After("Cull");
    systems.RegisterRender("Cull", record("Cull")).After("Culling.Prepare");
    systems.RegisterRender("Culling.Prepare", record("Culling.Prepare"));

    systems.RunRender(MakeCtx(scene));

    REQUIRE(order.size() == 3);
    CHECK(IndexOf(order, "Culling.Prepare") < IndexOf(order, "Cull"));
    CHECK(IndexOf(order, "Cull") < IndexOf(order, "Draw"));
}

TEST_CASE("SystemRegistry: Before is honored symmetrically with After")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    systems.RegisterRender("Late", record("Late"));
    systems.RegisterRender("Early", record("Early")).Before("Late");

    systems.RunRender(MakeCtx(scene));

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "Early");
    CHECK(order[1] == "Late");
}

TEST_CASE("SystemRegistry: a dependency on an unregistered system still runs everything")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    systems.RegisterRender("Solo",
                           [&order](RenderContext &) { order.emplace_back("Solo"); })
        .After("GhostThatWasNeverRegistered");

    systems.RunRender(MakeCtx(scene));

    CHECK(order.size() == 1); // logged an error, but did not drop the system
}

TEST_CASE("SystemRegistry: a dependency cycle falls back to running all systems")
{
    SystemRegistry systems;
    Assisi::ECS::Scene scene;
    std::vector<std::string> order;

    auto record = [&order](const char *name) {
        return [&order, name](RenderContext &) { order.emplace_back(name); };
    };

    // A <-> B mutual dependency: unschedulable, must not silently drop systems.
    systems.RegisterRender("A", record("A")).After("B");
    systems.RegisterRender("B", record("B")).After("A");

    systems.RunRender(MakeCtx(scene));

    CHECK(order.size() == 2); // fallback to registration order — nothing dropped
}
