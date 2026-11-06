/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SplineComponents.hpp
/// @brief A reflected component holding splines in each place a field can put
/// one, for the tests of code that finds them by reflection.

#include <Assisi/Geometry/Spline.hpp>
#include <Assisi/Prelude.hpp>

#include <vector>

namespace Assisi::Runtime::Tests
{

/// @brief A struct with a spline inside it, the way a road's lane might be.
ASTRUCT()
struct SplineLane
{
    AFIELD() Geometry::Spline centre;
    AFIELD() float width = 1.f;
};

/// @brief A spline as a field, splines in a list, and splines inside a list of
/// structs.
ACOMP()
struct SplineHolder
{
    AFIELD() Geometry::Spline outline;
    AFIELD() std::vector<Geometry::Spline> extras;
    AFIELD() std::vector<SplineLane> lanes;
    AFIELD() float unrelated = 0.f;
};

/// @brief Splines only inside a list of structs, so nothing on the component
/// itself is one.
ACOMP()
struct LaneHolder
{
    AFIELD() std::vector<SplineLane> lanes;
};

} // namespace Assisi::Runtime::Tests
