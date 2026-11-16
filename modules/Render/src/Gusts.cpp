/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Gusts.hpp>

#include <Assisi/Render/Water.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace Assisi::Render
{
namespace
{

/// Below this wind the pattern changes at this wind's pace instead, so a
/// lifetime stays finite; there are no gusts to see in air that still.
constexpr float kMinGustDrift = 0.5f;

/// The finer octave weighs half the coarser, so a gust is a blob with a ragged
/// edge.
constexpr float kGustDetailWeight = 0.5f;
/// Mixed into the finer octave's hash, so it is not the coarser one again: the
/// golden ratio's bits.
constexpr uint32_t kGustDetailKey = 0x9E3779B9u;
/// Brings the strongest gust in about a hundred to 1. Ten minutes of wind
/// carries about a hundred gusts past a point, and the gust factor is the
/// strongest of them. The two octaves' 99th percentile of |n| measures 0.49.
constexpr float kGustNoiseScale = 2.0f;

/// Perlin's improved gradients: toward the middles of a cube's twelve edges.
constexpr std::array<glm::vec3, 12> kGustGradients{
        glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f),
        glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(-1.0f, 0.0f, 1.0f),
        glm::vec3(1.0f, 0.0f, -1.0f), glm::vec3(-1.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 1.0f),
        glm::vec3(0.0f, -1.0f, 1.0f), glm::vec3(0.0f, 1.0f, -1.0f), glm::vec3(0.0f, -1.0f, -1.0f)};

/// The corners of a lattice cell: x, y and z from the index's low three bits.
constexpr uint32_t kCellCorners = 8u;

uint32_t WrapLattice(int32_t index, int32_t period)
{
    const int32_t wrapped = index % period;
    return static_cast<uint32_t>(wrapped < 0 ? wrapped + period : wrapped);
}

double WrapCell(double value)
{
    const double period = static_cast<double>(kGustPeriod);
    return value - period * std::floor(value / period);
}

/// Perlin's quintic, whose first and second derivatives are zero at 0 and 1.
float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/// One octave of gradient noise at @p p, repeating every @p period lattice
/// steps each way, between about -1 and 1.
float GradientNoise(glm::vec3 p, int32_t period, uint32_t key)
{
    const glm::vec3 floored = glm::floor(p);
    const glm::ivec3 base(floored);
    const glm::vec3 inside = p - floored;
    const glm::vec3 eased(Fade(inside.x), Fade(inside.y), Fade(inside.z));
    float sum = 0.0f;
    for (uint32_t corner = 0u; corner < kCellCorners; ++corner)
    {
        const glm::ivec3 offset(static_cast<int32_t>(corner & 1u), static_cast<int32_t>((corner >> 1u) & 1u),
                                static_cast<int32_t>((corner >> 2u) & 1u));
        const glm::ivec3 lattice = base + offset;
        const uint32_t hash = WaterHash(WrapLattice(lattice.x, period) ^
                                        WaterHash(WrapLattice(lattice.y, period) ^
                                                  WaterHash(WrapLattice(lattice.z, period) ^ key)));
        const glm::vec3 gradient = kGustGradients[hash % kGustGradients.size()];
        const glm::vec3 weights = glm::mix(glm::vec3(1.0f) - eased, eased, glm::vec3(offset));
        sum += weights.x * weights.y * weights.z * glm::dot(gradient, inside - glm::vec3(offset));
    }
    return sum;
}

float SpeedWithGust(float speed, float gustFactor, float noise)
{
    return speed * std::max(1.0f + (gustFactor - 1.0f) * noise, 0.0f);
}

} // namespace

glm::vec3 GustCell(const Gusts &gusts, glm::dvec2 position, double seconds)
{
    const glm::dvec2 toward(gusts.toward);
    const glm::dvec2 across(-toward.y, toward.x);
    const double speed = static_cast<double>(gusts.speed);
    const double lifetime = static_cast<double>(kGustLengthAlong) / std::max(speed, static_cast<double>(kMinGustDrift));
    const double along = (glm::dot(position, toward) - speed * seconds) / static_cast<double>(kGustLengthAlong);
    const double side = glm::dot(position, across) / static_cast<double>(kGustLengthAcross);
    return glm::vec3(glm::dvec3(WrapCell(along), WrapCell(side), WrapCell(seconds / lifetime)));
}

float GustNoise(glm::vec3 cell)
{
    const int32_t period = static_cast<int32_t>(kGustPeriod);
    const float coarse = GradientNoise(cell, period, 0u);
    const float fine = GradientNoise(cell * 2.0f, period * 2, kGustDetailKey);
    return std::clamp(kGustNoiseScale * (coarse + kGustDetailWeight * fine) / (1.0f + kGustDetailWeight), -1.0f, 1.0f);
}

float GustSpeed(const Gusts &gusts, glm::dvec2 position, double seconds)
{
    return SpeedWithGust(gusts.speed, gusts.gustFactor, GustNoise(GustCell(gusts, position, seconds)));
}

GustConstants MakeGustConstants(const Gusts &gusts, glm::dvec3 camera, double seconds)
{
    return GustConstants{.cell = glm::vec4(GustCell(gusts, glm::dvec2(camera.x, camera.z), seconds), gusts.gustFactor),
                         .wind = glm::vec4(gusts.toward, gusts.speed, 0.0f)};
}

float GustSpeedNear(const GustConstants &constants, glm::vec2 fromCamera)
{
    const glm::vec2 toward(constants.wind.x, constants.wind.y);
    const glm::vec2 across(-toward.y, toward.x);
    const glm::vec3 cell = glm::vec3(constants.cell) + glm::vec3(glm::dot(fromCamera, toward) / kGustLengthAlong,
                                                                 glm::dot(fromCamera, across) / kGustLengthAcross, 0.0f);
    return SpeedWithGust(constants.wind.z, constants.cell.w, GustNoise(cell));
}

} // namespace Assisi::Render
