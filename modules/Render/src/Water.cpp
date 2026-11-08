/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/Water.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace Assisi::Render
{
namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0f * kPi;

/// @name JONSWAP, as Horvath gives it
/// @{
/// α = kJonswapAlphaScale·(U²/(F·g))^kJonswapAlphaExponent.
constexpr float kJonswapAlphaScale = 0.076f;
constexpr float kJonswapAlphaExponent = 0.22f;
/// ωp = kJonswapPeakScale·(g²/(U·F))^(1/3).
constexpr float kJonswapPeakScale = 22.0f;
/// How sharply the spectrum rises to its peak: S ∝ exp(-kJonswapRise·(ωp/ω)^4).
constexpr float kJonswapRise = 1.25f;
/// γ: how much taller the peak is than a fully grown sea's.
constexpr float kJonswapPeakEnhancement = 3.3f;
/// The width of the peak's enhancement below and above it, as a share of ωp.
constexpr float kJonswapWidthBelow = 0.07f;
constexpr float kJonswapWidthAbove = 0.09f;
/// Below this share of ωp the spectrum is zero to float precision, and ω^5
/// would underflow first.
constexpr float kJonswapLowestShare = 0.1f;
/// @}

/// @name The spread around the bearing, Hasselmann's as Horvath fits it
/// @{
/// Below the peak: s = kSpreadBelowScale·(ω/ωp)^kSpreadBelowExponent.
constexpr float kSpreadBelowScale = 6.97f;
constexpr float kSpreadBelowExponent = 4.06f;
/// Above it: s = kSpreadAboveScale·(ω/ωp)^(kSpreadAboveExponent +
/// kSpreadAboveYouthExponent·youth).
constexpr float kSpreadAboveScale = 9.77f;
constexpr float kSpreadAboveExponent = -2.33f;
constexpr float kSpreadAboveYouthExponent = -1.45f;
/// U·ωp/g of a fully grown sea in that fit; a sea's youth is how far above it
/// its own is.
constexpr float kGrownWindRatio = 1.17f;
/// Swell adds kSpreadSwellScale·tanh(ωp/ω)·ξ².
constexpr float kSpreadSwellScale = 16.0f;
/// Past this a wave keeps to the bearing within a few degrees, and the exponent
/// need go no higher.
constexpr float kMaxSpreadExponent = 64.0f;
/// @}

/// @name Γ(s + 1)/Γ(s + ½)
/// @{
/// The asymptotic series is accurate to float precision from here up; below,
/// the recurrence steps down to it.
constexpr float kGammaRatioSeriesStart = 8.0f;
/// √x·(1 + 1/(8x) + 1/(128x²) - 5/(1024x³) - 21/(32768x⁴)).
constexpr float kGammaRatioTerm1 = 1.0f / 8.0f;
constexpr float kGammaRatioTerm2 = 1.0f / 128.0f;
constexpr float kGammaRatioTerm3 = -5.0f / 1024.0f;
constexpr float kGammaRatioTerm4 = -21.0f / 32768.0f;
/// @}

/// @name Integrating the spectrum
/// @{
/// Up to this many times the peak frequency, past which the tail holds under a
/// thousandth of the energy.
constexpr double kSpectrumHighestShare = 20.0;
constexpr uint32_t kSpectrumSteps = 4000u;
/// The significant height is four standard deviations.
constexpr float kSignificantHeightDeviations = 4.0f;
/// @}

/// Mixed into a draw's key for the second of its two uniform numbers: the
/// golden ratio's bits, so the two never share a hash.
constexpr uint32_t kSecondDrawKey = 0x9E3779B9u;
/// A uniform number takes a hash's top 24 bits, all a float holds exactly.
constexpr uint32_t kUniformShift = 8u;
constexpr float kUniformScale = 1.0f / 16777216.0f;
/// Half a step, so the first uniform is never zero and its logarithm finite.
constexpr float kUniformCentre = 0.5f;

float ClampFinite(float value, float low, float high, float fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

float WrapBearing(float degrees, float fallback)
{
    return std::isfinite(degrees) ? std::fmod(degrees, 360.0f) : fallback;
}

WindSea Sanitized(WindSea wind)
{
    const WindSea defaults;
    wind.windSpeed = ClampFinite(wind.windSpeed, 0.0f, kMaxWaterWindSpeed, defaults.windSpeed);
    wind.fetch = ClampFinite(wind.fetch, kMinWaterFetch, kMaxWaterFetch, defaults.fetch);
    wind.bearingDegrees = WrapBearing(wind.bearingDegrees, defaults.bearingDegrees);
    wind.spread = ClampFinite(wind.spread, 0.0f, 1.0f, defaults.spread);
    return wind;
}

Swell Sanitized(Swell swell)
{
    const Swell defaults;
    swell.height = ClampFinite(swell.height, 0.0f, kMaxSwellHeight, defaults.height);
    swell.period = ClampFinite(swell.period, kMinSwellPeriod, kMaxSwellPeriod, defaults.period);
    swell.bearingDegrees = WrapBearing(swell.bearingDegrees, defaults.bearingDegrees);
    return swell;
}

/// ∫ S(ω) dω for @p spectrum, by the midpoint rule.
double SpectrumVariance(float peakFrequency, float alpha)
{
    const double step = static_cast<double>(peakFrequency) * kSpectrumHighestShare / kSpectrumSteps;
    double variance = 0.0;
    for (uint32_t i = 0; i < kSpectrumSteps; ++i)
    {
        const double omega = (static_cast<double>(i) + 0.5) * step;
        variance += static_cast<double>(JonswapSpectrum(static_cast<float>(omega), peakFrequency, alpha)) * step;
    }
    return variance;
}

/// ∫ x^-5·exp(-1.25·x^-4)·γ^r(x) dx: the variance of a JONSWAP sea of α = 1
/// peaking at 1, in units where g = 1. A sea's variance is α·g²/ωp⁴ times it.
float JonswapShapeVariance()
{
    static const float variance =
        static_cast<float>(SpectrumVariance(1.0f, 1.0f) / static_cast<double>(kWaterGravity * kWaterGravity));
    return variance;
}

/// Γ(s + 1)/Γ(s + ½) for s ≥ 0.
float GammaRatio(float s)
{
    float x = s;
    uint32_t steps = 0u;
    while (x < kGammaRatioSeriesStart)
    {
        x += 1.0f;
        ++steps;
    }
    const float inverse = 1.0f / x;
    float ratio = std::sqrt(x) *
                  (1.0f + inverse * (kGammaRatioTerm1 +
                                     inverse * (kGammaRatioTerm2 + inverse * (kGammaRatioTerm3 + inverse * kGammaRatioTerm4))));
    // R(x - 1) = R(x)·(x - ½)/x, from Γ(x + 1) = x·Γ(x).
    for (uint32_t i = 0; i < steps; ++i)
    {
        ratio *= (x - 0.5f) / x;
        x -= 1.0f;
    }
    return ratio;
}

/// The wave energy per unit wavenumber area that @p spectrum puts at @p k:
/// S(ω)·D(θ)·(dω/dk)/k, which is S(ω, θ) moved from frequency and angle into
/// wavenumber space.
float WavenumberSpectrum(const WaveSpectrum &spectrum, glm::vec2 k, float kLength)
{
    if (spectrum.peakFrequency <= 0.0f)
    {
        return 0.0f;
    }
    const float omega = DeepWaterFrequency(kLength);
    const float bearing = glm::radians(spectrum.bearingDegrees);
    // North is -z and east +x, so a bearing turns from -z toward +x.
    const glm::vec2 toward(std::sin(bearing), -std::cos(bearing));
    const float theta = std::atan2(toward.x * k.y - toward.y * k.x, glm::dot(toward, k));
    const float omegaPerK = kWaterGravity / (2.0f * omega);
    return JonswapSpectrum(omega, spectrum.peakFrequency, spectrum.alpha) *
           DirectionalSpread(theta, SpreadExponent(omega, spectrum), spectrum.spread) * omegaPerK / kLength;
}

glm::vec2 Multiply(glm::vec2 a, glm::vec2 b)
{
    return glm::vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

/// i·@p a.
glm::vec2 TimesI(glm::vec2 a)
{
    return glm::vec2(-a.y, a.x);
}

} // namespace

WaterWaves Sanitized(WaterWaves waves)
{
    const WaterWaves defaults;
    waves.wind = Sanitized(waves.wind);
    waves.swell = Sanitized(waves.swell);
    waves.choppiness = ClampFinite(waves.choppiness, 0.0f, kMaxWaterChoppiness, defaults.choppiness);
    return waves;
}

float JonswapPeakFrequency(float windSpeed, float fetch)
{
    if (windSpeed <= 0.0f || fetch <= 0.0f)
    {
        return 0.0f;
    }
    return kJonswapPeakScale * std::cbrt(kWaterGravity * kWaterGravity / (windSpeed * fetch));
}

float JonswapAlpha(float windSpeed, float fetch)
{
    if (windSpeed <= 0.0f || fetch <= 0.0f)
    {
        return 0.0f;
    }
    return kJonswapAlphaScale * std::pow(windSpeed * windSpeed / (fetch * kWaterGravity), kJonswapAlphaExponent);
}

WaveSpectrum SpectrumOf(const WindSea &wind)
{
    const float peak = JonswapPeakFrequency(wind.windSpeed, wind.fetch);
    if (peak <= 0.0f)
    {
        return WaveSpectrum{};
    }
    return WaveSpectrum{.peakFrequency = peak,
                        .alpha = JonswapAlpha(wind.windSpeed, wind.fetch),
                        .youth = wind.windSpeed * peak / kWaterGravity - kGrownWindRatio,
                        .bearingDegrees = wind.bearingDegrees,
                        .spread = wind.spread,
                        .swell = 0.0f};
}

WaveSpectrum SpectrumOf(const Swell &swell)
{
    if (swell.height <= 0.0f || swell.period <= 0.0f)
    {
        return WaveSpectrum{};
    }
    // Hs = 4·√(α·g²/ωp⁴·shape), solved for α.
    const float peak = kTwoPi / swell.period;
    const float peak2 = peak * peak;
    const float deviation = swell.height / kSignificantHeightDeviations;
    return WaveSpectrum{.peakFrequency = peak,
                        .alpha = deviation * deviation * peak2 * peak2 /
                                 (kWaterGravity * kWaterGravity * JonswapShapeVariance()),
                        .youth = 0.0f,
                        .bearingDegrees = swell.bearingDegrees,
                        .spread = 0.0f,
                        .swell = 1.0f};
}

float JonswapSpectrum(float omega, float peakFrequency, float alpha)
{
    if (peakFrequency <= 0.0f || alpha <= 0.0f || omega < peakFrequency * kJonswapLowestShare)
    {
        return 0.0f;
    }
    const float width = omega <= peakFrequency ? kJonswapWidthBelow : kJonswapWidthAbove;
    const float offPeak = (omega - peakFrequency) / (width * peakFrequency);
    const float enhancement = std::pow(kJonswapPeakEnhancement, std::exp(-0.5f * offPeak * offPeak));
    const float ratio = peakFrequency / omega;
    const float ratio2 = ratio * ratio;
    const float omega2 = omega * omega;
    return alpha * kWaterGravity * kWaterGravity / (omega2 * omega2 * omega) * std::exp(-kJonswapRise * ratio2 * ratio2) *
           enhancement;
}

float SignificantWaveHeight(const WaveSpectrum &spectrum)
{
    if (spectrum.peakFrequency <= 0.0f)
    {
        return 0.0f;
    }
    return kSignificantHeightDeviations *
           static_cast<float>(std::sqrt(SpectrumVariance(spectrum.peakFrequency, spectrum.alpha)));
}

float SpreadExponent(float omega, const WaveSpectrum &spectrum)
{
    if (omega <= 0.0f || spectrum.peakFrequency <= 0.0f)
    {
        return 0.0f;
    }
    const float ratio = omega / spectrum.peakFrequency;
    float exponent = 0.0f;
    if (ratio <= 1.0f)
    {
        exponent = kSpreadBelowScale * std::pow(ratio, kSpreadBelowExponent);
    }
    else
    {
        exponent = kSpreadAboveScale * std::pow(ratio, kSpreadAboveExponent + kSpreadAboveYouthExponent * spectrum.youth);
    }
    exponent += kSpreadSwellScale * std::tanh(spectrum.peakFrequency / omega) * spectrum.swell * spectrum.swell;
    return std::clamp(exponent, 0.0f, kMaxSpreadExponent);
}

float SpreadNormalization(float exponent)
{
    return GammaRatio(std::max(exponent, 0.0f)) / (2.0f * std::sqrt(kPi));
}

float DirectionalSpread(float theta, float exponent, float spread)
{
    const float flat = 1.0f / kTwoPi;
    const float narrow =
        exponent > 0.0f ? SpreadNormalization(exponent) * std::pow(std::abs(std::cos(0.5f * theta)), 2.0f * exponent)
                        : flat;
    return narrow + (flat - narrow) * spread;
}

float DeepWaterFrequency(float k)
{
    return std::sqrt(kWaterGravity * std::max(k, 0.0f));
}

float LoopedFrequency(float omega)
{
    const float turn = static_cast<float>(2.0 * std::numbers::pi / kWaterLoopSeconds);
    return std::floor(omega / turn) * turn;
}

WaterCascades LayoutCascades(const WaterWaves &waves)
{
    float longest = 0.0f;
    for (const float peak : {SpectrumOf(waves.wind).peakFrequency, SpectrumOf(waves.swell).peakFrequency})
    {
        if (peak > 0.0f)
        {
            longest = std::max(longest, kTwoPi * kWaterGravity / (peak * peak));
        }
    }

    WaterCascades cascades;
    float tile = std::clamp(kWaterTileWavelengths * longest, kWaterMinTileLength, kWaterMaxTileLength);
    for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
    {
        cascades.tileLength[c] = tile;
        tile /= kWaterCascadeRatio;
    }
    cascades.minWavenumber[0] = 0.0f;
    for (uint32_t c = 1; c < kWaterCascadeCount; ++c)
    {
        cascades.minWavenumber[c] = kWaterBandStartHarmonic * kTwoPi / cascades.tileLength[c];
        cascades.maxWavenumber[c - 1u] = cascades.minWavenumber[c];
    }
    constexpr uint32_t kLast = kWaterCascadeCount - 1u;
    cascades.maxWavenumber[kLast] = static_cast<float>(kWaterFftSize / 2u) * kTwoPi / cascades.tileLength[kLast];
    return cascades;
}

glm::ivec2 FrequencyOfTexel(glm::uvec2 texel, uint32_t size)
{
    return glm::ivec2(texel) - glm::ivec2(static_cast<int32_t>(size / 2u));
}

glm::vec2 WavenumberOf(glm::ivec2 frequency, float tileLength)
{
    return glm::vec2(frequency) * (kTwoPi / tileLength);
}

uint32_t WaterHash(uint32_t value)
{
    // Jarzynski and Olano, "Hash Functions for GPU Rendering" (2020).
    const uint32_t state = value * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

glm::vec2 GaussianPair(uint32_t seed, uint32_t cascade, glm::ivec2 frequency)
{
    const uint32_t key =
        WaterHash(seed ^ WaterHash(cascade ^ WaterHash(static_cast<uint32_t>(frequency.x) ^
                                                       WaterHash(static_cast<uint32_t>(frequency.y)))));
    const float first = (static_cast<float>(WaterHash(key) >> kUniformShift) + kUniformCentre) * kUniformScale;
    const float second = static_cast<float>(WaterHash(key ^ kSecondDrawKey) >> kUniformShift) * kUniformScale;
    // Box and Muller.
    const float radius = std::sqrt(-2.0f * std::log(first));
    const float angle = kTwoPi * second;
    return glm::vec2(radius * std::cos(angle), radius * std::sin(angle));
}

float WaveAmplitude(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade, glm::vec2 k)
{
    const float kLength = glm::length(k);
    if (kLength <= 0.0f || kLength < cascades.minWavenumber[cascade] || kLength >= cascades.maxWavenumber[cascade])
    {
        return 0.0f;
    }
    const float step = kTwoPi / cascades.tileLength[cascade];
    const float energy = WavenumberSpectrum(SpectrumOf(waves.wind), k, kLength) +
                         WavenumberSpectrum(SpectrumOf(waves.swell), k, kLength);
    // A wave and its mirror together add twice this squared to the height's
    // variance, which is then the spectrum's over this texel's area.
    return std::sqrt(0.5f * energy * step * step);
}

glm::vec4 InitialWave(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade, glm::ivec2 frequency)
{
    const float tile = cascades.tileLength[cascade];
    const float inverseRoot2 = 1.0f / std::sqrt(2.0f);
    const glm::vec2 wave = GaussianPair(waves.seed, cascade, frequency) * inverseRoot2 *
                           WaveAmplitude(waves, cascades, cascade, WavenumberOf(frequency, tile));
    const glm::vec2 mirror = GaussianPair(waves.seed, cascade, -frequency) * inverseRoot2 *
                             WaveAmplitude(waves, cascades, cascade, WavenumberOf(-frequency, tile));
    return glm::vec4(wave.x, wave.y, mirror.x, -mirror.y);
}

glm::vec2 WaveAt(glm::vec4 initial, glm::vec2 k, float loopSeconds)
{
    const float phase = LoopedFrequency(DeepWaterFrequency(glm::length(k))) * loopSeconds;
    const glm::vec2 turn(std::cos(phase), std::sin(phase));
    return Multiply(glm::vec2(initial.x, initial.y), turn) + Multiply(glm::vec2(initial.z, initial.w), glm::vec2(turn.x, -turn.y));
}

double WaterLoopTime(double seconds)
{
    const double wrapped = std::fmod(seconds, kWaterLoopSeconds);
    return wrapped < 0.0 ? wrapped + kWaterLoopSeconds : wrapped;
}

std::array<glm::vec2, 4> WaveFields(glm::vec2 height, glm::vec2 k, float choppiness)
{
    const float kLength = glm::length(k);
    if (kLength <= 0.0f)
    {
        return {};
    }
    const glm::vec2 iHeight = TimesI(height);
    const glm::vec2 displacementX = iHeight * (choppiness * k.x / kLength);
    const glm::vec2 displacementZ = iHeight * (choppiness * k.y / kLength);
    const glm::vec2 slopeX = iHeight * k.x;
    const glm::vec2 slopeZ = iHeight * k.y;
    // Each derivative is i·k times what it differentiates.
    const glm::vec2 stretchX = TimesI(displacementX) * k.x;
    const glm::vec2 stretchZ = TimesI(displacementZ) * k.y;
    const glm::vec2 shear = TimesI(displacementX) * k.y;
    return {displacementX + iHeight, displacementZ + TimesI(shear), slopeX + TimesI(slopeZ),
            stretchX + TimesI(stretchZ)};
}

glm::vec3 WaterNormal(const WaterTexel &texel)
{
    const glm::vec3 alongX(1.0f + texel.stretchX, texel.slopeX, texel.shear);
    const glm::vec3 alongZ(texel.shear, texel.slopeZ, 1.0f + texel.stretchZ);
    return glm::normalize(glm::cross(alongZ, alongX));
}

float WaterJacobian(const WaterTexel &texel)
{
    return (1.0f + texel.stretchX) * (1.0f + texel.stretchZ) - texel.shear * texel.shear;
}

void InverseFft(std::span<std::complex<float>> data)
{
    const size_t size = data.size();
    // Reorder into bit-reversed position, then combine in doubling spans.
    for (size_t i = 1, j = 0; i < size; ++i)
    {
        size_t bit = size >> 1u;
        for (; (j & bit) != 0u; bit >>= 1u)
        {
            j ^= bit;
        }
        j ^= bit;
        if (i < j)
        {
            std::swap(data[i], data[j]);
        }
    }
    for (size_t span = 2; span <= size; span <<= 1u)
    {
        const double angle = 2.0 * std::numbers::pi / static_cast<double>(span);
        for (size_t start = 0; start < size; start += span)
        {
            for (size_t i = 0; i < span / 2u; ++i)
            {
                const double turn = angle * static_cast<double>(i);
                const std::complex<float> twiddle(static_cast<float>(std::cos(turn)), static_cast<float>(std::sin(turn)));
                const std::complex<float> even = data[start + i];
                const std::complex<float> odd = data[start + i + span / 2u] * twiddle;
                data[start + i] = even + odd;
                data[start + i + span / 2u] = even - odd;
            }
        }
    }
}

void InverseFft2D(std::span<std::complex<float>> data, uint32_t size)
{
    for (uint32_t row = 0; row < size; ++row)
    {
        InverseFft(data.subspan(static_cast<size_t>(row) * size, size));
    }
    std::vector<std::complex<float>> column(size);
    for (uint32_t x = 0; x < size; ++x)
    {
        for (uint32_t z = 0; z < size; ++z)
        {
            column[z] = data[static_cast<size_t>(z) * size + x];
        }
        InverseFft(column);
        for (uint32_t z = 0; z < size; ++z)
        {
            data[static_cast<size_t>(z) * size + x] = column[z];
        }
    }
}

std::vector<WaterTexel> EvaluateCascade(const WaterWaves &waves, const WaterCascades &cascades, uint32_t cascade,
                                        double seconds, uint32_t size)
{
    if (!std::has_single_bit(size) || size > kWaterFftSize || cascade >= kWaterCascadeCount)
    {
        return {};
    }
    const float loopSeconds = static_cast<float>(WaterLoopTime(seconds));
    const size_t count = static_cast<size_t>(size) * size;
    std::array<std::vector<std::complex<float>>, 4> fields;
    for (std::vector<std::complex<float>> &field : fields)
    {
        field.assign(count, std::complex<float>(0.0f, 0.0f));
    }

    // The grid's first row and column hold waves whose mirrors fall outside it;
    // left empty, every field stays real.
    const int32_t edge = -static_cast<int32_t>(size / 2u);
    for (uint32_t z = 0; z < size; ++z)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const glm::ivec2 frequency = FrequencyOfTexel(glm::uvec2(x, z), size);
            if (frequency.x == edge || frequency.y == edge)
            {
                continue;
            }
            const glm::vec2 k = WavenumberOf(frequency, cascades.tileLength[cascade]);
            const glm::vec2 height = WaveAt(InitialWave(waves, cascades, cascade, frequency), k, loopSeconds);
            const std::array<glm::vec2, 4> packed = WaveFields(height, k, waves.choppiness);
            for (size_t f = 0; f < fields.size(); ++f)
            {
                fields[f][static_cast<size_t>(z) * size + x] = std::complex<float>(packed[f].x, packed[f].y);
            }
        }
    }

    for (std::vector<std::complex<float>> &field : fields)
    {
        InverseFft2D(field, size);
    }

    // The texture's middle is the zero wavenumber, so every point picks up
    // e^(-iπ·n) = (-1)^n along each axis.
    std::vector<WaterTexel> texels(count);
    for (uint32_t z = 0; z < size; ++z)
    {
        for (uint32_t x = 0; x < size; ++x)
        {
            const size_t i = static_cast<size_t>(z) * size + x;
            const float sign = ((x + z) & 1u) == 0u ? 1.0f : -1.0f;
            texels[i] = WaterTexel{.height = fields[0][i].imag() * sign,
                                   .displacementX = fields[0][i].real() * sign,
                                   .displacementZ = fields[1][i].real() * sign,
                                   .slopeX = fields[2][i].real() * sign,
                                   .slopeZ = fields[2][i].imag() * sign,
                                   .stretchX = fields[3][i].real() * sign,
                                   .stretchZ = fields[3][i].imag() * sign,
                                   .shear = fields[1][i].imag() * sign};
        }
    }
    return texels;
}

glm::vec2 CascadeOffset(glm::dvec2 position, double tileLength)
{
    glm::dvec2 wrapped(std::fmod(position.x, tileLength), std::fmod(position.y, tileLength));
    wrapped.x += wrapped.x < 0.0 ? tileLength : 0.0;
    wrapped.y += wrapped.y < 0.0 ? tileLength : 0.0;
    glm::vec2 offset(wrapped);
    // A remainder a hair under the tile can round up to it in float.
    const float tile = static_cast<float>(tileLength);
    offset.x = offset.x >= tile ? 0.0f : offset.x;
    offset.y = offset.y >= tile ? 0.0f : offset.y;
    return offset;
}

} // namespace Assisi::Render
