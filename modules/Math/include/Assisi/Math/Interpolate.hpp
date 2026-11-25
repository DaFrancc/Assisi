/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Interpolate.hpp
/// @brief Blending between values and easing toward them: lerp and its
/// inverse, clamping, smooth steps, slerp, frame-rate-independent damping, a
/// critically damped spring and constant-speed approach.
///
/// Written out here rather than forwarded to GLM, which the engine means to
/// replace: every caller goes through these, so that swap touches this file
/// alone. They take scalars, GLM vectors and colours, and quaternions where a
/// rotation is meant.

#include <Assisi/Math/Float.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cmath>
#include <numbers>

namespace Assisi::Math
{

namespace Detail
{

/// The dot product of two scalars, which is their product, so one formula
/// serves a scalar and a vector.
template <FloatingPoint T> [[nodiscard]] constexpr T Dot(T a, T b) noexcept
{
    return a * b;
}

template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr T Dot(const glm::vec<L, T, Q> &a, const glm::vec<L, T, Q> &b) noexcept
{
    T sum{0};
    for (glm::length_t i = 0; i < L; ++i)
    {
        sum += a[i] * b[i];
    }
    return sum;
}

template <FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr T Dot(const glm::qua<T, Q> &a, const glm::qua<T, Q> &b) noexcept
{
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace Detail

/// @brief The point @p t of the way from @p a to @p b: @p a at 0, @p b at 1,
/// and beyond them outside that range.
template <typename V, FloatingPoint T> [[nodiscard]] constexpr V Lerp(const V &a, const V &b, T t)
{
    return V(a + (b - a) * t);
}

/// @brief How far @p value is from @p a toward @p b: 0 at @p a, 1 at @p b, and
/// outside that range beyond them. Lerp's inverse. 0 when @p a equals @p b,
/// where every value is as far as any other.
template <FloatingPoint T> [[nodiscard]] constexpr T InverseLerp(T a, T b, T value) noexcept
{
    return a == b ? T{0} : (value - a) / (b - a);
}

/// @brief @p value moved from the range @p inA to @p inB onto the range
/// @p outA to @p outB, keeping where it sits between the ends: health from 0 to
/// 100 onto a bar from 0 to 400 pixels wide. Not clamped: Saturate the share
/// first, or Clamp the result, to keep it inside.
template <FloatingPoint T> [[nodiscard]] constexpr T Remap(T value, T inA, T inB, T outA, T outB) noexcept
{
    return Lerp(outA, outB, InverseLerp(inA, inB, value));
}

/// @brief @p value held inside @p low to @p high.
template <FloatingPoint T> [[nodiscard]] constexpr T Clamp(T value, T low, T high) noexcept
{
    return value < low ? low : (value > high ? high : value);
}

/// @brief @p value with each component held inside the matching ones of
/// @p low and @p high.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] constexpr glm::vec<L, T, Q> Clamp(const glm::vec<L, T, Q> &value, const glm::vec<L, T, Q> &low,
                                                const glm::vec<L, T, Q> &high) noexcept
{
    glm::vec<L, T, Q> held = value;
    for (glm::length_t i = 0; i < L; ++i)
    {
        held[i] = Clamp(value[i], low[i], high[i]);
    }
    return held;
}

/// @brief @p value held inside 0 to 1: what a share or a blend factor must be.
template <FloatingPoint T> [[nodiscard]] constexpr T Saturate(T value) noexcept
{
    return Clamp(value, T{0}, T{1});
}

/// @brief 0 at or before @p edge0, 1 at or after @p edge1, and an S-curve
/// between that starts and ends flat, so a fade or a blend eases in and out.
/// Equal edges make a hard step: 0 before them, 1 at and after.
template <FloatingPoint T> [[nodiscard]] constexpr T SmoothStep(T edge0, T edge1, T x) noexcept
{
    if (edge0 == edge1)
    {
        return x < edge0 ? T{0} : T{1};
    }
    const T t = Saturate(InverseLerp(edge0, edge1, x));
    return t * t * (T{3} - t * T{2});
}

/// @brief SmoothStep with its curvature flat at the ends as well, so nothing
/// driven by it jolts as it starts or stops speeding up (Perlin, "Improving
/// Noise"). Equal edges make the same hard step.
template <FloatingPoint T> [[nodiscard]] constexpr T SmootherStep(T edge0, T edge1, T x) noexcept
{
    if (edge0 == edge1)
    {
        return x < edge0 ? T{0} : T{1};
    }
    const T t = Saturate(InverseLerp(edge0, edge1, x));
    return t * t * t * (t * (t * T{6} - T{15}) + T{10});
}

/// @brief The rotation @p t of the way from @p a to @p b at a steady turning
/// speed, the short way round. Both must be unit quaternions.
template <FloatingPoint T, glm::qualifier Q>
[[nodiscard]] glm::qua<T, Q> Slerp(const glm::qua<T, Q> &a, glm::qua<T, Q> b, T t) noexcept
{
    // Closer than this to the same rotation, the arc is too short for its sine
    // to divide by; a normalized lerp is as good there.
    constexpr T kNearlySame = T{1} - static_cast<T>(1e-5);

    T cosine = Detail::Dot(a, b);
    // q and -q are one rotation; the one nearer a is the short way.
    if (cosine < T{0})
    {
        b = -b;
        cosine = -cosine;
    }
    if (cosine > kNearlySame)
    {
        const glm::qua<T, Q> blended = a + (b - a) * t;
        return blended * (T{1} / std::sqrt(Detail::Dot(blended, blended)));
    }
    const T angle = std::acos(cosine);
    const T sine = std::sin(angle);
    return a * (std::sin((T{1} - t) * angle) / sine) + b * (std::sin(t * angle) / sine);
}

/// @brief The share of the way to its target a damped value covers in @p dt
/// seconds, with @p seconds its time constant: `1 - exp(-dt / seconds)`.
///
/// What makes damping the same at every frame rate: ten short frames cover
/// exactly what one long frame of the same time does, where a fixed share per
/// frame would ease faster the more frames there are (Driscoll, "Frame Rate
/// Independent Damping Using Lerp"). 1 for a time constant of 0 or less, which
/// reaches the target at once even on a frame where no time passed; otherwise
/// 0 for no time passed.
template <FloatingPoint T> [[nodiscard]] T DampFactor(T seconds, T dt) noexcept
{
    if (seconds <= T{0})
    {
        return T{1};
    }
    if (dt <= T{0})
    {
        return T{0};
    }
    return T{1} - std::exp(-dt / seconds);
}

/// @brief @p current eased toward @p target over @p dt seconds, the same at
/// any frame rate. @p seconds is the time constant: about 63% of the way after
/// that long, and 95% after three times it. 0 or less arrives at once.
template <typename V, FloatingPoint T> [[nodiscard]] V Damp(const V &current, const V &target, T seconds, T dt)
{
    return Lerp(current, target, DampFactor(seconds, dt));
}

/// @brief Damp for an angle in radians, turning the short way round: from
/// 350 degrees to 10 it goes up through 360, not down through 180.
template <FloatingPoint T> [[nodiscard]] T DampAngle(T current, T target, T seconds, T dt) noexcept
{
    constexpr T kFullTurn = static_cast<T>(2.0 * std::numbers::pi);
    const T turn = std::remainder(target - current, kFullTurn);
    return current + turn * DampFactor(seconds, dt);
}

/// @brief Damp for a rotation: Slerp by the share DampFactor gives.
template <FloatingPoint T, glm::qualifier Q>
[[nodiscard]] glm::qua<T, Q> DampRotation(const glm::qua<T, Q> &current, const glm::qua<T, Q> &target, T seconds,
                                          T dt) noexcept
{
    return Slerp(current, target, DampFactor(seconds, dt));
}

/// @brief @p current moved toward @p target by a critically damped spring,
/// over @p dt seconds, with @p velocity carried from call to call.
///
/// Unlike Damp, it builds up speed and slows before arriving, so a camera or
/// a marker following something starts and stops softly. It never overshoots.
/// @p seconds is roughly how long it takes to arrive; 0 or less arrives at
/// once. Keep @p velocity beside the value, zero to start (Lowe, "Critically
/// Damped Ease-In/Ease-Out Smoothing", Game Programming Gems 4).
template <typename V, FloatingPoint T> [[nodiscard]] V SmoothDamp(const V &current, const V &target, V &velocity,
                                                                  T seconds, T dt)
{
    // exp(-x) as 1 / (1 + x + 0.48x² + 0.235x³), close enough for any frame's
    // step and cheaper than exp.
    constexpr T kSquareTerm = static_cast<T>(0.48);
    constexpr T kCubeTerm = static_cast<T>(0.235);

    if (seconds <= T{0})
    {
        velocity = V(T{0});
        return target;
    }
    if (dt <= T{0})
    {
        return current;
    }
    const T omega = T{2} / seconds;
    const T x = omega * dt;
    const T decay = T{1} / (T{1} + x + kSquareTerm * x * x + kCubeTerm * x * x * x);
    const V change = V(current - target);
    const V pull = V((velocity + change * omega) * dt);
    velocity = V((velocity - pull * omega) * decay);
    const V moved = V(target + (change + pull) * decay);
    // Past the target from where it started: stop there rather than swing back.
    if (Detail::Dot(V(target - current), V(moved - target)) > T{0})
    {
        velocity = V(T{0});
        return target;
    }
    return moved;
}

/// @brief @p current moved toward @p target by at most @p maxStep, stopping
/// exactly on it: a constant speed when @p maxStep is a speed times the frame's
/// seconds.
template <FloatingPoint T> [[nodiscard]] constexpr T MoveTowards(T current, T target, T maxStep) noexcept
{
    const T gap = target - current;
    if (gap <= maxStep && gap >= -maxStep)
    {
        return target;
    }
    return current + (gap > T{0} ? maxStep : -maxStep);
}

/// @brief MoveTowards for a point: at most @p maxStep along the straight line
/// to @p target.
template <glm::length_t L, FloatingPoint T, glm::qualifier Q>
[[nodiscard]] glm::vec<L, T, Q> MoveTowards(const glm::vec<L, T, Q> &current, const glm::vec<L, T, Q> &target,
                                            T maxStep) noexcept
{
    const glm::vec<L, T, Q> gap = target - current;
    const T distance = std::sqrt(Detail::Dot(gap, gap));
    if (distance <= maxStep || distance == T{0})
    {
        return target;
    }
    return current + gap * (maxStep / distance);
}

} // namespace Assisi::Math
