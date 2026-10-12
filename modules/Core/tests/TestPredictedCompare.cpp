/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPredictedCompare.cpp
/// @brief When a client's prediction of one field counts as the host's value.

#include <doctest/doctest.h>

#include <Assisi/Core/Reflect/PredictedCompare.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

using Assisi::Core::Reflect::kPredictedAbsoluteTolerance;
using Assisi::Core::Reflect::kPredictedRelativeTolerance;
using Assisi::Core::Reflect::PredictedFieldEqual;

namespace
{

/// The shape of a glm vector, without glm: a fixed component count and [].
struct Triple
{
    using length_type = std::int32_t;
    static constexpr length_type length() { return 3; }
    float operator[](length_type i) const { return values[static_cast<std::size_t>(i)]; }
    std::array<float, 3> values{};
};

/// A world coordinate far enough out that its float steps are coarser than
/// the absolute tolerance.
constexpr double kFarOut = 1000.0;

} // namespace

TEST_CASE("two floats a rounding apart agree, and two a real step apart do not")
{
    // Two machines compute the same step and round it differently.
    constexpr float kValue = 0.5f;
    CHECK(PredictedFieldEqual(kValue, kValue + static_cast<float>(kPredictedAbsoluteTolerance / 2.0)));
    CHECK_FALSE(PredictedFieldEqual(kValue, kValue + static_cast<float>(kPredictedAbsoluteTolerance * 2.0)));
}

TEST_CASE("a large float agrees within a fraction of itself")
{
    const float far = static_cast<float>(kFarOut);
    const float drifted = static_cast<float>(kFarOut * (1.0 + kPredictedRelativeTolerance / 2.0));
    const float moved = static_cast<float>(kFarOut * (1.0 + kPredictedRelativeTolerance * 4.0));
    CHECK(PredictedFieldEqual(far, drifted));
    CHECK_FALSE(PredictedFieldEqual(far, moved));
}

TEST_CASE("a NaN agrees with nothing, itself included")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(PredictedFieldEqual(nan, nan));
    CHECK_FALSE(PredictedFieldEqual(nan, 0.f));
}

TEST_CASE("an integer agrees only exactly")
{
    CHECK(PredictedFieldEqual(std::int32_t{3}, std::int32_t{3}));
    CHECK_FALSE(PredictedFieldEqual(std::int32_t{3}, std::int32_t{4}));
}

TEST_CASE("a vector agrees component by component")
{
    const float rounding = static_cast<float>(kPredictedAbsoluteTolerance / 2.0);
    const float step = static_cast<float>(kPredictedAbsoluteTolerance * 2.0);
    const Triple a{{1.f, 2.f, 3.f}};
    CHECK(PredictedFieldEqual(a, Triple{{1.f + rounding, 2.f, 3.f - rounding}}));
    CHECK_FALSE(PredictedFieldEqual(a, Triple{{1.f, 2.f, 3.f + step}}));
}
