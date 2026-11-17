/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/WhiteBalance.hpp>

#include <Assisi/Math/Float.hpp>

#include <algorithm>

namespace Assisi::Render
{
namespace
{

// glm takes columns; these read as rows, as the standards print them.
glm::mat3 FromRows(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c)
{
    return glm::transpose(glm::mat3(a, b, c));
}

// Linear sRGB to CIE XYZ, D65.
const glm::mat3 kRgbToXyz = FromRows(glm::vec3(0.4124564f, 0.3575761f, 0.1804375f),
                                     glm::vec3(0.2126729f, 0.7151522f, 0.0721750f),
                                     glm::vec3(0.0193339f, 0.1191920f, 0.9503041f));

// CIE XYZ to the cone responses the Bradford method scales.
const glm::mat3 kXyzToCones = FromRows(glm::vec3(0.8951f, 0.2664f, -0.1614f), glm::vec3(-0.7502f, 1.7135f, 0.0367f),
                                       glm::vec3(0.0389f, -0.0685f, 1.0296f));

// The temperatures, in kelvin, where the pieces of BlackBody's fit meet.
constexpr float kFitWarm = 2222.0f;
constexpr float kFitMiddle = 4000.0f;

// How far, in CIE 1960 uv, a tint of one moves the white across the black body
// line: about what a strongly green fluorescent tube is off it.
constexpr float kTintUv = 0.02f;

// The step, in kelvin, the line's direction is measured over.
constexpr float kDirectionStep = 10.0f;

// Where light of @p kelvin from a black body lies in CIE 1931 xy: x a cubic in
// 1000 / kelvin, y a cubic in x, each in pieces, fit to the exact line from
// 1667 K to 25000 K.
glm::vec2 BlackBody(float kelvin)
{
    const float t = 1.0e3f / kelvin;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float x = kelvin <= kFitMiddle ? -0.2661239f * t3 - 0.2343589f * t2 + 0.8776956f * t + 0.179910f
                                         : -3.0258469f * t3 + 2.1070379f * t2 + 0.2226347f * t + 0.240390f;
    const float x2 = x * x;
    const float x3 = x2 * x;
    float y = 0.0f;
    if (kelvin <= kFitWarm)
    {
        y = -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
    }
    else if (kelvin <= kFitMiddle)
    {
        y = -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
    }
    else
    {
        y = 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    }
    return glm::vec2(x, y);
}

glm::vec2 XyToUv(const glm::vec2 &xy)
{
    const float d = -2.0f * xy.x + 12.0f * xy.y + 3.0f;
    return glm::vec2(4.0f * xy.x, 6.0f * xy.y) / d;
}

glm::vec2 UvToXy(const glm::vec2 &uv)
{
    const float d = 2.0f * uv.x - 8.0f * uv.y + 4.0f;
    return glm::vec2(3.0f * uv.x, 2.0f * uv.y) / d;
}

glm::vec3 XyToXyz(const glm::vec2 &xy)
{
    return glm::vec3(xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y);
}

float SafeKelvin(float kelvin)
{
    return Math::IsFinite(kelvin) ? std::clamp(kelvin, kMinWhiteBalanceKelvin, kMaxWhiteBalanceKelvin)
                                  : kNeutralWhiteBalanceKelvin;
}

float SafeTint(float tint)
{
    return Math::IsFinite(tint) ? std::clamp(tint, kMinWhiteBalanceTint, kMaxWhiteBalanceTint) : 0.0f;
}

} // namespace

glm::vec2 WhiteBalanceChromaticity(float kelvin, float tint)
{
    const float safeKelvin = SafeKelvin(kelvin);
    const glm::vec2 uv = XyToUv(BlackBody(safeKelvin));
    // Square to the line, on the side of larger v, which is toward green.
    const glm::vec2 along = XyToUv(BlackBody(safeKelvin + kDirectionStep)) - XyToUv(BlackBody(safeKelvin));
    glm::vec2 across = glm::normalize(glm::vec2(-along.y, along.x));
    if (across.y < 0.0f)
    {
        across = -across;
    }
    return UvToXy(uv + across * (SafeTint(tint) * kTintUv));
}

glm::mat3 WhiteBalanceMatrix(float kelvin, float tint)
{
    // Measured against the black body at the neutral temperature rather than
    // against D65, which lies a little off the line: the neutral setting is then
    // exactly no change.
    const glm::vec3 source = kXyzToCones * XyToXyz(WhiteBalanceChromaticity(kelvin, tint));
    const glm::vec3 target = kXyzToCones * XyToXyz(WhiteBalanceChromaticity(kNeutralWhiteBalanceKelvin, 0.0f));
    const glm::mat3 scale(glm::vec3(target.x / source.x, 0.0f, 0.0f), glm::vec3(0.0f, target.y / source.y, 0.0f),
                          glm::vec3(0.0f, 0.0f, target.z / source.z));
    return glm::inverse(kRgbToXyz) * glm::inverse(kXyzToCones) * scale * kXyzToCones * kRgbToXyz;
}

glm::vec3 WhiteBalanceWhite(float kelvin, float tint)
{
    return glm::inverse(kRgbToXyz) * XyToXyz(WhiteBalanceChromaticity(kelvin, tint));
}

} // namespace Assisi::Render
