/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/EnvironmentSettings.hpp>
#include <Assisi/Render/Sky.hpp>
#include <Assisi/Render/SkyProbeInputs.hpp>
#include <Assisi/Render/SpecularIbl.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{
/// How far a unit vector, a dot product of two, or a texture coordinate may be
/// from exact and still count as the same: float rounding through a matrix
/// inverse or a normalise, nothing more.
constexpr float kRoundingTolerance = 1e-5f;

/// Relative error allowed of a mean over samples of a constant, which should
/// return the constant to within rounding of the running sum.
constexpr float kConstantMeanTolerance = 1e-4f;

/// Relative disagreement allowed between two estimates of the same lobe taken
/// with differently rotated sample sets. Both use 1024 samples of a smooth
/// environment, which lands well inside this.
constexpr float kRotatedEstimateTolerance = 0.01f;

/// Absolute disagreement allowed between the importance-sampled split sum and
/// the gridded one. The larger error is the sampled estimate's at a grazing
/// view of a fairly smooth lobe, about 0.02 at the table's sample count.
constexpr float kQuadratureTolerance = 0.025f;

/// Polar steps of the reference grid; the azimuth takes four times as many.
constexpr int32_t kQuadratureSteps = 400;

/// The smoothest a material gets: mesh.frag clamps roughness here.
constexpr float kMinIblTestRoughness = 0.04f;

glm::vec3 Dir(float elevationDegrees, float azimuthDegrees)
{
    const float el = glm::radians(elevationDegrees);
    const float az = glm::radians(azimuthDegrees);
    return glm::vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
}

std::vector<glm::vec3> Directions()
{
    std::vector<glm::vec3> directions;
    for (int32_t elevation = -90; elevation <= 90; elevation += 15)
    {
        for (int32_t azimuth = 0; azimuth < 360; azimuth += 40)
        {
            directions.push_back(Dir(static_cast<float>(elevation), static_cast<float>(azimuth)));
        }
    }
    return directions;
}

float Luminance(const glm::vec3 &linear)
{
    return glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

/// A GGX lobe's (A, B) by brute force: the BRDF written out term by term —
/// distribution, @p smithG1's masking for each direction, Schlick's Fresnel —
/// integrated over a midpoint grid of light directions with no importance
/// sampling at all. A + B is the lobe's directional albedo at F0 = 1.
template <typename SmithG1>
glm::vec2 QuadratureLobe(float nDotV, float roughness, int32_t polarSteps, const SmithG1 &smithG1)
{
    const float alpha = roughness * roughness;
    const float a2 = alpha * alpha;
    const glm::vec3 view(std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV);
    const int32_t azimuthSteps = 4 * polarSteps;
    const double dTheta = glm::half_pi<double>() / polarSteps;
    const double dPhi = glm::two_pi<double>() / azimuthSteps;

    double a = 0.0;
    double b = 0.0;
    for (int32_t i = 0; i < polarSteps; ++i)
    {
        const double theta = (i + 0.5) * dTheta;
        for (int32_t j = 0; j < azimuthSteps; ++j)
        {
            const double phi = (j + 0.5) * dPhi;
            const glm::vec3 light(static_cast<float>(std::sin(theta) * std::cos(phi)),
                                  static_cast<float>(std::sin(theta) * std::sin(phi)),
                                  static_cast<float>(std::cos(theta)));
            const glm::vec3 half = glm::normalize(light + view);
            const float vDotH = glm::dot(view, half);
            const float d = half.z * half.z * (a2 - 1.0f) + 1.0f;
            const float distribution = a2 / (glm::pi<float>() * d * d);
            const float masking = smithG1(nDotV) * smithG1(light.z);
            const float fresnel = std::pow(1.0f - vDotH, kSchlickExponent);

            // f * (n.l) with F factored out, over the solid angle of this cell.
            const double brdf = static_cast<double>(distribution * masking / (4.0f * nDotV));
            const double weight = brdf * std::sin(theta) * dTheta * dPhi;
            a += static_cast<double>(1.0f - fresnel) * weight;
            b += static_cast<double>(fresnel) * weight;
        }
    }
    return glm::vec2(static_cast<float>(a), static_cast<float>(b));
}

glm::vec2 QuadratureEnvBrdf(float nDotV, float roughness, int32_t polarSteps)
{
    const float alpha = roughness * roughness;
    return QuadratureLobe(nDotV, roughness, polarSteps, [alpha](float nDotX) { return IblSmithG1(nDotX, alpha); });
}

float QuadratureDirectAlbedo(float nDotV, float roughness, int32_t polarSteps)
{
    const glm::vec2 ab = QuadratureLobe(nDotV, roughness, polarSteps,
                                        [roughness](float nDotX) { return DirectSmithG1(nDotX, roughness); });
    return ab.x + ab.y;
}

/// Which face and texel Vulkan's cube sampling reads for @p direction: the
/// major axis picks the face, and the two remaining components, divided by it,
/// are the texel. Transcribed from the specification's face-selection table,
/// independently of the table SpecularIbl.hpp carries.
struct CubeTexel
{
    uint32_t face;
    glm::vec2 uv;
};

CubeTexel SelectCubeTexel(const glm::vec3 &r)
{
    const glm::vec3 a = glm::abs(r);
    uint32_t face = 0;
    float sc = 0.0f;
    float tc = 0.0f;
    float ma = 0.0f;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = r.x > 0.0f ? 0u : 1u;
        sc = r.x > 0.0f ? -r.z : r.z;
        tc = -r.y;
        ma = a.x;
    }
    else if (a.y >= a.z)
    {
        face = r.y > 0.0f ? 2u : 3u;
        sc = r.x;
        tc = r.y > 0.0f ? r.z : -r.z;
        ma = a.y;
    }
    else
    {
        face = r.z > 0.0f ? 4u : 5u;
        sc = r.z > 0.0f ? r.x : -r.x;
        tc = -r.y;
        ma = a.z;
    }
    return CubeTexel{face, glm::vec2(0.5f * (sc / ma + 1.0f), 0.5f * (tc / ma + 1.0f))};
}

glm::vec3 Turned(const glm::vec3 &direction, float degrees)
{
    const glm::vec3 axis = glm::normalize(glm::cross(direction, glm::vec3(0.0f, 0.0f, 1.0f)));
    return glm::normalize(glm::angleAxis(glm::radians(degrees), axis) * direction);
}

SkySun SunAt(float elevationDegrees, float azimuthDegrees)
{
    return SkySun{.directionToSun = Dir(elevationDegrees, azimuthDegrees), .color = glm::vec3(1.0f), .intensity = 1.0f};
}
} // namespace

TEST_CASE("The radical inverse walks the van der Corput sequence")
{
    // The order the shader's bitfieldReverse produces. A sequence that merely
    // covered [0, 1) would still integrate, but the GPU and the CPU would take
    // different samples and the reference would stop being one.
    CHECK(RadicalInverse(0u) == 0.0f);
    CHECK(RadicalInverse(1u) == doctest::Approx(0.5f));
    CHECK(RadicalInverse(2u) == doctest::Approx(0.25f));
    CHECK(RadicalInverse(3u) == doctest::Approx(0.75f));
    CHECK(RadicalInverse(4u) == doctest::Approx(0.125f));
    CHECK(RadicalInverse(0xFFFFFFFFu) < 1.0f);
}

TEST_CASE("A GGX half vector is unit length, faces the normal, and is the normal at zero roughness")
{
    for (const glm::vec3 &normal : Directions())
    {
        for (uint32_t i = 0; i < 64u; ++i)
        {
            const glm::vec2 xi = Hammersley(i, 64u);
            const glm::vec3 half = ImportanceSampleGgx(xi, 0.6f, normal);
            CHECK(glm::length(half) == doctest::Approx(1.0f).epsilon(kRoundingTolerance));
            CHECK(glm::dot(half, normal) >= 0.0f);

            const glm::vec3 mirror = ImportanceSampleGgx(xi, 0.0f, normal);
            CHECK(glm::dot(mirror, normal) == doctest::Approx(1.0f).epsilon(kRoundingTolerance));
        }
    }
}

TEST_CASE("The environment's Smith term is the one with k = alpha / 2")
{
    // The per-light BRDF's k = (r + 1)^2 / 8 gives 0.733 here. The quadrature
    // tests below share each lobe's Smith term with the code they check, so
    // these values are what pins which term each lobe has.
    CHECK(IblSmithG1(0.5f, 0.5f) == doctest::Approx(0.8f));
    CHECK(IblSmithG1(1.0f, 0.3f) == doctest::Approx(1.0f));
}

TEST_CASE("The per-light Smith term is CookTorrance's, k = (roughness + 1)^2 / 8")
{
    // At roughness 0.5, k = 1.5^2 / 8.
    constexpr float kExpectedK = 0.28125f;
    CHECK(DirectSmithG1(0.5f, 0.5f) == doctest::Approx(0.5f / (0.5f * (1.0f - kExpectedK) + kExpectedK)));
    CHECK(DirectSmithG1(1.0f, 0.8f) == doctest::Approx(1.0f));
}

TEST_CASE("The per-light lobe's albedo is the integral it claims to be")
{
    // What CookTorrance's multi-scatter compensation divides by. The same grid
    // as the split sum's check, with the per-light masking in place.
    float worst = 0.0f;
    for (const float roughness : {0.3f, 0.5f, 0.7f, 1.0f})
    {
        for (const float nDotV : {0.1f, 0.4f, 0.7f, 1.0f})
        {
            const float sampled = IntegrateDirectAlbedo(nDotV, roughness, kBrdfLutSampleCount);
            const float gridded = QuadratureDirectAlbedo(nDotV, roughness, kQuadratureSteps);
            worst = std::max(worst, std::abs(sampled - gridded));
        }
    }
    CHECK(worst < kQuadratureTolerance);
}

TEST_CASE("The split-sum table is the integral it claims to be")
{
    // A second route to the same number: the BRDF written out term by term and
    // integrated over a grid of light directions, with nothing importance-
    // sampled. The two share only the Smith term, which the test above pins.
    // Held off the mirror end, where the lobe is too narrow for a grid.
    float worst = 0.0f;
    for (const float roughness : {0.3f, 0.5f, 0.7f, 1.0f})
    {
        for (const float nDotV : {0.1f, 0.4f, 0.7f, 1.0f})
        {
            const glm::vec2 sampled = IntegrateEnvBrdf(nDotV, roughness, kBrdfLutSampleCount);
            const glm::vec2 gridded = QuadratureEnvBrdf(nDotV, roughness, kQuadratureSteps);
            worst = std::max({worst, std::abs(sampled.x - gridded.x), std::abs(sampled.y - gridded.y)});
        }
    }
    CHECK(worst < kQuadratureTolerance);
}

TEST_CASE("The split-sum table never reflects more than arrives, and a mirror reflects nearly all of it")
{
    // What a 256-sample estimate may overshoot a true bound by.
    constexpr float kEstimateOvershoot = 1e-3f;
    for (float roughness = 0.0f; roughness <= 1.0f; roughness += 0.125f)
    {
        for (float nDotV = 0.0f; nDotV <= 1.0f; nDotV += 0.125f)
        {
            const glm::vec2 ab = IntegrateEnvBrdf(nDotV, roughness, 256u);
            CHECK(ab.x >= 0.0f);
            CHECK(ab.y >= 0.0f);
            CHECK(ab.x + ab.y <= 1.0f + kEstimateOvershoot);
        }
    }

    // Seen head on, a smooth surface's lobe is all inside the hemisphere and
    // barely masked, so at F0 = 1 it returns nearly what it receives — and the
    // Fresnel bias, which only a grazing half vector earns, is nearly nothing.
    constexpr float kMirrorAlbedoFloor = 0.97f;
    constexpr float kHeadOnBiasCeiling = 0.01f;
    const glm::vec2 mirror = IntegrateEnvBrdf(1.0f, kMinIblTestRoughness, 256u);
    CHECK(mirror.x + mirror.y > kMirrorAlbedoFloor);
    CHECK(mirror.y < kHeadOnBiasCeiling);
}

TEST_CASE("The BRDF table is laid out n.v across and roughness down, at texel centres")
{
    // mesh.frag reads it at vec2(n.v, roughness). The two axes are not
    // interchangeable — the bias grows toward grazing views and the scale falls
    // with roughness — so a transposed table reads as a plausible wrong answer.
    constexpr uint32_t size = 8;
    constexpr uint32_t samples = 64;
    const std::vector<BrdfTableTexel> table = BuildBrdfLut(size, samples);
    REQUIRE(table.size() == size * size);

    const uint32_t column = 1;
    const uint32_t row = 6;
    const float nDotV = (column + 0.5f) / size;
    const float roughness = (row + 0.5f) / size;
    const BrdfTableTexel &texel = table[row * size + column];
    const glm::vec2 expected = IntegrateEnvBrdf(nDotV, roughness, samples);
    CHECK(texel.environment.x == doctest::Approx(expected.x));
    CHECK(texel.environment.y == doctest::Approx(expected.y));
    CHECK(texel.directAlbedo == doctest::Approx(IntegrateDirectAlbedo(nDotV, roughness, samples)));

    // How far apart the two orientations are at this texel, well clear of the
    // estimate's own noise, so a transposed table cannot pass by accident.
    constexpr float kTransposedGap = 0.05f;
    const glm::vec2 transposed = IntegrateEnvBrdf(roughness, nDotV, samples);
    CHECK(std::abs(transposed.x - expected.x) > kTransposedGap);
}

TEST_CASE("A uniform environment prefilters to itself at every roughness")
{
    // The estimator is a weighted mean, so a constant goes through unchanged
    // however wide the lobe. A missing or wrong normalisation shows here first.
    const glm::vec3 constant(0.3f, 0.5f, 0.9f);
    const auto uniform = [&](const glm::vec3 &) { return constant; };

    for (const float roughness : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
    {
        for (const glm::vec3 &reflection : Directions())
        {
            const glm::vec3 filtered = PrefilterGgx(uniform, reflection, roughness, 128u);
            CHECK(filtered.r == doctest::Approx(constant.r).epsilon(kConstantMeanTolerance));
            CHECK(filtered.b == doctest::Approx(constant.b).epsilon(kConstantMeanTolerance));
        }
    }
}

TEST_CASE("The lobe is the same shape whichever way the reflection points")
{
    // An environment symmetric about one axis: any two reflections at the same
    // angle to it must filter to the same value. A tangent frame that is not
    // orthonormal, or that degenerates near the helper axis, stretches the lobe
    // differently for different reflections and breaks the symmetry.
    const glm::vec3 axis(0.0f, 1.0f, 0.0f);
    // How sharply the environment peaks along its axis: enough that a lobe
    // stretched the wrong way reads a visibly different mean.
    constexpr float kPeakSharpness = 4.0f;
    const auto peaked = [&](const glm::vec3 &d) { return glm::vec3(std::exp(kPeakSharpness * glm::dot(d, axis))); };

    for (const float roughness : {0.3f, 0.7f})
    {
        const float reference = PrefilterGgx(peaked, Dir(50.0f, 0.0f), roughness, 1024u).r;
        for (const float azimuth : {35.0f, 90.0f, 160.0f, 270.0f})
        {
            const float turned = PrefilterGgx(peaked, Dir(50.0f, azimuth), roughness, 1024u).r;
            CHECK(turned == doctest::Approx(reference).epsilon(kRotatedEstimateTolerance));
        }
    }

    // Straight along +Z, where the frame switches helper axis.
    const auto aroundZ = [](const glm::vec3 &d) { return glm::vec3(std::exp(kPeakSharpness * d.z)); };
    const float onAxis = PrefilterGgx(aroundZ, glm::vec3(0.0f, 0.0f, 1.0f), 0.5f, 1024u).r;
    const float nearAxis = PrefilterGgx(aroundZ, glm::normalize(glm::vec3(0.0f, 0.03f, 1.0f)), 0.5f, 1024u).r;
    CHECK(onAxis == doctest::Approx(nearAxis).epsilon(kRotatedEstimateTolerance));
}

TEST_CASE("Blurring a sky toward its sun dims the glow around it")
{
    // Rougher surfaces reflect a wider patch of sky, and the brightest part of a
    // disk-less sky is the aureole the lobe is centred on — so every step of
    // roughness trades some of it for the dimmer sky around.
    const SkyProbeInputs inputs = MakeSkyProbeInputs(SunAt(12.0f, 30.0f), SkyMoon{}, SkySettings{});
    const auto sky = [&](const glm::vec3 &d) { return SkyRadiance(d, inputs.sun, inputs.moon, inputs.settings); };

    const glm::vec3 towardSun = inputs.sun.directionToSun;
    const float mirror = Luminance(PrefilterGgx(sky, towardSun, 0.0f, 1u));
    const float satin = Luminance(PrefilterGgx(sky, towardSun, 0.5f, 1024u));
    const float matte = Luminance(PrefilterGgx(sky, towardSun, 1.0f, 1024u));
    CHECK(mirror > satin);
    CHECK(satin > matte);
    CHECK(matte > 0.0f);
}

TEST_CASE("Each face's capture matrix draws the directions the sampler reads back")
{
    // The capture rasterizes the sky through these matrices; the sampler and
    // the prefilter read the result through CubeFaceDirection. A face drawn
    // mirrored or upside down is a plausible sky everywhere except at its
    // seams, which is exactly what nothing else would catch.
    //
    // The backend flips the viewport, so a framebuffer's first row is NDC
    // y = +1: the texel at (u, v) is rasterized at NDC (2u - 1, 1 - 2v).
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        const glm::mat4 inverseViewProjection = glm::inverse(CubeFaceViewProjection(face));
        for (const float u : {0.1f, 0.5f, 0.9f})
        {
            for (const float v : {0.1f, 0.5f, 0.9f})
            {
                const glm::vec4 far = inverseViewProjection * glm::vec4(2.0f * u - 1.0f, 1.0f - 2.0f * v, 1.0f, 1.0f);
                const glm::vec3 drawn = glm::normalize(glm::vec3(far) / far.w);
                const glm::vec3 sampled = CubeFaceDirection(face, glm::vec2(u, v));
                CHECK(glm::dot(drawn, sampled) == doctest::Approx(1.0f).epsilon(kRoundingTolerance));
            }
        }
    }
}

TEST_CASE("The face table is Vulkan's: every texel direction selects its own face and texel")
{
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        for (const float u : {0.1f, 0.3f, 0.5f, 0.8f})
        {
            for (const float v : {0.2f, 0.5f, 0.9f})
            {
                const CubeTexel texel = SelectCubeTexel(CubeFaceDirection(face, glm::vec2(u, v)));
                CHECK(texel.face == face);
                CHECK(texel.uv.x == doctest::Approx(u).epsilon(kRoundingTolerance));
                CHECK(texel.uv.y == doctest::Approx(v).epsilon(kRoundingTolerance));
            }
        }
    }
}

TEST_CASE("The mip chain maps onto roughness end to end, with the work spread evenly")
{
    CHECK(SkyProbeMipCount(128u) == 5u);
    CHECK(SkyProbeMipCount(16u) == 2u);
    CHECK(SkyProbeMipCount(512u) == 7u);

    CHECK(PrefilterRoughness(0u, 5u) == 0.0f);
    CHECK(PrefilterRoughness(4u, 5u) == doctest::Approx(1.0f));
    CHECK(PrefilterRoughness(2u, 5u) == doctest::Approx(0.5f));

    // The mirror mip copies; each mip after it has a quarter of the texels and
    // four times the samples, up to the cap.
    CHECK(PrefilterSampleCount(0u, 64u) == 1u);
    CHECK(PrefilterSampleCount(1u, 64u) == 64u);
    CHECK(PrefilterSampleCount(2u, 64u) == 256u);
    CHECK(PrefilterSampleCount(3u, 64u) == 1024u);
    CHECK(PrefilterSampleCount(6u, 64u) == kMaxPrefilterSampleCount);
    CHECK(PrefilterSampleCount(30u, kMaxProbeSampleCount) == kMaxPrefilterSampleCount);
}

TEST_CASE("A probe's inputs carry no disk and nothing a shader cannot read")
{
    SkySettings settings;
    settings.sunDiskIntensity = 50.0f;
    settings.exposure = std::numeric_limits<float>::quiet_NaN();
    SkyMoon moon;
    moon.intensity = 0.02f;
    moon.diskIntensity = 3.0f;

    const SkyProbeInputs inputs = MakeSkyProbeInputs(SunAt(30.0f, 0.0f), moon, settings);
    CHECK(inputs.settings.sunDiskIntensity == 0.0f);
    CHECK(inputs.moon.diskIntensity == 0.0f);
    CHECK(inputs.settings.exposure == doctest::Approx(kDefaultSkyExposure));
    // What the disks do not own survives.
    CHECK(inputs.moon.intensity == doctest::Approx(0.02f));
}

TEST_CASE("The probe bakes again when the sky moves past its tolerance, and not before")
{
    SkyMoon moon;
    moon.directionToMoon = Dir(40.0f, 200.0f);
    moon.intensity = 0.02f;
    const SkyProbeInputs baked = MakeSkyProbeInputs(SunAt(30.0f, 10.0f), moon, SkySettings{});

    CHECK_FALSE(SkyProbeInputsDiffer(baked, baked, 0.5f));
    CHECK_FALSE(SkyProbeInputsDiffer(baked, baked, 0.0f));

    SkyProbeInputs nudged = baked;
    nudged.sun.directionToSun = Turned(baked.sun.directionToSun, 0.4f);
    CHECK_FALSE(SkyProbeInputsDiffer(baked, nudged, 0.5f));
    nudged.sun.directionToSun = Turned(baked.sun.directionToSun, 0.6f);
    CHECK(SkyProbeInputsDiffer(baked, nudged, 0.5f));

    // Zero is the exact baseline: any movement at all.
    nudged.sun.directionToSun = Turned(baked.sun.directionToSun, 0.01f);
    CHECK(SkyProbeInputsDiffer(baked, nudged, 0.0f));

    SkyProbeInputs moonMoved = baked;
    moonMoved.moon.directionToMoon = Turned(baked.moon.directionToMoon, 0.6f);
    CHECK(SkyProbeInputsDiffer(baked, moonMoved, 0.5f));

    SkyProbeInputs dimmed = baked;
    dimmed.sun.intensity = baked.sun.intensity * 1.01f;
    CHECK_FALSE(SkyProbeInputsDiffer(baked, dimmed, 0.5f));
    dimmed.sun.intensity = baked.sun.intensity * 1.05f;
    CHECK(SkyProbeInputsDiffer(baked, dimmed, 0.5f));

    // An edit to the air is an edit, whatever the tolerance.
    SkyProbeInputs edited = baked;
    edited.settings.exposure = baked.settings.exposure * 1.001f;
    CHECK(SkyProbeInputsDiffer(baked, edited, 0.5f));
    edited = baked;
    edited.settings.groundColor.g += 0.01f;
    CHECK(SkyProbeInputsDiffer(baked, edited, 0.5f));

    // What only a disk reads is not in the probe, so it bakes nothing.
    SkyProbeInputs diskOnly = baked;
    diskOnly.settings.sunSizeDegrees = baked.settings.sunSizeDegrees * 2.0f;
    diskOnly.moon.imageUp = -baked.moon.imageUp;
    CHECK_FALSE(SkyProbeInputsDiffer(baked, diskOnly, 0.0f));
}

TEST_CASE("Environment settings are held inside their ranges")
{
    EnvironmentSettings settings;
    settings.resolution = 100;
    CHECK(Sanitized(settings).resolution == 128u);
    settings.resolution = 8;
    CHECK(Sanitized(settings).resolution == kMinProbeResolution);
    settings.resolution = 4096;
    CHECK(Sanitized(settings).resolution == kMaxProbeResolution);

    settings.sampleCount = 1;
    CHECK(Sanitized(settings).sampleCount == kMinProbeSampleCount);
    settings.sampleCount = 100000;
    CHECK(Sanitized(settings).sampleCount == kMaxProbeSampleCount);

    settings.rebakeDegrees = -3.0f;
    CHECK(Sanitized(settings).rebakeDegrees == 0.0f);
    settings.rebakeDegrees = 90.0f;
    CHECK(Sanitized(settings).rebakeDegrees == kMaxProbeRebakeDegrees);
    settings.rebakeDegrees = std::numeric_limits<float>::quiet_NaN();
    CHECK(Sanitized(settings).rebakeDegrees == kDefaultProbeRebakeDegrees);

    const EnvironmentSettings once = Sanitized(settings);
    CHECK(Sanitized(once) == once);
    // Every mip halves exactly only on a power of two.
    CHECK(std::has_single_bit(once.resolution));
}
