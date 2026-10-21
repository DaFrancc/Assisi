/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Float.hpp
/// @brief Float checks and comparisons that give the same answer in every build.
///
/// Optimized builds use fast-math, which lets the compiler assume NaN and
/// infinity never happen, so `x != x` and similar checks stop working. These
/// functions read the float's bits instead, which fast-math can't change.
///
/// Rules:
/// - Check for NaN or infinity with IsNaN / IsInf / IsFinite, not std::isnan.
/// - Never compare floats with `==`:
///   - "Are these two results the same?" -> NearlyEqual
///   - "Did this value change?"          -> Identical

#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Math
{

namespace Detail
{

/// Where the exponent and fraction bits sit in each float type.
template <typename T> struct FloatLayout;

template <> struct FloatLayout<float>
{
    using Bits = std::uint32_t;
    static constexpr Bits kExponent = 0x7F80'0000u;
    static constexpr Bits kFraction = 0x007F'FFFFu;
};

template <> struct FloatLayout<double>
{
    using Bits = std::uint64_t;
    static constexpr Bits kExponent = 0x7FF0'0000'0000'0000u;
    static constexpr Bits kFraction = 0x000F'FFFF'FFFF'FFFFu;
};

template <typename T> [[nodiscard]] constexpr typename FloatLayout<T>::Bits BitsOf(T value) noexcept
{
    return std::bit_cast<typename FloatLayout<T>::Bits>(value);
}

} // namespace Detail

/// @brief float or double.
template <typename T>
concept FloatingPoint = std::same_as<T, float> || std::same_as<T, double>;

/// @brief True if @p value is NaN (quiet or signaling).
template <FloatingPoint T> [[nodiscard]] constexpr bool IsNaN(T value) noexcept
{
    using Layout = Detail::FloatLayout<T>;
    const auto bits = Detail::BitsOf(value);
    return (bits & Layout::kExponent) == Layout::kExponent && (bits & Layout::kFraction) != 0;
}

/// @brief True if @p value is +infinity or -infinity.
template <FloatingPoint T> [[nodiscard]] constexpr bool IsInf(T value) noexcept
{
    using Layout = Detail::FloatLayout<T>;
    const auto bits = Detail::BitsOf(value);
    return (bits & Layout::kExponent) == Layout::kExponent && (bits & Layout::kFraction) == 0;
}

/// @brief True if @p value is a normal number: not NaN and not infinite.
template <FloatingPoint T> [[nodiscard]] constexpr bool IsFinite(T value) noexcept
{
    using Layout = Detail::FloatLayout<T>;
    return (Detail::BitsOf(value) & Layout::kExponent) != Layout::kExponent;
}

/// @brief True if every component of @p value is finite.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr bool IsFinite(const glm::vec<L, T, Q> &value) noexcept
{
    for (glm::length_t i = 0; i < L; ++i)
    {
        if (!IsFinite(value[i]))
        {
            return false;
        }
    }
    return true;
}

/// @brief True if every component of @p value is finite.
template <FloatingPoint T, glm::qualifier Q> [[nodiscard]] constexpr bool IsFinite(const glm::qua<T, Q> &value) noexcept
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) && IsFinite(value.w);
}

/// @brief The epsilon NearlyEqual uses by default.
template <FloatingPoint T> inline constexpr T kDefaultEpsilon = std::same_as<T, float> ? T(1e-5) : T(1e-12);

/// @brief True if @p lhs and @p rhs are equal within @p epsilon.
///
/// Passes when `|lhs - rhs| <= epsilon * max(1, |lhs|, |rhs|)`. The margin is
/// absolute for values up to 1 and grows with larger values, so one epsilon
/// works at any size.
///
/// Both values must be finite. For values that might be NaN or infinite, use
/// NearlyEqualSafe.
template <FloatingPoint T>
[[nodiscard]] constexpr bool NearlyEqual(T lhs, T rhs, std::type_identity_t<T> epsilon = kDefaultEpsilon<T>) noexcept
{
    // Written as sign tests so the compiler emits masks, not branches.
    const T signedDifference = lhs - rhs;
    const T difference = signedDifference < T(0) ? -signedDifference : signedDifference;
    const T lhsMagnitude = lhs < T(0) ? -lhs : lhs;
    const T rhsMagnitude = rhs < T(0) ? -rhs : rhs;
    const T largest = lhsMagnitude > rhsMagnitude ? lhsMagnitude : rhsMagnitude;
    const T scale = largest > T(1) ? largest : T(1);
    return difference <= epsilon * scale;
}

/// @brief NearlyEqual on each component. Every component must be finite.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr bool NearlyEqual(const glm::vec<L, T, Q> &lhs, const glm::vec<L, T, Q> &rhs,
                                         std::type_identity_t<T> epsilon = kDefaultEpsilon<T>) noexcept
{
    for (glm::length_t i = 0; i < L; ++i)
    {
        if (!NearlyEqual(lhs[i], rhs[i], epsilon))
        {
            return false;
        }
    }
    return true;
}

/// @brief NearlyEqual for values that might be NaN or infinite. Slightly slower.
///
/// NaN is never equal to anything, including itself. Infinity is equal only to
/// the same infinity.
template <FloatingPoint T>
[[nodiscard]] constexpr bool NearlyEqualSafe(T lhs, T rhs, std::type_identity_t<T> epsilon = kDefaultEpsilon<T>) noexcept
{
    if (IsNaN(lhs) || IsNaN(rhs) || IsNaN(epsilon))
    {
        return false;
    }
    if (IsInf(lhs) || IsInf(rhs))
    {
        return Detail::BitsOf(lhs) == Detail::BitsOf(rhs);
    }
    return NearlyEqual(lhs, rhs, epsilon);
}

/// @brief NearlyEqualSafe on each component.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr bool NearlyEqualSafe(const glm::vec<L, T, Q> &lhs, const glm::vec<L, T, Q> &rhs,
                                             std::type_identity_t<T> epsilon = kDefaultEpsilon<T>) noexcept
{
    for (glm::length_t i = 0; i < L; ++i)
    {
        if (!NearlyEqualSafe(lhs[i], rhs[i], epsilon))
        {
            return false;
        }
    }
    return true;
}

/// @brief True if @p lhs and @p rhs have exactly the same bits.
///
/// Use it to check whether a value changed, where any change counts. Unlike
/// `==`, a NaN equals the same NaN, and 0 and -0 are different.
template <FloatingPoint T> [[nodiscard]] constexpr bool Identical(T lhs, T rhs) noexcept
{
    return Detail::BitsOf(lhs) == Detail::BitsOf(rhs);
}

/// @brief Identical on each component.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr bool Identical(const glm::vec<L, T, Q> &lhs, const glm::vec<L, T, Q> &rhs) noexcept
{
    for (glm::length_t i = 0; i < L; ++i)
    {
        if (!Identical(lhs[i], rhs[i]))
        {
            return false;
        }
    }
    return true;
}

/// @brief Identical on each component.
template <FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr bool Identical(const glm::qua<T, Q> &lhs, const glm::qua<T, Q> &rhs) noexcept
{
    return Identical(lhs.x, rhs.x) && Identical(lhs.y, rhs.y) && Identical(lhs.z, rhs.z) && Identical(lhs.w, rhs.w);
}

} // namespace Assisi::Math
