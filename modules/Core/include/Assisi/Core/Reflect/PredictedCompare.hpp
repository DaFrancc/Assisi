/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/PredictedCompare.hpp
/// @brief Whether a client predicted one field of an ACOMP(predicted)
/// component as the host computed it.
///
/// The generated compare of a predicted component calls this once per field.
/// Two machines do not compute a float to the same bit — fast-math and fused
/// multiply-add round differently, and a host and a client may be different
/// builds — so a float agrees within a tolerance, and a vector, quaternion or
/// matrix component by component. Everything else agrees exactly.

#include <Assisi/Math/Float.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>

namespace Assisi::Core::Reflect
{

/// Below this, two floats agree whatever their size: the smallest difference
/// a prediction is worth correcting near zero.
inline constexpr double kPredictedAbsoluteTolerance = 1e-4;

/// Above it, two floats agree within this fraction of the larger: a few dozen
/// float steps, more than two simulations' rounding drifts apart in a tick.
inline constexpr double kPredictedRelativeTolerance = 1e-5;

/// @brief A glm vector, quaternion or matrix: a fixed number of components,
/// each read with [].
template <typename T>
concept ComponentWise = requires(const T &value) {
    typename T::length_type;
    { T::length() }->std::convertible_to<std::size_t>;
    value[typename T::length_type{0}];
};

/// @brief Whether @p lhs and @p rhs hold the same value as far as a prediction
/// is concerned. A NaN agrees with nothing.
template <typename T> [[nodiscard]] bool PredictedFieldEqual(const T &lhs, const T &rhs)
{
    if constexpr (std::floating_point<T>)
    {
        // Asked first: fast-math lets the tolerance compare below call a NaN
        // close to anything. An infinity agrees only with the same infinity.
        const double lhsValue = static_cast<double>(lhs);
        const double rhsValue = static_cast<double>(rhs);
        if (!Math::IsFinite(lhsValue) || !Math::IsFinite(rhsValue))
        {
            return !Math::IsNaN(lhsValue) && Math::Identical(lhsValue, rhsValue);
        }
        const double difference = std::abs(static_cast<double>(lhs) - static_cast<double>(rhs));
        const double larger =
            std::max(std::abs(static_cast<double>(lhs)), std::abs(static_cast<double>(rhs)));
        return difference <= std::max(kPredictedAbsoluteTolerance, kPredictedRelativeTolerance * larger);
    }
    else if constexpr (ComponentWise<T>)
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(T::length()); ++i)
        {
            if (!PredictedFieldEqual(lhs[static_cast<typename T::length_type>(i)],
                                     rhs[static_cast<typename T::length_type>(i)]))
            {
                return false;
            }
        }
        return true;
    }
    else
    {
        return lhs == rhs;
    }
}

} // namespace Assisi::Core::Reflect
