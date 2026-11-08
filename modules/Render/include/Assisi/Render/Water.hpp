/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Water.hpp
/// @brief Waves on open water: what each wave starts as, how fast it travels,
/// and the cascades of tiles that hold them.
///
/// A water surface is the sum of many waves of every length and direction,
/// each a size an ocean spectrum gives for the wind that raised it. Each frame
/// the GPU moves every wave forward to the time and adds them up with an
/// inverse FFT, into four cascades: tiles of different sizes, each holding one
/// band of wavelengths, layered so no repeat shows. The method is Tessendorf's,
/// "Simulating Ocean Water" (2001); the spectrum and its spread around the wind
/// are those Horvath fitted to measured seas, "Empirical Directional Wave
/// Spectra for Computer Graphics" (2015).
///
/// Everything here depends only on the settings and the time, so every machine
/// sharing a clock draws the same waves without sending any.
///
/// **These functions are the specification and water_spectrum.comp,
/// water_fft.comp and water_resolve.comp their transcription**; the tests hold
/// the CPU form.

#include <Assisi/Math/GLM.hpp>

#include <array>
#include <complex>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace Assisi::Render
{

/// @brief Gravity at the water's surface, in metres per second squared.
inline constexpr float kWaterGravity = 9.81f;

/// @brief How many cascades hold a body's waves: enough that the open sea keeps
/// waves a few centimetres long near the camera while its largest tile holds
/// swell hundreds of metres long.
inline constexpr uint32_t kWaterCascadeCount = 4u;

/// @brief Each cascade's texture is this many texels on a side. A power of two,
/// which the FFT needs.
inline constexpr uint32_t kWaterFftSize = 256u;

/// @brief The waves repeat after this many seconds exactly. Every wave's
/// frequency is rounded to a whole number of turns in it, so the time can be
/// wrapped to it in double and the phases stay precise in float after hours of
/// play; a loop this long is never noticed.
inline constexpr double kWaterLoopSeconds = 1024.0;

/// @brief Each cascade's tile is this many times smaller than the one before.
/// Not a whole number, so the tiles' repeats never line up.
inline constexpr float kWaterCascadeRatio = 5.7f;

/// @brief A cascade takes over from the one before at this harmonic of its own
/// tile: its first few are too coarse a step to hold the waves the larger tile
/// already holds finely.
inline constexpr float kWaterBandStartHarmonic = 6.0f;

/// @brief The largest tile holds this many of the longest waves the spectrum
/// peaks at. A body whose energy sits in one band shows that band's tile
/// repeating across it; this many waves to a tile spread the peak over many
/// directions and across two cascades whose sizes never line up, so no repeat
/// shows even on a lake a few tiles wide.
inline constexpr float kWaterTileWavelengths = 32.0f;

/// @name The largest tile's size, in metres
/// @{
/// For water too calm to have a peak: below this the tiles would hold nothing
/// a mesh or a pixel can show.
inline constexpr float kWaterMinTileLength = 1.0f;
/// Holds the longest swell a spectrum here makes.
inline constexpr float kWaterMaxTileLength = 4000.0f;
/// @}

/// @name Wind
/// @{
/// A hurricane's wind, the fastest a spectrum is asked for.
inline constexpr float kMaxWaterWindSpeed = 40.0f;
/// Fetch: how far the wind has blown over open water, in metres. A puddle's
/// to a whole ocean's.
inline constexpr float kMinWaterFetch = 10.0f;
inline constexpr float kMaxWaterFetch = 2.0e6f;
/// The open sea's fetch, where nothing nearby limits it.
inline constexpr float kOceanFetch = 1.0e5f;
/// @}

/// @name Swell, as a forecast gives it
/// @{
/// Metres of significant height.
inline constexpr float kMaxSwellHeight = 10.0f;
/// Seconds between crests.
inline constexpr float kMinSwellPeriod = 4.0f;
inline constexpr float kMaxSwellPeriod = 25.0f;
inline constexpr float kDefaultSwellPeriod = 10.0f;
/// @}

/// @brief How far choppiness can pull the surface toward the crests. Past it
/// most crests fold over themselves.
inline constexpr float kMaxWaterChoppiness = 2.0f;

/// @brief The waves a local wind raises.
struct WindSea
{
    /// Metres per second, measured ten metres above the water. Zero raises no
    /// waves.
    float windSpeed = 0.0f;
    /// How far the wind has blown over open water, in metres. Longer fetch
    /// grows longer, taller waves.
    float fetch = kOceanFetch;
    /// The compass bearing the wind blows toward: 0 north, 90 east.
    float bearingDegrees = 0.0f;
    /// From 0, every wave travels near the wind's bearing, to 1, waves travel
    /// every way equally.
    float spread = 0.0f;

    bool operator==(const WindSea &) const = default;
};

/// @brief Swell: long, even waves from a storm far away, described as a
/// forecast gives them.
struct Swell
{
    /// The significant height, in metres. Zero is no swell.
    float height = 0.0f;
    /// Seconds between crests.
    float period = kDefaultSwellPeriod;
    /// The compass bearing the swell travels toward: 0 north, 90 east.
    float bearingDegrees = 0.0f;

    bool operator==(const Swell &) const = default;
};

/// @brief Everything that decides a body's waves.
struct WaterWaves
{
    WindSea wind;
    Swell swell;
    /// How far the surface is pulled sideways toward each crest, which
    /// sharpens the crests and flattens the troughs. Zero moves it only up and
    /// down.
    float choppiness = 1.0f;
    /// Which sea of its kind: the same settings with another seed give other
    /// waves.
    uint32_t seed = 0u;

    bool operator==(const WaterWaves &) const = default;
};

/// @brief @p waves with every field finite and in range.
[[nodiscard]] WaterWaves Sanitized(WaterWaves waves);

/// @brief One JONSWAP spectrum, as the GPU takes it: where it peaks, how much
/// energy it holds and how it spreads around its bearing. A wind sea and a swell
/// both come to one of these.
struct WaveSpectrum
{
    /// ωp, in radians per second. Zero holds no waves.
    float peakFrequency = 0.0f;
    /// JONSWAP's α: how much energy the sea holds overall.
    float alpha = 0.0f;
    /// U·ωp/g less a fully grown sea's: how young the sea is, which widens the
    /// spread of the waves shorter than the peak.
    float youth = 0.0f;
    float bearingDegrees = 0.0f;
    /// From 0, close to the bearing, to 1, every way equally.
    float spread = 0.0f;
    /// From 0 to 1: how far the crests are drawn out into the long, even lines
    /// of swell.
    float swell = 0.0f;

    bool operator==(const WaveSpectrum &) const = default;
};

/// @name The spectrum
/// @{
/// @brief The angular frequency, in radians per second, at which a wind of
/// @p windSpeed over @p fetch raises the most wave energy.
[[nodiscard]] float JonswapPeakFrequency(float windSpeed, float fetch);

/// @brief JONSWAP's α for a wind of @p windSpeed over @p fetch: the energy a
/// young sea holds, which grows with the wind and shrinks as the fetch lengthens
/// the waves.
[[nodiscard]] float JonswapAlpha(float windSpeed, float fetch);

/// @brief The wind sea's spectrum.
[[nodiscard]] WaveSpectrum SpectrumOf(const WindSea &wind);

/// @brief The swell's spectrum: peaking at its period, scaled to its height,
/// spreading as a fully grown sea's and drawn out into long crests.
[[nodiscard]] WaveSpectrum SpectrumOf(const Swell &swell);

/// @brief The JONSWAP spectrum: how much the height of the sea varies, per unit
/// of angular frequency, at @p omega, for a sea peaking at @p peakFrequency
/// with energy @p alpha.
[[nodiscard]] float JonswapSpectrum(float omega, float peakFrequency, float alpha);

/// @brief The significant wave height of @p spectrum, in metres: four standard
/// deviations of the surface's height, what a sailor reports as the height of
/// the sea.
[[nodiscard]] float SignificantWaveHeight(const WaveSpectrum &spectrum);

/// @brief The exponent of the spread around the bearing at @p omega: larger
/// keeps waves nearer it. Waves near the peak spread least.
[[nodiscard]] float SpreadExponent(float omega, const WaveSpectrum &spectrum);

/// @brief What makes Q(s)·|cos(θ/2)|^(2s) integrate to one around the circle:
/// Γ(s + 1) / (2·√π·Γ(s + ½)).
[[nodiscard]] float SpreadNormalization(float exponent);

/// @brief How a wave's energy shares out by the angle @p theta between where it
/// travels and the wind's bearing, in radians; integrates to one around the
/// circle.
[[nodiscard]] float DirectionalSpread(float theta, float exponent, float spread);

/// @brief The angular frequency of a deep-water wave of wavenumber @p k.
[[nodiscard]] float DeepWaterFrequency(float k);

/// @brief @p omega rounded down to a whole number of turns per
/// kWaterLoopSeconds, which is what makes the waves loop.
[[nodiscard]] float LoopedFrequency(float omega);
/// @}

/// @brief Where a body's cascades lie: each tile's size, and the band of
/// wavenumbers it holds, which meet without overlapping.
struct WaterCascades
{
    /// Metres, largest first.
    std::array<float, kWaterCascadeCount> tileLength{};
    /// Radians per metre; a wave belongs to the cascade whose band holds its
    /// wavenumber.
    std::array<float, kWaterCascadeCount> minWavenumber{};
    std::array<float, kWaterCascadeCount> maxWavenumber{};

    bool operator==(const WaterCascades &) const = default;
};

/// @brief The cascades sized to @p waves: the largest tile holds a few of the
/// longest peak waves, and each after is kWaterCascadeRatio smaller.
[[nodiscard]] WaterCascades LayoutCascades(const WaterWaves &waves);

/// @brief The wave a texel of an FFT texture of side @p size holds, as whole
/// cycles across its tile along x and z; the texture's middle is zero.
[[nodiscard]] glm::ivec2 FrequencyOfTexel(glm::uvec2 texel, uint32_t size);

/// @brief The wavenumber vector of @p frequency on a tile @p tileLength wide,
/// in radians per metre along x and z.
[[nodiscard]] glm::vec2 WavenumberOf(glm::ivec2 frequency, float tileLength);

/// @brief Mixes @p value to a well-spread 32-bit integer: the PCG hash. Every
/// GPU and CPU computes it the same, bit for bit.
[[nodiscard]] uint32_t WaterHash(uint32_t value);

/// @brief Two independent random numbers of mean 0 and variance 1, the same
/// every time for the same @p seed, @p cascade and @p frequency.
[[nodiscard]] glm::vec2 GaussianPair(uint32_t seed, uint32_t cascade, glm::ivec2 frequency);

/// @brief How tall a wave of wavenumber @p k in @p cascade starts, before its
/// random draw: its share of the spectrum's variance on the cascade's grid.
/// Zero outside the cascade's band.
[[nodiscard]] float WaveAmplitude(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade,
                                  glm::vec2 k);

/// @brief A wave's start, h0(k) in xy, and its mirror's, conj(h0(-k)) in zw:
/// what water_spectrum.comp writes, once, for each texel.
[[nodiscard]] glm::vec4 InitialWave(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade,
                                    glm::ivec2 frequency);

/// @brief The wave of wavenumber @p k that started as @p initial, at
/// @p loopSeconds into the loop: h0·e^(iωt) + conj(h0(-k))·e^(-iωt), as a
/// complex number in xy.
[[nodiscard]] glm::vec2 WaveAt(glm::vec4 initial, glm::vec2 k, float loopSeconds);

/// @brief @p seconds wrapped into the loop, in [0, kWaterLoopSeconds).
[[nodiscard]] double WaterLoopTime(double seconds);

/// @brief The four complex fields one wave of wavenumber @p k and complex
/// height @p height adds to, each two real fields packed as real + i·imaginary:
///   0: x displacement + i·height
///   1: z displacement + i·d(x displacement)/dz
///   2: d(height)/dx + i·d(height)/dz
///   3: d(x displacement)/dx + i·d(z displacement)/dz
/// The x displacement is i·(kx/|k|)·h·@p choppiness, and z's likewise: the
/// sign that moves points toward each crest, so a wave h = A·cos(kx) moves
/// them by -A·sin(kx), as a Gerstner wave does.
[[nodiscard]] std::array<glm::vec2, 4> WaveFields(glm::vec2 height, glm::vec2 k, float choppiness);

/// @brief One point of a cascade's surface: where it moved to and how the
/// surface bends there.
struct WaterTexel
{
    float height = 0.0f;
    float displacementX = 0.0f;
    float displacementZ = 0.0f;
    float slopeX = 0.0f;
    float slopeZ = 0.0f;
    float stretchX = 0.0f;
    float stretchZ = 0.0f;
    /// d(x displacement)/dz, which equals d(z displacement)/dx.
    float shear = 0.0f;
};

/// @brief The surface's normal at @p texel, from the tangents of the displaced
/// surface, so the sharp crests choppiness makes are lit correctly.
[[nodiscard]] glm::vec3 WaterNormal(const WaterTexel &texel);

/// @brief How much the surface is stretched (above 1) or squeezed (below 1)
/// at @p texel. Below zero it has folded over itself: a breaking crest.
[[nodiscard]] float WaterJacobian(const WaterTexel &texel);

/// @brief The inverse discrete Fourier transform of @p data in place, without a
/// 1/N: x_n = Σ_k X_k·e^(2πi·kn/N). Its size must be a power of two.
void InverseFft(std::span<std::complex<float>> data);

/// @brief InverseFft over every row of a @p size by @p size grid, then every
/// column.
void InverseFft2D(std::span<std::complex<float>> data, uint32_t size);

/// @brief One cascade's surface at @p seconds, evaluated on the CPU on a grid of
/// @p size by @p size points spaced evenly across its tile, row by row along z.
///
/// @p size may be smaller than kWaterFftSize: it then holds the cascade's
/// longest waves only, which is what floating bodies need.
[[nodiscard]] std::vector<WaterTexel> EvaluateCascade(const WaterWaves &waves, const WaterCascades &cascades,
                                                      uint32_t cascade, double seconds, uint32_t size);

/// @brief Where a world position lands in a cascade's tile, in metres from the
/// tile's corner: the camera's position modulo @p tileLength, in double. Added
/// to a position measured from the camera, it places the waves in the world
/// however far from the origin the camera is.
[[nodiscard]] glm::vec2 CascadeOffset(glm::dvec2 position, double tileLength);

} // namespace Assisi::Render
