/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFloat.cpp
/// @brief What Float.hpp answers, in the optimized builds that are its reason
/// to exist.
///
/// The Math tests compile with the engine's fast-math flags, so in dev and ship
/// these run where `x != x` is already false for a NaN. Every value goes through
/// Opaque first, or the compiler could fold the answers at compile time, where
/// fast-math does not apply, and the test would pass without proving anything.

#include <doctest/doctest.h>

#include <Assisi/Math/Float.hpp>

#include <cmath>
#include <limits>

using namespace Assisi::Math;

namespace
{

/// @p value as the optimizer has to take it: read back from memory it cannot
/// see through, so nothing about it is known at compile time.
template <typename T> T Opaque(T value)
{
    volatile T stored = value;
    return stored;
}

template <typename T> struct Specials
{
    T quietNaN = Opaque(std::numeric_limits<T>::quiet_NaN());
    T signalingNaN = Opaque(std::numeric_limits<T>::signaling_NaN());
    T negativeNaN = Opaque(-std::numeric_limits<T>::quiet_NaN());
    T infinity = Opaque(std::numeric_limits<T>::infinity());
    T negativeInfinity = Opaque(-std::numeric_limits<T>::infinity());
    T largest = Opaque(std::numeric_limits<T>::max());
    T smallestNormal = Opaque(std::numeric_limits<T>::min());
    T denormal = Opaque(std::numeric_limits<T>::denorm_min());
    T zero = Opaque(T(0));
    T negativeZero = Opaque(-T(0));
    T one = Opaque(T(1));
};

} // namespace

TEST_CASE_TEMPLATE("IsNaN, IsInf and IsFinite tell every kind of value apart", T, float, double)
{
    const Specials<T> s;

    for (const T nan : {s.quietNaN, s.signalingNaN, s.negativeNaN})
    {
        CHECK(IsNaN(nan));
        CHECK_FALSE(IsInf(nan));
        CHECK_FALSE(IsFinite(nan));
    }
    for (const T infinity : {s.infinity, s.negativeInfinity})
    {
        CHECK_FALSE(IsNaN(infinity));
        CHECK(IsInf(infinity));
        CHECK_FALSE(IsFinite(infinity));
    }
    for (const T number : {s.largest, s.smallestNormal, s.denormal, s.zero, s.negativeZero, s.one, -s.largest})
    {
        CHECK_FALSE(IsNaN(number));
        CHECK_FALSE(IsInf(number));
        CHECK(IsFinite(number));
    }
}

TEST_CASE("IsNaN catches a NaN that arithmetic made, not only one that was written")
{
    // Two loads each, not one value used twice: fast-math rewrites x / x to 1
    // and x - x to 0, so the same variable on both sides would make no NaN at
    // all and leave nothing to catch.
    const float zero = Opaque(0.f);
    const float otherZero = Opaque(0.f);
    const float infinity = Opaque(std::numeric_limits<float>::infinity());
    const float otherInfinity = Opaque(std::numeric_limits<float>::infinity());
    CHECK(IsNaN(Opaque(zero / otherZero)));
    CHECK(IsNaN(Opaque(infinity - otherInfinity)));
    CHECK(IsInf(Opaque(Opaque(1.f) / zero)));
}

TEST_CASE("IsFinite on a vector or a quaternion needs every component finite")
{
    const float nan = Opaque(std::numeric_limits<float>::quiet_NaN());
    const float infinity = Opaque(std::numeric_limits<float>::infinity());

    CHECK(IsFinite(glm::vec3(1.f, 2.f, 3.f)));
    CHECK_FALSE(IsFinite(glm::vec3(1.f, nan, 3.f)));
    CHECK_FALSE(IsFinite(glm::vec2(infinity, 0.f)));
    CHECK_FALSE(IsFinite(glm::vec4(0.f, 0.f, 0.f, nan)));

    CHECK(IsFinite(glm::quat(1.f, 0.f, 0.f, 0.f)));
    CHECK_FALSE(IsFinite(glm::quat(nan, 0.f, 0.f, 0.f)));
    CHECK_FALSE(IsFinite(glm::quat(1.f, 0.f, 0.f, infinity)));
}

TEST_CASE_TEMPLATE("NearlyEqual: values a rounding apart agree, values a real step apart do not", T, float, double)
{
    const T tenth = Opaque(T(0.1));
    const T sum = Opaque(tenth + tenth + tenth);
    CHECK(NearlyEqual(sum, T(0.3)));
    CHECK(NearlyEqual(Opaque(T(0)), Opaque(-T(0))));
    CHECK_FALSE(NearlyEqual(Opaque(T(1)), Opaque(T(1.001))));
    CHECK_FALSE(NearlyEqual(Opaque(T(1)), Opaque(T(-1))));
}

TEST_CASE("NearlyEqual: the epsilon is what the caller says when the caller says one")
{
    CHECK(NearlyEqual(Opaque(1.f), Opaque(1.05f), 0.1f));
    CHECK_FALSE(NearlyEqual(Opaque(1.f), Opaque(1.2f), 0.1f));
}

TEST_CASE("NearlyEqual: the margin grows with the values, so one epsilon serves every size")
{
    // Adjacent floats near a hundred thousand are about 0.008 apart, far wider
    // than an absolute 1e-5. Scaled, the margin there is about one unit.
    const float big = Opaque(100000.f);
    CHECK(NearlyEqual(big, Opaque(100000.5f)));
    CHECK_FALSE(NearlyEqual(big, Opaque(100003.f)));

    // Below magnitude one the margin stays absolute, so two tiny values agree
    // however different they are relative to each other.
    CHECK(NearlyEqual(Opaque(1e-8f), Opaque(3e-8f)));
    CHECK_FALSE(NearlyEqual(Opaque(0.f), Opaque(1e-4f)));
}

TEST_CASE("NearlyEqual on a vector needs every component to agree")
{
    const glm::vec3 a{1.f, 2.f, 3.f};
    CHECK(NearlyEqual(a, glm::vec3(1.f, 2.f, Opaque(3.f + 1e-6f))));
    CHECK_FALSE(NearlyEqual(a, glm::vec3(1.f, 2.1f, 3.f)));
    CHECK(NearlyEqual(a, glm::vec3(1.f, 2.1f, 3.f), 0.2f));
}

TEST_CASE_TEMPLATE("NearlyEqualSafe: NaN agrees with nothing, and infinity only with itself", T, float, double)
{
    const Specials<T> s;
    CHECK_FALSE(NearlyEqualSafe(s.quietNaN, s.quietNaN));
    CHECK_FALSE(NearlyEqualSafe(s.signalingNaN, s.signalingNaN));
    CHECK_FALSE(NearlyEqualSafe(s.quietNaN, s.one));
    CHECK_FALSE(NearlyEqualSafe(s.one, s.quietNaN));
    CHECK_FALSE(NearlyEqualSafe(s.one, s.one, s.quietNaN));

    CHECK(NearlyEqualSafe(s.infinity, s.infinity));
    CHECK(NearlyEqualSafe(s.negativeInfinity, s.negativeInfinity));
    CHECK_FALSE(NearlyEqualSafe(s.infinity, s.negativeInfinity));
    CHECK_FALSE(NearlyEqualSafe(s.infinity, s.largest));
    CHECK_FALSE(NearlyEqualSafe(s.largest, s.infinity));
}

TEST_CASE_TEMPLATE("NearlyEqualSafe: between numbers it answers what NearlyEqual does", T, float, double)
{
    const T tenth = Opaque(T(0.1));
    const T sum = Opaque(tenth + tenth + tenth);
    CHECK(NearlyEqualSafe(sum, T(0.3)));
    CHECK_FALSE(NearlyEqualSafe(Opaque(T(1)), Opaque(T(1.001))));
    CHECK(NearlyEqualSafe(Opaque(T(1)), Opaque(T(1.05)), T(0.1)));
}

TEST_CASE("NearlyEqualSafe on a vector: one NaN component and it disagrees")
{
    const float nan = Opaque(std::numeric_limits<float>::quiet_NaN());
    const glm::vec3 a{1.f, 2.f, 3.f};
    CHECK(NearlyEqualSafe(a, glm::vec3(1.f, 2.f, Opaque(3.f + 1e-6f))));
    CHECK_FALSE(NearlyEqualSafe(a, glm::vec3(1.f, nan, 3.f)));
    CHECK_FALSE(NearlyEqualSafe(glm::vec3(nan), glm::vec3(nan)));
}

TEST_CASE_TEMPLATE("Identical: the same bits and nothing else", T, float, double)
{
    const Specials<T> s;
    CHECK(Identical(s.one, Opaque(T(1))));
    CHECK_FALSE(Identical(s.one, std::nextafter(s.one, T(2))));
    CHECK(Identical(s.quietNaN, Opaque(std::numeric_limits<T>::quiet_NaN())));
    CHECK_FALSE(Identical(s.quietNaN, s.one));
    CHECK_FALSE(Identical(s.one, s.quietNaN));
    CHECK_FALSE(Identical(s.zero, s.negativeZero));
}

TEST_CASE("Identical on a vector or a quaternion: a NaN written over a number is a change")
{
    const float nan = Opaque(std::numeric_limits<float>::quiet_NaN());
    const glm::vec3 velocity{0.f, -9.81f, 0.f};

    CHECK(Identical(velocity, glm::vec3(0.f, -9.81f, 0.f)));
    CHECK_FALSE(Identical(velocity, glm::vec3(nan)));
    CHECK_FALSE(Identical(glm::vec3(nan), velocity));

    CHECK(Identical(glm::quat(1.f, 0.f, 0.f, 0.f), glm::quat(1.f, 0.f, 0.f, 0.f)));
    CHECK_FALSE(Identical(glm::quat(1.f, 0.f, 0.f, 0.f), glm::quat(nan, 0.f, 0.f, 0.f)));
}

TEST_CASE("All of it answers at compile time too")
{
    static_assert(IsNaN(std::numeric_limits<float>::quiet_NaN()));
    static_assert(IsInf(-std::numeric_limits<double>::infinity()));
    static_assert(IsFinite(1.f));
    static_assert(NearlyEqual(1.f, 1.f + 1e-6f));
    static_assert(!NearlyEqual(1.f, 2.f));
    static_assert(!NearlyEqualSafe(std::numeric_limits<float>::quiet_NaN(), 1.f));
    static_assert(Identical(2.0, 2.0));
    CHECK(true);
}
