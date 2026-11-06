/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSplineFields.cpp
/// @brief The editor finds a component's splines by reflection: as a field, in a
///        list, and inside a list of structs, and settles every one it finds.

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <vector>

#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Editor/SplineFields.hpp>
#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Runtime/Path.hpp>
#include <Assisi/Runtime/Tests/RuleComponents.hpp>
#include <Assisi/Runtime/Tests/SplineComponents.hpp>

using namespace Assisi;
using Geometry::Spline;
using Geometry::SplineNode;
using Geometry::SplineNodeMode;
using Runtime::Tests::SplineHolder;
using Runtime::Tests::SplineLane;

namespace
{

const Core::Reflect::ComponentMeta &MetaNamed(const char *name)
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find(name);
    REQUIRE(meta != nullptr);
    return *meta;
}

/// A spline told apart from the others by where its one node sits.
Spline Marked(float x)
{
    Spline spline;
    spline.nodes = {SplineNode{.position = {x, 0.f, 0.f}}, SplineNode{.position = {x, 0.f, 5.f}},
                    SplineNode{.position = {x + 3.f, 0.f, 6.f}}};
    return spline;
}

SplineHolder FilledHolder()
{
    SplineHolder holder;
    holder.outline = Marked(1.f);
    holder.extras = {Marked(2.f), Marked(3.f)};
    holder.lanes = {SplineLane{.centre = Marked(4.f)}, SplineLane{.centre = Marked(5.f)}};
    return holder;
}

} // namespace

TEST_CASE("SplineFields: a component holding a spline anywhere is told from one holding none")
{
    CHECK(Editor::HoldsSpline(MetaNamed("SplineHolder").fields));
    CHECK(Editor::HoldsSpline(MetaNamed("Path").fields));
    CHECK(Editor::HoldsSpline(MetaNamed("LaneHolder").fields));
    CHECK_FALSE(Editor::HoldsSpline(MetaNamed("LoadState").fields));
}

TEST_CASE("SplineFields: every spline is found, as a field, in a list and inside a list of structs")
{
    const SplineHolder holder = FilledHolder();
    std::vector<float> found;
    Editor::ForEachSpline(&holder, MetaNamed("SplineHolder").fields,
                          [&found](const Spline &spline) { found.push_back(spline.nodes[0].position.x); });
    CHECK(found == std::vector<float>{1.f, 2.f, 3.f, 4.f, 5.f});
}

TEST_CASE("SplineFields: settling reaches every spline it finds")
{
    SplineHolder holder = FilledHolder();
    Editor::SettleSplines(&holder, MetaNamed("SplineHolder").fields);

    const auto settled = [](const Spline &spline)
                         {
                             Spline expected = spline;
                             Geometry::SettleHandles(expected);
                             for (std::size_t i = 0; i < spline.nodes.size(); ++i)
                             {
                                 CHECK(spline.nodes[i].handleIn == expected.nodes[i].handleIn);
                                 CHECK(spline.nodes[i].handleOut == expected.nodes[i].handleOut);
                                 CHECK(spline.nodes[i].handleOut != glm::vec3(0.f)); // something was written
                             }
                         };
    settled(holder.outline);
    settled(holder.extras[1]);
    settled(holder.lanes[1].centre);
}

TEST_CASE("SplineFields: the path to each spline leads back to it")
{
    SplineHolder holder = FilledHolder();
    std::size_t visited = 0;
    Editor::ForEachSplinePath(&holder, MetaNamed("SplineHolder").fields,
                              [&](Spline &spline, std::span<const Editor::FieldStep> path)
    {
        std::byte *found =
            Editor::ResolveFieldPath(reinterpret_cast<std::byte *>(&holder), path);
        CHECK(found == reinterpret_cast<std::byte *>(&spline));
        ++visited;
    });
    CHECK(visited == 5);
}
