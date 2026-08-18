/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Tonemap.hpp>

#include <cmath>

using namespace Assisi::Render;

namespace
{
// The AgX curve as tonemap.frag applies it, in C++ so its shape can be
// asserted — the shader is what runs, and the two are only as agreed as the
// constants below. Every literal here appears once in tonemap.frag and nowhere
// else; if the look changes without these tests changing, that is where to look.
constexpr float kMinEv = -12.47393f;
constexpr float kMaxEv = 4.026069f;

// Column-major, matching the GLSL mat3 constructor argument for argument, so the
// rows are the mixing weights: each is near enough to summing to 1 that a
// neutral grey passes through unchanged.
const glm::mat3 kInset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f,
                       0.878468636469772f, 0.0784336f, 0.0792237451477643f, 0.0791661274605434f,
                       0.879142973793104f);

const glm::mat3 kOutset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f,
                        1.15190312990417f, -0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f,
                        1.15107367264116f);

float Sigmoid(float x)
{
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

glm::vec3 Sigmoid(glm::vec3 v)
{
    return {Sigmoid(v.x), Sigmoid(v.y), Sigmoid(v.z)};
}

/// The grade: an ASC CDL power and a saturation about the pixel's luma.
glm::vec3 Grade(glm::vec3 v, float contrast, float saturation)
{
    v = glm::pow(glm::max(v, glm::vec3(0.0f)), glm::vec3(contrast));
    const float luma = glm::dot(v, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return glm::max(glm::vec3(luma) + saturation * (v - glm::vec3(luma)), glm::vec3(0.0f));
}

/// Radiance in, display-encoded colour out. The grade sits between the sigmoid
/// and the outset matrix, which is where AgX intends a look to go: the outset
/// restores saturation *after* the look has had its say.
glm::vec3 AgxDisplay(glm::vec3 radiance, const TonemapSettings &settings)
{
    const TonemapConstants pc = MakeTonemapConstants(settings, /*copyThrough=*/ false);

    glm::vec3 v = radiance * pc.exposureScale;
    v = kInset * v;
    v = glm::clamp(glm::log2(v), glm::vec3(kMinEv), glm::vec3(kMaxEv));
    v = (v - kMinEv) / (kMaxEv - kMinEv);
    v = Sigmoid(v);
    v = Grade(v, pc.contrast, pc.saturation);
    return glm::clamp(kOutset * v, glm::vec3(0.0f), glm::vec3(1.0f));
}

/// The curve with no grade on top, for the tests that are about the curve.
constexpr TonemapSettings kUngraded{
    .op = TonemapOperator::AgX, .exposureStops = 0.0f, .contrast = 1.0f, .saturation = 1.0f};

/// Perceptual mid-grey, the value every tone curve is judged at first.
constexpr float kMidGrey = 0.18f;

float MinChannel(glm::vec3 v)
{
    return std::min(v.x, std::min(v.y, v.z));
}
} // namespace

TEST_CASE("Exposure is a power of two applied before the curve")
{
    CHECK(ExposureScale(0.0f) == doctest::Approx(1.0f));
    CHECK(ExposureScale(1.0f) == doctest::Approx(2.0f));
    CHECK(ExposureScale(-1.0f) == doctest::Approx(0.5f));
    CHECK(ExposureScale(3.0f) == doctest::Approx(8.0f));

    // A stop of exposure and twice the radiance are the same picture — that
    // equivalence is what makes the slider mean "stops" rather than "a number".
    const TonemapSettings oneStopUp{
        .op = TonemapOperator::AgX, .exposureStops = 1.0f, .contrast = 1.0f, .saturation = 1.0f};
    const glm::vec3 exposed = AgxDisplay(glm::vec3(kMidGrey), oneStopUp);
    const glm::vec3 doubled = AgxDisplay(glm::vec3(kMidGrey * 2.0f), kUngraded);
    CHECK(exposed.x == doctest::Approx(doubled.x).epsilon(1e-4));
    CHECK(exposed.y == doctest::Approx(doubled.y).epsilon(1e-4));
    CHECK(exposed.z == doctest::Approx(doubled.z).epsilon(1e-4));
}

TEST_CASE("Out-of-range and non-finite settings cannot reach the shader")
{
    // options.json is hand-editable and a NaN in any lane takes the frame to
    // black, which reads as a driver fault rather than as a typo in a config.
    CHECK(ExposureScale(1000.0f) == doctest::Approx(std::exp2(kMaxExposureStops)));
    CHECK(ExposureScale(-1000.0f) == doctest::Approx(std::exp2(kMinExposureStops)));
    // NaN and infinity are junk rather than an extreme, so both fall back to the
    // default rather than to the end of the slider: a config that says nothing
    // usable should look like one that says nothing.
    CHECK(ExposureScale(std::nanf("")) == doctest::Approx(1.0f));
    CHECK(ExposureScale(INFINITY) == doctest::Approx(1.0f));

    TonemapSettings hostile;
    hostile.op = static_cast<TonemapOperator>(99u);
    hostile.exposureStops = std::nanf("");
    hostile.contrast = -5.0f;
    hostile.saturation = INFINITY;

    const TonemapSettings safe = Sanitized(hostile);
    CHECK(static_cast<std::uint32_t>(safe.op) < kTonemapOperatorCount);
    CHECK(safe.exposureStops == doctest::Approx(0.0f));
    CHECK(safe.contrast >= kMinContrast);
    CHECK(safe.contrast <= kMaxContrast);
    CHECK(safe.saturation >= kMinSaturation);
    CHECK(safe.saturation <= kMaxSaturation);
}

TEST_CASE("A copy-through step neither exposes nor grades")
{
    // The Blit runs this same shader over an image the tone map has already
    // mapped, and the debug views hand it channel values rather than radiance.
    // Applying exposure or the grade again would restate both.
    TonemapSettings loud;
    loud.exposureStops = 4.0f;
    loud.contrast = 2.0f;
    loud.saturation = 0.0f;

    const TonemapConstants copy = MakeTonemapConstants(loud, /*copyThrough=*/ true);
    CHECK(copy.passthrough != 0u);
    CHECK(copy.exposureScale == doctest::Approx(1.0f));
    CHECK(copy.contrast == doctest::Approx(1.0f));
    CHECK(copy.saturation == doctest::Approx(1.0f));

    const TonemapConstants mapped = MakeTonemapConstants(loud, /*copyThrough=*/ false);
    CHECK(mapped.passthrough == 0u);
    CHECK(mapped.exposureScale == doctest::Approx(16.0f));
    CHECK(mapped.contrast == doctest::Approx(2.0f));
    CHECK(mapped.saturation == doctest::Approx(0.0f));
}

TEST_CASE("The curve is monotonic, and black stays black")
{
    // log2(0) is -inf, which the EV clamp has to absorb — without it the sigmoid
    // is evaluated on a NaN and the darkest pixel in the frame is the loudest.
    const glm::vec3 black = AgxDisplay(glm::vec3(0.0f), kUngraded);
    CHECK(black.x == doctest::Approx(0.0f));
    CHECK(black.y == doctest::Approx(0.0f));
    CHECK(black.z == doctest::Approx(0.0f));

    float previous = -1.0f;
    for (float radiance = 0.0f; radiance < 64.0f; radiance += 0.05f)
    {
        CAPTURE(radiance);
        const float mapped = AgxDisplay(glm::vec3(radiance), kUngraded).x;
        CHECK(mapped >= previous);
        CHECK(mapped <= 1.0f);
        previous = mapped;
    }
}

TEST_CASE("Highlights keep gradation well past the point a per-channel fit gives up")
{
    // The ACES fit reaches display white at ~7.2x linear and everything above it
    // is flat. Radiance at 8x and at 16x must still be two different colours, or
    // there is no reason to have paid for an HDR target.
    const float at8 = AgxDisplay(glm::vec3(8.0f), kUngraded).x;
    const float at16 = AgxDisplay(glm::vec3(16.0f), kUngraded).x;
    CHECK(at8 < 1.0f);
    CHECK(at16 > at8);

    // And mid-grey lands near the middle of the display range rather than being
    // crushed or blown by the curve.
    const float grey = AgxDisplay(glm::vec3(kMidGrey), kUngraded).x;
    CHECK(grey == doctest::Approx(0.496f).epsilon(0.02));
}

TEST_CASE("Bright saturated colour bleaches toward white without rotating hue")
{
    // This is the property AgX was chosen for. A per-channel curve leaves a pure
    // blue pure blue at any intensity — the channels that started at zero stay
    // there — so a bright light never reads as a bright light, and a nearly
    // saturated one rotates toward the corner of the cube it is closest to.
    float previousFloor = -1.0f;
    for (const float intensity : {0.1f, 1.0f, 10.0f, 100.0f, 1000.0f})
    {
        CAPTURE(intensity);
        const glm::vec3 blue = AgxDisplay(glm::vec3(0.0f, 0.0f, intensity), kUngraded);

        // Purple is red climbing above green. The inset weights the two within
        // 0.1% of each other, which is what holds the hue steady.
        CHECK(blue.x == doctest::Approx(blue.y).epsilon(0.01));
        CHECK(blue.z >= blue.x);

        // Every step brighter moves the whole colour closer to white.
        const float floor = MinChannel(blue);
        CHECK(floor > previousFloor);
        previousFloor = floor;
    }
    // By three orders of magnitude over white it is a white light, not a blue one.
    CHECK(MinChannel(AgxDisplay(glm::vec3(0.0f, 0.0f, 1000.0f), kUngraded)) > 0.9f);

    // Red skews toward orange the same way, by green climbing above blue.
    for (const float intensity : {1.0f, 10.0f, 100.0f})
    {
        CAPTURE(intensity);
        const glm::vec3 red = AgxDisplay(glm::vec3(intensity, 0.0f, 0.0f), kUngraded);
        CHECK(red.y == doctest::Approx(red.z).epsilon(0.01));
        CHECK(red.x >= red.y);
    }
}

TEST_CASE("The grade is what puts contrast back")
{
    const glm::vec3 grey(kMidGrey);

    // Identity: the knobs at 1 leave the curve exactly as it came.
    const TonemapSettings identity{
        .op = TonemapOperator::AgX, .exposureStops = 0.0f, .contrast = 1.0f, .saturation = 1.0f};
    CHECK(AgxDisplay(grey, identity).x == doctest::Approx(AgxDisplay(grey, kUngraded).x));

    // Contrast above 1 deepens the shadows — the whole reason the default is not 1.
    const TonemapSettings punchy{.op = TonemapOperator::AgX,
                                 .exposureStops = 0.0f,
                                 .contrast = kPunchyContrast,
                                 .saturation = kPunchySaturation};
    CHECK(AgxDisplay(grey, punchy).x < AgxDisplay(grey, identity).x);

    // Saturation at 0 is greyscale, whatever went in.
    const TonemapSettings grey0{
        .op = TonemapOperator::AgX, .exposureStops = 0.0f, .contrast = 1.0f, .saturation = 0.0f};
    const glm::vec3 drained = AgxDisplay(glm::vec3(0.6f, 0.2f, 0.05f), grey0);
    CHECK(drained.x == doctest::Approx(drained.y).epsilon(0.02));
    CHECK(drained.y == doctest::Approx(drained.z).epsilon(0.02));

    // And saturation above 1 pushes the channels apart rather than together.
    const TonemapSettings rich{
        .op = TonemapOperator::AgX, .exposureStops = 0.0f, .contrast = 1.0f, .saturation = 1.8f};
    const glm::vec3 colour(0.6f, 0.2f, 0.05f);
    const glm::vec3 plain = AgxDisplay(colour, identity);
    const glm::vec3 loud = AgxDisplay(colour, rich);
    CHECK((loud.x - loud.z) > (plain.x - plain.z));
}

TEST_CASE("The defaults are the punchy look, on AgX")
{
    // A neutral AgX reads flat, which is the reaction that gets it rejected. The
    // pass has to look better than the Reinhard it replaces on first launch.
    const TonemapSettings defaults;
    CHECK(defaults.op == TonemapOperator::AgX);
    CHECK(defaults.exposureStops == doctest::Approx(0.0f));
    CHECK(defaults.contrast > 1.0f);
    CHECK(defaults.saturation > 1.0f);
}
