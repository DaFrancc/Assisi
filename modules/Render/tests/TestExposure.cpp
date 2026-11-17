/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/Exposure.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace Assisi::Render;

namespace
{

using Histogram = std::array<uint32_t, kExposureHistogramBins>;

Histogram Empty()
{
    Histogram bins;
    bins.fill(0u);
    return bins;
}

} // namespace

TEST_CASE("A pixel lands in the bin of its brightness, and black in the bin metering skips")
{
    CHECK(ExposureHistogramBin(0.0f) == 0u);
    CHECK(ExposureHistogramBin(-1.0f) == 0u);
    CHECK(ExposureHistogramBin(std::numeric_limits<float>::quiet_NaN()) == 0u);
    CHECK(ExposureHistogramBin(std::exp2(kExposureHistogramMinLog2) * 0.9f) == 0u);
    CHECK(ExposureHistogramBin(std::exp2(kExposureHistogramMinLog2)) == 1u);
    CHECK(ExposureHistogramBin(std::exp2(kExposureHistogramMaxLog2)) == kExposureHistogramBins - 1u);
    CHECK(ExposureHistogramBin(std::numeric_limits<float>::infinity()) == kExposureHistogramBins - 1u);

    // Each bin's middle falls in that bin, so the two functions agree.
    for (uint32_t bin = 1; bin < kExposureHistogramBins; ++bin)
    {
        CAPTURE(bin);
        CHECK(ExposureHistogramBin(std::exp2(ExposureHistogramBinLog2(bin))) == bin);
    }
}

TEST_CASE("A frame of one brightness meters at that brightness")
{
    Histogram bins = Empty();
    bins[40] = 1000u;
    CHECK(MeteredLog2Luminance(bins) == doctest::Approx(ExposureHistogramBinLog2(40)));

    // Black pixels do not count, however many there are.
    bins[0] = 100000u;
    CHECK(MeteredLog2Luminance(bins) == doctest::Approx(ExposureHistogramBinLog2(40)));
}

TEST_CASE("Metering leaves out the darker half and the brightest few")
{
    // Half the frame in shadow, half in light: the light decides.
    Histogram halves = Empty();
    halves[20] = 100u;
    halves[80] = 100u;
    CHECK(MeteredLog2Luminance(halves) == doctest::Approx(ExposureHistogramBinLog2(80)));

    // The sun and its glare, a twentieth of the frame, change nothing.
    Histogram sun = Empty();
    sun[40] = 96u;
    sun[120] = 4u;
    CHECK(MeteredLog2Luminance(sun) == doctest::Approx(ExposureHistogramBinLog2(40)));

    // A bin straddling a share counts only its part inside: here a third of the
    // kept pixels are bright.
    Histogram mixed = Empty();
    mixed[30] = 80u;
    mixed[60] = 20u;
    // Kept: from 50 to 95, so 30 dark ones and 15 bright ones.
    const float expected = (30.0f * ExposureHistogramBinLog2(30) + 15.0f * ExposureHistogramBinLog2(60)) / 45.0f;
    CHECK(MeteredLog2Luminance(mixed) == doctest::Approx(expected));
}

TEST_CASE("An empty frame meters at the target, so no exposure")
{
    CHECK(MeteredLog2Luminance(Empty()) == doctest::Approx(std::log2(kExposureTargetLuminance)));
}

TEST_CASE("The aim draws the metered brightness at the target, held inside the range")
{
    const ExposureSettings settings{.minStops = -4.0f, .maxStops = 6.0f};
    CHECK(TargetExposureStops(std::log2(kExposureTargetLuminance), settings) == doctest::Approx(0.0f));
    // A frame a stop brighter is drawn a stop darker.
    CHECK(TargetExposureStops(std::log2(kExposureTargetLuminance) + 1.0f, settings) == doctest::Approx(-1.0f));

    ExposureSettings brighter = settings;
    brighter.compensationStops = 0.5f;
    CHECK(TargetExposureStops(std::log2(kExposureTargetLuminance), brighter) == doctest::Approx(0.5f));

    // A dark cave asks for twelve stops and gets the most the range allows.
    CHECK(TargetExposureStops(std::log2(kExposureTargetLuminance) - 12.0f, settings) == doctest::Approx(6.0f));
    CHECK(TargetExposureStops(std::log2(kExposureTargetLuminance) + 12.0f, settings) == doctest::Approx(-4.0f));
}

TEST_CASE("The exposure closes on its aim over time, never past it")
{
    CHECK(AdaptExposureStops(0.0f, 4.0f, 1.5f, 0.0f) == doctest::Approx(0.0f));
    // ln 2 / rate seconds covers half the way.
    CHECK(AdaptExposureStops(0.0f, 4.0f, 1.5f, std::log(2.0f) / 1.5f) == doctest::Approx(2.0f));
    CHECK(AdaptExposureStops(0.0f, 4.0f, 1.5f, 1000.0f) == doctest::Approx(4.0f));
    CHECK(AdaptExposureStops(0.0f, 4.0f, 0.0f, 10.0f) == doctest::Approx(0.0f));
    CHECK(AdaptExposureStops(0.0f, 4.0f, 1.5f, -1.0f) == doctest::Approx(0.0f));
    CHECK(AdaptExposureStops(3.0f, -1.0f, 1.5f, 0.5f) < 3.0f);
    CHECK(AdaptExposureStops(3.0f, -1.0f, 1.5f, 0.5f) > -1.0f);

    // Two half frames land where one whole frame does, so the frame rate does
    // not change how fast the eye adjusts.
    const float halves = AdaptExposureStops(AdaptExposureStops(0.0f, 4.0f, 1.5f, 0.01f), 4.0f, 1.5f, 0.01f);
    CHECK(halves == doctest::Approx(AdaptExposureStops(0.0f, 4.0f, 1.5f, 0.02f)));
}

TEST_CASE("Hand-typed exposure settings are held in range")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const ExposureSettings broken = Sanitized(ExposureSettings{
        .manualStops = nan, .automatic = 5.0f, .minStops = 3.0f, .maxStops = -2.0f, .adaptationRate = nan});
    CHECK(broken.manualStops == 0.0f);
    CHECK(broken.automatic == 1.0f);
    CHECK(broken.minStops == 3.0f);
    CHECK(broken.maxStops == 3.0f);
    CHECK(broken.adaptationRate == kDefaultExposureAdaptationRate);
    CHECK(Sanitized(broken) == broken);
}
