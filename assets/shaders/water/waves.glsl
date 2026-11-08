// The wave spectrum and the fields one wave adds to. Render::Water.hpp is the
// specification and its tests hold it; this transcribes it.

const float kPi    = 3.14159265359;
const float kTwoPi = 6.28318530718;

// Must match Render::kWaterGravity, kWaterFftSize and kWaterLoopSeconds.
const float kWaterGravity     = 9.81;
const uint  kWaterFftSize     = 256u;
const uint  kWaterFftLog2     = 8u;
const float kWaterLoopSeconds = 1024.0;
const uint  kWaterCascadeCount = 4u;

// JONSWAP, as Horvath gives it.
const float kJonswapRise            = 1.25;
const float kJonswapPeakEnhancement = 3.3;
const float kJonswapWidthBelow      = 0.07;
const float kJonswapWidthAbove      = 0.09;
const float kJonswapLowestShare     = 0.1;

// Hasselmann's spread, as Horvath fits it.
const float kSpreadBelowScale         = 6.97;
const float kSpreadBelowExponent      = 4.06;
const float kSpreadAboveScale         = 9.77;
const float kSpreadAboveExponent      = -2.33;
const float kSpreadAboveYouthExponent = -1.45;
const float kSpreadSwellScale         = 16.0;
const float kMaxSpreadExponent        = 64.0;

// Γ(s + 1)/Γ(s + ½) by its asymptotic series from here up.
const float kGammaRatioSeriesStart = 8.0;
const float kGammaRatioTerm1       = 1.0 / 8.0;
const float kGammaRatioTerm2       = 1.0 / 128.0;
const float kGammaRatioTerm3       = -5.0 / 1024.0;
const float kGammaRatioTerm4       = -21.0 / 32768.0;

// The draws' second key, and how a hash becomes a uniform number.
const uint  kSecondDrawKey = 0x9E3779B9u;
const uint  kUniformShift  = 8u;
const float kUniformScale  = 1.0 / 16777216.0;
const float kUniformCentre = 0.5;

// One JONSWAP spectrum: x = peak frequency, y = alpha, z = youth, w = bearing
// in radians; and its spread and swell, as Render::WaveSpectrum holds them.
struct WaveShape
{
    vec4  peakAlphaYouthBearing;
    float spread;
    float swell;
};

vec2 ComplexMultiply(vec2 a, vec2 b)
{
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 TimesI(vec2 a)
{
    return vec2(-a.y, a.x);
}

uint WaterHash(uint value)
{
    // Jarzynski and Olano, "Hash Functions for GPU Rendering" (2020).
    uint state = value * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec2 GaussianPair(uint seed, uint cascade, ivec2 frequency)
{
    uint  key    = WaterHash(seed ^ WaterHash(cascade ^ WaterHash(uint(frequency.x) ^ WaterHash(uint(frequency.y)))));
    float first  = (float(WaterHash(key) >> kUniformShift) + kUniformCentre) * kUniformScale;
    float second = float(WaterHash(key ^ kSecondDrawKey) >> kUniformShift) * kUniformScale;
    float radius = sqrt(-2.0 * log(first));
    float angle  = kTwoPi * second;
    return vec2(radius * cos(angle), radius * sin(angle));
}

float JonswapSpectrum(float omega, float peak, float alpha)
{
    if (peak <= 0.0 || alpha <= 0.0 || omega < peak * kJonswapLowestShare)
    {
        return 0.0;
    }
    float width       = omega <= peak ? kJonswapWidthBelow : kJonswapWidthAbove;
    float offPeak     = (omega - peak) / (width * peak);
    float enhancement = pow(kJonswapPeakEnhancement, exp(-0.5 * offPeak * offPeak));
    float ratio       = peak / omega;
    float ratio2      = ratio * ratio;
    float omega2      = omega * omega;
    return alpha * kWaterGravity * kWaterGravity / (omega2 * omega2 * omega) * exp(-kJonswapRise * ratio2 * ratio2) *
           enhancement;
}

float SpreadExponent(float omega, WaveShape shape)
{
    float peak = shape.peakAlphaYouthBearing.x;
    if (omega <= 0.0 || peak <= 0.0)
    {
        return 0.0;
    }
    float ratio    = omega / peak;
    float exponent = ratio <= 1.0
                         ? kSpreadBelowScale * pow(ratio, kSpreadBelowExponent)
                         : kSpreadAboveScale *
                               pow(ratio, kSpreadAboveExponent + kSpreadAboveYouthExponent * shape.peakAlphaYouthBearing.z);
    exponent += kSpreadSwellScale * tanh(peak / omega) * shape.swell * shape.swell;
    return clamp(exponent, 0.0, kMaxSpreadExponent);
}

float GammaRatio(float s)
{
    float x     = s;
    uint  steps = 0u;
    while (x < kGammaRatioSeriesStart)
    {
        x += 1.0;
        ++steps;
    }
    float inverse = 1.0 / x;
    float ratio   = sqrt(x) * (1.0 + inverse * (kGammaRatioTerm1 +
                                              inverse * (kGammaRatioTerm2 + inverse * (kGammaRatioTerm3 +
                                                                                       inverse * kGammaRatioTerm4))));
    for (uint i = 0u; i < steps; ++i)
    {
        ratio *= (x - 0.5) / x;
        x -= 1.0;
    }
    return ratio;
}

float DirectionalSpread(float theta, float exponent, float spread)
{
    float even   = 1.0 / kTwoPi;
    float narrow = exponent > 0.0
                       ? GammaRatio(exponent) / (2.0 * sqrt(kPi)) * pow(abs(cos(0.5 * theta)), 2.0 * exponent)
                       : even;
    return narrow + (even - narrow) * spread;
}

float DeepWaterFrequency(float k)
{
    return sqrt(kWaterGravity * max(k, 0.0));
}

float LoopedFrequency(float omega)
{
    float turn = kTwoPi / kWaterLoopSeconds;
    return floor(omega / turn) * turn;
}

// S(ω)·D(θ)·(dω/dk)/k: the shape's energy per unit wavenumber area at k.
float WavenumberSpectrum(WaveShape shape, vec2 k, float kLength)
{
    float peak = shape.peakAlphaYouthBearing.x;
    if (peak <= 0.0)
    {
        return 0.0;
    }
    float omega   = DeepWaterFrequency(kLength);
    float bearing = shape.peakAlphaYouthBearing.w;
    vec2  toward  = vec2(sin(bearing), -cos(bearing));
    float theta   = atan(toward.x * k.y - toward.y * k.x, dot(toward, k));
    float omegaPerK = kWaterGravity / (2.0 * omega);
    return JonswapSpectrum(omega, peak, shape.peakAlphaYouthBearing.y) *
           DirectionalSpread(theta, SpreadExponent(omega, shape), shape.spread) * omegaPerK / kLength;
}

// The four packed fields one wave of complex height @p height adds to:
// x displacement + i·height, z displacement + i·shear, slope x + i·slope z,
// stretch x + i·stretch z.
void WaveFields(vec2 height, vec2 k, float choppiness, out vec4 fields01, out vec4 fields23)
{
    float kLength = length(k);
    if (kLength <= 0.0)
    {
        fields01 = vec4(0.0);
        fields23 = vec4(0.0);
        return;
    }
    vec2 iHeight       = TimesI(height);
    vec2 displacementX = iHeight * (choppiness * k.x / kLength);
    vec2 displacementZ = iHeight * (choppiness * k.y / kLength);
    vec2 slopeX        = iHeight * k.x;
    vec2 slopeZ        = iHeight * k.y;
    vec2 stretchX      = TimesI(displacementX) * k.x;
    vec2 stretchZ      = TimesI(displacementZ) * k.y;
    vec2 shear         = TimesI(displacementX) * k.y;
    fields01 = vec4(displacementX + iHeight, displacementZ + TimesI(shear));
    fields23 = vec4(slopeX + TimesI(slopeZ), stretchX + TimesI(stretchZ));
}
