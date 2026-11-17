/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Path.hpp
/// @brief A spline that stands on its own: a route something follows.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Geometry/Spline.hpp>

namespace Assisi::Runtime
{

/// @brief A route on an entity, such as one a patrol walks or a script moves
///        something along. Its nodes are in the entity's space.
///
/// A component that needs a shape of its own, such as a fog volume's outline,
/// holds its own Geometry::Spline instead of a Path, so the shape belongs to
/// what uses it.
///
/// Not replicable: a path is level data, which every machine reads from the
/// same file.
ACOMP()
struct Path
{
    AFIELD() Geometry::Spline spline;
};

} // namespace Assisi::Runtime
