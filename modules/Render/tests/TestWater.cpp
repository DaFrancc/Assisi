/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/Water.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using namespace Assisi::Render;

namespace
{

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// A fresh breeze over open water, with a long swell beside it.
WaterWaves OpenSea()
{
    return WaterWaves{.wind = WindSea{.windSpeed = 12.0f, .fetch = kOceanFetch, .bearingDegrees = 90.0f, .spread = 0.2f},
                      .swell = Swell{.height = 1.5f, .period = 11.0f, .bearingDegrees = 60.0f},
                      .choppiness = 1.0f,
                      .seed = 7u};
}

/// A pond on a light breeze.
WaterWaves Pond()
{
    return WaterWaves{.wind = WindSea{.windSpeed = 4.0f, .fetch = 100.0f, .bearingDegrees = 0.0f, .spread = 0.3f},
                      .swell = Swell{},
                      .choppiness = 0.5f,
                      .seed = 3u};
}

/// ∫ S(ω) dω by the midpoint rule, over every frequency that holds energy:
/// independent of SignificantWaveHeight's own integral.
double SpectrumVariance(const WaveSpectrum &spectrum)
{
    constexpr int32_t kSteps = 20000;
    const double high = static_cast<double>(spectrum.peakFrequency) * 20.0;
    const double step = high / kSteps;
    double sum = 0.0;
    for (int32_t i = 0; i < kSteps; ++i)
    {
        const double omega = (static_cast<double>(i) + 0.5) * step;
        sum += static_cast<double>(JonswapSpectrum(static_cast<float>(omega), spectrum.peakFrequency, spectrum.alpha)) *
               step;
    }
    return sum;
}

/// The significant wave height a sailor would report: four standard
/// deviations of the surface's height.
double SignificantHeight(float windSpeed, float fetch)
{
    return 4.0 * std::sqrt(SpectrumVariance(SpectrumOf(WindSea{.windSpeed = windSpeed, .fetch = fetch})));
}

/// The variance every texel of every cascade adds to the surface's height:
/// each wave and its mirror together add twice its amplitude squared.
double CascadeVariance(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade)
{
    double sum = 0.0;
    for (uint32_t z = 0; z < kWaterFftSize; ++z)
    {
        for (uint32_t x = 0; x < kWaterFftSize; ++x)
        {
            const glm::ivec2 frequency = FrequencyOfTexel(glm::uvec2(x, z), kWaterFftSize);
            const float amplitude = WaveAmplitude(waves, cascades, cascade,
                                                  WavenumberOf(frequency, cascades.tileLength[cascade]));
            sum += 2.0 * static_cast<double>(amplitude) * static_cast<double>(amplitude);
        }
    }
    return sum;
}

double Mean(const std::vector<WaterTexel> &texels, float WaterTexel::*field)
{
    double sum = 0.0;
    for (const WaterTexel &texel : texels)
    {
        sum += static_cast<double>(texel.*field);
    }
    return sum / static_cast<double>(texels.size());
}

double Variance(const std::vector<WaterTexel> &texels, float WaterTexel::*field)
{
    const double mean = Mean(texels, field);
    double sum = 0.0;
    for (const WaterTexel &texel : texels)
    {
        const double d = static_cast<double>(texel.*field) - mean;
        sum += d * d;
    }
    return sum / static_cast<double>(texels.size());
}

} // namespace

TEST_CASE("The water hash gives the same bits on every machine")
{
    // The PCG hash's published outputs: the GPU's transcription must give these.
    CHECK(WaterHash(0u) == 129708002u);
    CHECK(WaterHash(1u) == 2831084092u);
    CHECK(WaterHash(12345u) == 4099845390u);
    CHECK(WaterHash(0xFFFFFFFFu) == 3861530882u);
}

TEST_CASE("The random draws behind the waves are standard normal, and the same for the same seed")
{
    double sum = 0.0;
    double sumSquares = 0.0;
    uint32_t count = 0;
    constexpr int32_t kHalf = 128;
    for (int32_t z = -kHalf; z < kHalf; ++z)
    {
        for (int32_t x = -kHalf; x < kHalf; ++x)
        {
            const glm::vec2 draw = GaussianPair(11u, 1u, glm::ivec2(x, z));
            sum += static_cast<double>(draw.x) + static_cast<double>(draw.y);
            sumSquares += static_cast<double>(draw.x * draw.x) + static_cast<double>(draw.y * draw.y);
            count += 2u;
        }
    }
    const double mean = sum / count;
    CHECK(std::abs(mean) < 0.01);
    CHECK(sumSquares / count - mean * mean == doctest::Approx(1.0).epsilon(0.02));

    const glm::vec2 once = GaussianPair(11u, 1u, glm::ivec2(-5, 9));
    const glm::vec2 again = GaussianPair(11u, 1u, glm::ivec2(-5, 9));
    CHECK(once.x == again.x);
    CHECK(once.y == again.y);
    CHECK(GaussianPair(12u, 1u, glm::ivec2(-5, 9)).x != once.x);
    CHECK(GaussianPair(11u, 2u, glm::ivec2(-5, 9)).x != once.x);
    CHECK(GaussianPair(11u, 1u, glm::ivec2(5, -9)).x != once.x);
}

TEST_CASE("The JONSWAP spectrum peaks at its peak frequency, and no wind raises no waves")
{
    const WaveSpectrum sea = SpectrumOf(WindSea{.windSpeed = 12.0f, .fetch = kOceanFetch});
    CHECK(sea.peakFrequency == JonswapPeakFrequency(12.0f, kOceanFetch));
    float bestOmega = 0.0f;
    float best = 0.0f;
    for (int32_t i = 1; i < 4000; ++i)
    {
        const float omega = sea.peakFrequency * 0.001f * static_cast<float>(i);
        const float value = JonswapSpectrum(omega, sea.peakFrequency, sea.alpha);
        if (value > best)
        {
            best = value;
            bestOmega = omega;
        }
    }
    CHECK(bestOmega == doctest::Approx(sea.peakFrequency).epsilon(0.01));

    const WaveSpectrum calm = SpectrumOf(WindSea{.windSpeed = 0.0f});
    CHECK(calm.peakFrequency == 0.0f);
    CHECK(SignificantWaveHeight(calm) == 0.0f);
    CHECK(JonswapSpectrum(0.0f, sea.peakFrequency, sea.alpha) == 0.0f);
}

TEST_CASE("Swell has the height and period it was given")
{
    for (const Swell &swell : {Swell{.height = 1.5f, .period = 11.0f}, Swell{.height = 4.0f, .period = 18.0f},
                               Swell{.height = 0.3f, .period = 6.0f}})
    {
        CAPTURE(swell.height);
        const WaveSpectrum spectrum = SpectrumOf(swell);
        CHECK(spectrum.peakFrequency == doctest::Approx(2.0 * std::numbers::pi / static_cast<double>(swell.period)));
        CHECK(4.0 * std::sqrt(SpectrumVariance(spectrum)) == doctest::Approx(swell.height).epsilon(0.01));
        CHECK(SignificantWaveHeight(spectrum) == doctest::Approx(swell.height).epsilon(0.01));
    }
    CHECK(SpectrumOf(Swell{.height = 0.0f}).peakFrequency == 0.0f);
}

TEST_CASE("A longer fetch grows longer, taller waves")
{
    float previousPeak = std::numeric_limits<float>::max();
    double previousHeight = 0.0;
    for (const float fetch : {100.0f, 1000.0f, 10000.0f, 100000.0f})
    {
        CAPTURE(fetch);
        const float peak = JonswapPeakFrequency(10.0f, fetch);
        const double height = SignificantHeight(10.0f, fetch);
        CHECK(peak < previousPeak);
        CHECK(height > previousHeight);
        previousPeak = peak;
        previousHeight = height;
    }
}

TEST_CASE("A pond's waves are smaller and shorter than the open sea's")
{
    // Open sea in a fresh breeze: waves of a couple of metres, peaking near a
    // seven-second period, as measured seas do.
    const double seaHeight = SignificantHeight(12.0f, kOceanFetch);
    const double seaPeriod = kTwoPi / static_cast<double>(JonswapPeakFrequency(12.0f, kOceanFetch));
    CHECK(seaHeight > 1.5);
    CHECK(seaHeight < 4.0);
    CHECK(seaPeriod > 5.0);
    CHECK(seaPeriod < 9.0);

    const double pondHeight = SignificantHeight(4.0f, 100.0f);
    CHECK(pondHeight < seaHeight / 50.0);
    CHECK(JonswapPeakFrequency(4.0f, 100.0f) > JonswapPeakFrequency(12.0f, kOceanFetch) * 5.0f);
}

TEST_CASE("The spread normalization matches the gamma functions it stands for")
{
    for (const double s : {0.0, 0.1, 0.5, 1.0, 2.5, 7.0, 20.0, 60.0})
    {
        CAPTURE(s);
        const double exact = std::tgamma(s + 1.0) / (2.0 * std::sqrt(std::numbers::pi) * std::tgamma(s + 0.5));
        CHECK(static_cast<double>(SpreadNormalization(static_cast<float>(s))) == doctest::Approx(exact).epsilon(1e-4));
    }
}

TEST_CASE("The spread around the wind shares out all of a wave's energy, however narrow")
{
    constexpr int32_t kSteps = 20000;
    for (const float exponent : {0.0f, 0.3f, 1.0f, 2.5f, 7.0f, 20.0f, 60.0f})
    {
        for (const float spread : {0.0f, 0.5f, 1.0f})
        {
            CAPTURE(exponent);
            CAPTURE(spread);
            double sum = 0.0;
            const double step = kTwoPi / kSteps;
            for (int32_t i = 0; i < kSteps; ++i)
            {
                const double theta = -std::numbers::pi + (static_cast<double>(i) + 0.5) * step;
                sum += static_cast<double>(DirectionalSpread(static_cast<float>(theta), exponent, spread)) * step;
            }
            CHECK(sum == doctest::Approx(1.0).epsilon(1e-3));
        }
    }

    // Narrow keeps the energy with the wind; a spread of one shares it evenly.
    CHECK(DirectionalSpread(0.0f, 10.0f, 0.0f) > DirectionalSpread(1.0f, 10.0f, 0.0f));
    CHECK(DirectionalSpread(0.0f, 10.0f, 1.0f) == doctest::Approx(DirectionalSpread(2.5f, 10.0f, 1.0f)));
}

TEST_CASE("Waves near the peak keep closest to the wind, and swell draws them out further")
{
    const WaveSpectrum sea = SpectrumOf(WindSea{.windSpeed = 12.0f, .fetch = kOceanFetch});
    const float peak = sea.peakFrequency;
    const float atPeak = SpreadExponent(peak, sea);
    CHECK(atPeak > SpreadExponent(peak * 0.5f, sea));
    CHECK(atPeak > SpreadExponent(peak * 2.0f, sea));
    WaveSpectrum drawnOut = sea;
    drawnOut.swell = 1.0f;
    CHECK(SpreadExponent(peak, drawnOut) > atPeak);
}

TEST_CASE("Long waves travel faster than short ones")
{
    const float longWave = 2.0f * std::numbers::pi_v<float> / 100.0f;
    const float shortWave = 2.0f * std::numbers::pi_v<float> / 1.0f;
    // A deep-water wave's speed is sqrt(g / k): a hundred-metre wave about
    // twelve metres a second, a one-metre wave about one and a quarter.
    CHECK(DeepWaterFrequency(longWave) / longWave == doctest::Approx(12.49).epsilon(0.01));
    CHECK(DeepWaterFrequency(shortWave) / shortWave == doctest::Approx(1.249).epsilon(0.01));
}

TEST_CASE("Rounded frequencies make the waves repeat exactly after the loop")
{
    const double turn = kTwoPi / kWaterLoopSeconds;
    for (const float omega : {0.2f, 0.95f, 3.7f, 41.0f, 115.0f})
    {
        CAPTURE(omega);
        const double looped = static_cast<double>(LoopedFrequency(omega));
        CHECK(looped <= static_cast<double>(omega));
        CHECK(looped > static_cast<double>(omega) - turn * 1.001);
        const double turns = looped / turn;
        CHECK(std::abs(turns - std::round(turns)) < 1e-2);
    }

    CHECK(WaterLoopTime(5.25) == doctest::Approx(5.25));
    CHECK(WaterLoopTime(5.25 + kWaterLoopSeconds * 37.0) == doctest::Approx(5.25));
    CHECK(WaterLoopTime(-1.0) == doctest::Approx(kWaterLoopSeconds - 1.0));

    const WaterWaves waves = OpenSea();
    const WaterCascades cascades = LayoutCascades(waves);
    const glm::ivec2 frequency(7, -3);
    const glm::vec2 k = WavenumberOf(frequency, cascades.tileLength[0]);
    const glm::vec4 initial = InitialWave(waves, cascades, 0u, frequency);
    const glm::vec2 start = WaveAt(initial, k, 0.0f);
    const glm::vec2 later = WaveAt(initial, k, static_cast<float>(WaterLoopTime(kWaterLoopSeconds * 3.0)));
    CHECK(later.x == doctest::Approx(start.x));
    CHECK(later.y == doctest::Approx(start.y));
}

TEST_CASE("The cascades' bands meet without overlapping, inside what each grid can hold")
{
    for (const WaterWaves &waves : {OpenSea(), Pond()})
    {
        const WaterCascades cascades = LayoutCascades(waves);
        CHECK(cascades.minWavenumber[0] == 0.0f);
        for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
        {
            CAPTURE(c);
            const float fundamental = 2.0f * std::numbers::pi_v<float> / cascades.tileLength[c];
            const float nyquist = fundamental * static_cast<float>(kWaterFftSize / 2u);
            CHECK(cascades.minWavenumber[c] < cascades.maxWavenumber[c]);
            CHECK(cascades.maxWavenumber[c] <= nyquist);
            if (c + 1u < kWaterCascadeCount)
            {
                CHECK(cascades.maxWavenumber[c] == cascades.minWavenumber[c + 1u]);
                CHECK(cascades.tileLength[c] / cascades.tileLength[c + 1u] == doctest::Approx(kWaterCascadeRatio));
            }
            else
            {
                CHECK(cascades.maxWavenumber[c] == doctest::Approx(nyquist));
            }
        }
    }
}

TEST_CASE("The largest tile holds a few of the longest waves, within its limits")
{
    const WaterWaves sea = OpenSea();
    const WaterCascades seaCascades = LayoutCascades(sea);
    // The swell peaks longest here: deep-water waves of period T are g·T²/2π long.
    const float swellWavelength =
        kWaterGravity * sea.swell.period * sea.swell.period / (2.0f * std::numbers::pi_v<float>);
    CHECK(seaCascades.tileLength[0] == doctest::Approx(kWaterTileWavelengths * swellWavelength));

    // Without the swell the wind's own waves size it.
    WaterWaves calm = sea;
    calm.swell.height = 0.0f;
    const float windPeak = JonswapPeakFrequency(calm.wind.windSpeed, calm.wind.fetch);
    const float windWavelength = 2.0f * std::numbers::pi_v<float> *kWaterGravity / (windPeak * windPeak);
    CHECK(LayoutCascades(calm).tileLength[0] == doctest::Approx(kWaterTileWavelengths * windWavelength));

    CHECK(LayoutCascades(Pond()).tileLength[0] == kWaterMinTileLength);
}

TEST_CASE("Every wavenumber the spectrum holds belongs to exactly one cascade")
{
    for (const WaterWaves &waves : {OpenSea(), Pond()})
    {
        const WaterCascades cascades = LayoutCascades(waves);
        constexpr uint32_t kMagnitudes = 400u;
        const float lowest = 2.0f * std::numbers::pi_v<float> / cascades.tileLength[0];
        const float highest = cascades.maxWavenumber[kWaterCascadeCount - 1u] * 0.999f;
        for (uint32_t i = 0; i < kMagnitudes; ++i)
        {
            const float share = static_cast<float>(i) / static_cast<float>(kMagnitudes - 1u);
            const float magnitude = lowest * std::pow(highest / lowest, share);
            // Along the wind, where every spectrum here holds energy.
            const float bearing = glm::radians(waves.wind.bearingDegrees);
            const glm::vec2 k = magnitude * glm::vec2(std::sin(bearing), -std::cos(bearing));
            uint32_t holders = 0u;
            for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
            {
                holders += WaveAmplitude(waves, cascades, c, k) > 0.0f ? 1u : 0u;
            }
            CAPTURE(magnitude);
            CHECK(holders <= 1u);
            // Far below the peak the spectrum is a denormal the build flushes
            // to zero; only energy that shows has to land somewhere.
            constexpr float kVisibleShare = 1e-9f;
            const WaveSpectrum wind = SpectrumOf(waves.wind);
            const float atPeak = JonswapSpectrum(wind.peakFrequency, wind.peakFrequency, wind.alpha);
            if (JonswapSpectrum(DeepWaterFrequency(magnitude), wind.peakFrequency, wind.alpha) > atPeak * kVisibleShare)
            {
                CHECK(holders == 1u);
            }
        }
    }
}

TEST_CASE("The cascades together hold all of the spectrum's energy")
{
    for (const WaterWaves &waves : {OpenSea(), Pond()})
    {
        const WaterCascades cascades = LayoutCascades(waves);
        double total = 0.0;
        for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
        {
            total += CascadeVariance(waves, cascades, c);
        }
        const double expected = SpectrumVariance(SpectrumOf(waves.wind)) + SpectrumVariance(SpectrumOf(waves.swell));
        CHECK(total == doctest::Approx(expected).epsilon(0.1));
    }
}

TEST_CASE("The FFT matches the sum it stands for")
{
    constexpr uint32_t kSize = 16u;
    std::vector<std::complex<float>> data(kSize * kSize);
    for (uint32_t i = 0; i < data.size(); ++i)
    {
        data[i] = std::complex<float>(std::sin(0.7f * static_cast<float>(i)), std::cos(1.3f * static_cast<float>(i)));
    }
    const std::vector<std::complex<float>> input = data;
    InverseFft2D(data, kSize);

    for (uint32_t z = 0; z < kSize; ++z)
    {
        for (uint32_t x = 0; x < kSize; ++x)
        {
            std::complex<double> direct(0.0, 0.0);
            for (uint32_t kz = 0; kz < kSize; ++kz)
            {
                for (uint32_t kx = 0; kx < kSize; ++kx)
                {
                    const double angle = kTwoPi * static_cast<double>(kx * x + kz * z) / kSize;
                    const std::complex<float> value = input[kz * kSize + kx];
                    direct += std::complex<double>(value.real(), value.imag()) *
                              std::complex<double>(std::cos(angle), std::sin(angle));
                }
            }
            CAPTURE(x);
            CAPTURE(z);
            CHECK(static_cast<double>(data[z * kSize + x].real()) == doctest::Approx(direct.real()).epsilon(1e-4));
            CHECK(static_cast<double>(data[z * kSize + x].imag()) == doctest::Approx(direct.imag()).epsilon(1e-4));
        }
    }
}

TEST_CASE("A wave and its mirror make a real surface")
{
    // The height alone, through the FFT: everything imaginary must cancel.
    const WaterWaves waves = OpenSea();
    const WaterCascades cascades = LayoutCascades(waves);
    constexpr uint32_t kSize = 64u;
    std::vector<std::complex<float>> heights(kSize * kSize);
    constexpr int32_t kEdge = -static_cast<int32_t>(kSize / 2u);
    for (uint32_t z = 0; z < kSize; ++z)
    {
        for (uint32_t x = 0; x < kSize; ++x)
        {
            // The grid's first row and column have no mirror inside it.
            const glm::ivec2 frequency = FrequencyOfTexel(glm::uvec2(x, z), kSize);
            if (frequency.x == kEdge || frequency.y == kEdge)
            {
                continue;
            }
            const glm::vec2 k = WavenumberOf(frequency, cascades.tileLength[0]);
            const glm::vec2 h = WaveAt(InitialWave(waves, cascades, 0u, frequency), k, 12.5f);
            heights[z * kSize + x] = std::complex<float>(h.x, h.y);
        }
    }
    InverseFft2D(heights, kSize);
    float largestReal = 0.0f;
    float largestImaginary = 0.0f;
    for (const std::complex<float> &value : heights)
    {
        largestReal = std::max(largestReal, std::abs(value.real()));
        largestImaginary = std::max(largestImaginary, std::abs(value.imag()));
    }
    CHECK(largestReal > 0.1f);
    CHECK(largestImaginary < largestReal * 1e-4f);
}

TEST_CASE("The surface varies as much as its spectrum says")
{
    // One sea is one random draw, so average a few seeds: the variance of each
    // swings with the few dozen waves near the peak that carry most of it.
    constexpr uint32_t kSeeds = 8u;
    WaterWaves waves = OpenSea();
    waves.swell.height = 0.0f;
    const WaterCascades cascades = LayoutCascades(waves);
    double measured = 0.0;
    for (uint32_t seed = 1u; seed <= kSeeds; ++seed)
    {
        waves.seed = seed;
        const std::vector<WaterTexel> surface = EvaluateCascade(waves, cascades, 0u, 30.0, kWaterFftSize);
        REQUIRE(surface.size() == kWaterFftSize * kWaterFftSize);
        CHECK(std::abs(Mean(surface, &WaterTexel::height)) < 1e-3);
        measured += Variance(surface, &WaterTexel::height) / kSeeds;
    }
    CHECK(measured == doctest::Approx(CascadeVariance(waves, cascades, 0u)).epsilon(0.15));
}

TEST_CASE("The same settings and time give the same surface, and another seed another")
{
    const WaterWaves waves = OpenSea();
    const WaterCascades cascades = LayoutCascades(waves);
    const std::vector<WaterTexel> once = EvaluateCascade(waves, cascades, 1u, 100.0, 32u);
    const std::vector<WaterTexel> again = EvaluateCascade(waves, cascades, 1u, 100.0, 32u);
    REQUIRE(once.size() == again.size());
    bool identical = true;
    for (size_t i = 0; i < once.size(); ++i)
    {
        identical = identical && once[i].height == again[i].height && once[i].slopeX == again[i].slopeX;
    }
    CHECK(identical);

    WaterWaves other = waves;
    other.seed += 1u;
    CHECK(EvaluateCascade(other, cascades, 1u, 100.0, 32u)[5].height != once[5].height);

    // A whole loop later, the same sea.
    const std::vector<WaterTexel> looped = EvaluateCascade(waves, cascades, 1u, 100.0 + kWaterLoopSeconds, 32u);
    CHECK(looped[5].height == doctest::Approx(once[5].height).epsilon(1e-3));
}

TEST_CASE("Choppiness pulls the surface toward the crests")
{
    WaterWaves flat = OpenSea();
    flat.choppiness = 0.0f;
    const WaterCascades cascades = LayoutCascades(flat);
    const std::vector<WaterTexel> still = EvaluateCascade(flat, cascades, 0u, 20.0, 64u);
    CHECK(Variance(still, &WaterTexel::displacementX) == doctest::Approx(0.0));
    CHECK(Variance(still, &WaterTexel::displacementZ) == doctest::Approx(0.0));

    const std::vector<WaterTexel> choppy = EvaluateCascade(OpenSea(), cascades, 0u, 20.0, kWaterFftSize);
    const double meanHeight = Mean(choppy, &WaterTexel::height);
    const double deviation = std::sqrt(Variance(choppy, &WaterTexel::height));
    double crestJacobian = 0.0;
    double troughJacobian = 0.0;
    uint32_t crests = 0;
    uint32_t troughs = 0;
    double meanJacobian = 0.0;
    for (const WaterTexel &texel : choppy)
    {
        const double jacobian = static_cast<double>(WaterJacobian(texel));
        meanJacobian += jacobian;
        if (static_cast<double>(texel.height) > meanHeight + deviation)
        {
            crestJacobian += jacobian;
            ++crests;
        }
        else if (static_cast<double>(texel.height) < meanHeight - deviation)
        {
            troughJacobian += jacobian;
            ++troughs;
        }
    }
    REQUIRE(crests > 0u);
    REQUIRE(troughs > 0u);
    // Squeezed at the crests, stretched in the troughs, and no area made or lost.
    CHECK(crestJacobian / crests < 1.0);
    CHECK(troughJacobian / troughs > 1.0);
    CHECK(meanJacobian / static_cast<double>(choppy.size()) == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("The normal comes from the displaced surface's tangents")
{
    const glm::vec3 flat = WaterNormal(WaterTexel{});
    CHECK(flat.x == doctest::Approx(0.0f));
    CHECK(flat.y == doctest::Approx(1.0f));
    CHECK(flat.z == doctest::Approx(0.0f));

    // A slope of a half along x, nothing moved sideways.
    const glm::vec3 sloped = WaterNormal(WaterTexel{.slopeX = 0.5f});
    const glm::vec3 expected = glm::normalize(glm::vec3(-0.5f, 1.0f, 0.0f));
    CHECK(sloped.x == doctest::Approx(expected.x));
    CHECK(sloped.y == doctest::Approx(expected.y));
    CHECK(sloped.z == doctest::Approx(expected.z));

    // The same slope squeezed to half its width by choppiness is twice as steep.
    const glm::vec3 squeezed = WaterNormal(WaterTexel{.slopeX = 0.5f, .stretchX = -0.5f});
    const glm::vec3 steeper = glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f));
    CHECK(squeezed.x == doctest::Approx(steeper.x));
    CHECK(squeezed.y == doctest::Approx(steeper.y));
}

TEST_CASE("The Jacobian measures how much the surface is stretched")
{
    CHECK(WaterJacobian(WaterTexel{}) == doctest::Approx(1.0f));
    CHECK(WaterJacobian(WaterTexel{.stretchX = -0.5f}) == doctest::Approx(0.5f));
    CHECK(WaterJacobian(WaterTexel{.stretchX = 0.5f, .stretchZ = 0.5f, .shear = 0.5f}) == doctest::Approx(2.0f));
    // Folded over itself.
    CHECK(WaterJacobian(WaterTexel{.stretchX = -1.5f}) < 0.0f);
}

TEST_CASE("The fields one wave adds to are its height's derivatives, packed in pairs")
{
    // A wave of unit height travelling along x with wavenumber 2.
    const std::array<glm::vec2, 4> fields = WaveFields(glm::vec2(1.0f, 0.0f), glm::vec2(2.0f, 0.0f), 1.0f);
    // x displacement i·kx/|k|·h = i, packed with height h = 1 as i + i·1 = 2i.
    CHECK(fields[0].x == doctest::Approx(0.0f));
    CHECK(fields[0].y == doctest::Approx(2.0f));
    // z displacement and the shear are zero along x.
    CHECK(fields[1].x == doctest::Approx(0.0f));
    CHECK(fields[1].y == doctest::Approx(0.0f));
    // slope along x is i·kx·h = 2i; along z nothing.
    CHECK(fields[2].x == doctest::Approx(0.0f));
    CHECK(fields[2].y == doctest::Approx(2.0f));
    // stretch along x is i·kx times the x displacement, -kx²/|k|·h = -2.
    CHECK(fields[3].x == doctest::Approx(-2.0f));
    CHECK(fields[3].y == doctest::Approx(0.0f));

    // Nothing at the zero wavenumber, which has no direction.
    const std::array<glm::vec2, 4> still = WaveFields(glm::vec2(1.0f, 0.0f), glm::vec2(0.0f), 1.0f);
    CHECK(still[0].x == 0.0f);
    CHECK(still[3].x == 0.0f);
}

TEST_CASE("A cascade's offset places the waves exactly, however far from the origin")
{
    const double tile = 159.3;
    const glm::dvec2 far(300000.25, -123456.75);
    const glm::vec2 offset = CascadeOffset(far, tile);
    const long double expectedX = std::fmod(static_cast<long double>(far.x), static_cast<long double>(tile));
    long double expectedZ = std::fmod(static_cast<long double>(far.y), static_cast<long double>(tile));
    expectedZ += expectedZ < 0.0L ? static_cast<long double>(tile) : 0.0L;
    CHECK(static_cast<double>(offset.x) == doctest::Approx(static_cast<double>(expectedX)).epsilon(1e-6));
    CHECK(static_cast<double>(offset.y) == doctest::Approx(static_cast<double>(expectedZ)).epsilon(1e-6));
    CHECK(offset.x >= 0.0f);
    CHECK(offset.y >= 0.0f);
    CHECK(static_cast<double>(offset.x) < tile);

    // A whole tile further on lands in the same place.
    const glm::vec2 next = CascadeOffset(far + glm::dvec2(tile, -tile), tile);
    CHECK(next.x == doctest::Approx(offset.x).epsilon(1e-5));
    CHECK(next.y == doctest::Approx(offset.y).epsilon(1e-5));
}

TEST_CASE("Sanitized waves keep every field finite and in range")
{
    WaterWaves wild;
    wild.wind.windSpeed = std::numeric_limits<float>::quiet_NaN();
    wild.wind.fetch = 1.0f;
    wild.wind.spread = 4.0f;
    wild.wind.bearingDegrees = 450.0f;
    wild.swell.height = 1000.0f;
    wild.swell.period = std::numeric_limits<float>::infinity();
    wild.swell.bearingDegrees = std::numeric_limits<float>::quiet_NaN();
    wild.choppiness = -3.0f;

    const WaterWaves tame = Sanitized(wild);
    CHECK(tame.wind.windSpeed == 0.0f);
    CHECK(tame.wind.fetch == kMinWaterFetch);
    CHECK(tame.wind.spread == 1.0f);
    CHECK(tame.wind.bearingDegrees == doctest::Approx(90.0f));
    CHECK(tame.swell.height == kMaxSwellHeight);
    CHECK(tame.swell.period == kDefaultSwellPeriod);
    CHECK(tame.swell.bearingDegrees == 0.0f);
    CHECK(tame.choppiness == 0.0f);
    CHECK(Sanitized(tame) == tame);
}
