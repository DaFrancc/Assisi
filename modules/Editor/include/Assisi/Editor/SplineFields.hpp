/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SplineFields.hpp
/// @brief Every Geometry::Spline inside a reflected object, wherever it sits.
///
/// A component holds a spline as a field of its own, so the editor finds them by
/// reflection rather than by knowing each component: one that gains a Spline
/// field is drawn and settled with no editor code for it.
///
/// A spline is found as a field, as a row of a list or fixed array, and inside a
/// nested struct, at any depth. A map's values are not walked: a map hands out
/// its entries read-only, and no component keeps a spline in one.

#include <functional>
#include <span>

#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Geometry/Spline.hpp>

namespace Assisi::Editor
{

/// @brief Whether an object described by @p fields holds a spline anywhere,
///        judged from the description alone.
[[nodiscard]] bool HoldsSpline(std::span<const Core::Reflect::FieldMeta> fields);

/// @brief Calls @p visit with each spline in @p object, which @p fields
///        describes, in field order.
void ForEachSpline(const void *object, std::span<const Core::Reflect::FieldMeta> fields,
                   const std::function<void(const Geometry::Spline &)> &visit);

/// @brief Settles every spline in @p object (Geometry::SettleHandles), so each
///        Corner and Smooth node stores the handles it draws with.
///
/// Run after an edit to the object, so a node switched to Bezier afterwards
/// starts from the curve on screen.
void SettleSplines(void *object, std::span<const Core::Reflect::FieldMeta> fields);

} // namespace Assisi::Editor
