/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Ssao.hpp>
#include <Assisi/Render/SsaoSettings.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{
/// A 1080p target through a 60-degree lens, near and far as the editor camera
/// has them.
constexpr float kWidth = 1920.0f;
constexpr float kHeight = 1080.0f;
constexpr float kFovDegrees = 60.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 1000.0f;

/// Relative error allowed of a distance that has been through a projection and
/// back: single-precision depth, and nothing else.
constexpr float kRoundTripTolerance = 1e-3f;

/// A pixel's worth of disagreement, for anything measured on the target.
constexpr float kPixelTolerance = 1e-2f;

glm::mat4 Projection()
{
    return glm::perspective(glm::radians(kFovDegrees), kWidth / kHeight, kNear, kFar);
}

/// Where the rasterizer puts a view-space point: through the projection to NDC,
/// then onto a target whose top row is NDC +1.
glm::vec2 RasterizedPixel(const glm::mat4 &projection, const glm::vec3 &view)
{
    const glm::vec4 clip = projection * glm::vec4(view, 1.0f);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * kWidth, (0.5f - ndc.y * 0.5f) * kHeight) - glm::vec2(0.5f);
}

float StoredDepth(const glm::mat4 &projection, const glm::vec3 &view)
{
    const glm::vec4 clip = projection * glm::vec4(view, 1.0f);
    return clip.z / clip.w;
}
} // namespace

TEST_CASE("Ambient occlusion settings are held inside their ranges, and sane ones are left alone")
{
    SsaoSettings wild;
    wild.sampleCount = 0;
    wild.radius = std::numeric_limits<float>::quiet_NaN();
    wild.strength = -1.0f;
    const SsaoSettings tamed = Sanitized(wild);
    CHECK(tamed.sampleCount == kMinSsaoSampleCount);
    CHECK(tamed.radius == doctest::Approx(kDefaultSsaoRadius));
    CHECK(tamed.strength == doctest::Approx(kMinSsaoStrength));

    wild.sampleCount = 1000;
    wild.radius = 1e6f;
    wild.strength = std::numeric_limits<float>::infinity();
    const SsaoSettings capped = Sanitized(wild);
    CHECK(capped.sampleCount == kMaxSsaoSampleCount);
    CHECK(capped.radius == doctest::Approx(kMaxSsaoRadius));
    CHECK(capped.strength == doctest::Approx(kDefaultSsaoStrength));

    CHECK(Sanitized(capped) == capped);
    CHECK(Sanitized(SsaoSettings{}) == SsaoSettings{});
}

TEST_CASE("Ambient occlusion is off until someone turns it on")
{
    // Off is the frame there was before the pass existed; a default that turned
    // it on would change every existing options file's picture without a word.
    CHECK_FALSE(SsaoSettings{}.enabled);
}

TEST_CASE("The kernel stays off the tangent plane, inside the radius, and crowds toward the point")
{
    for (uint32_t count = kMinSsaoSampleCount; count <= kMaxSsaoSampleCount; ++count)
    {
        CAPTURE(count);
        const std::vector<glm::vec3> kernel = BuildSsaoKernel(count);
        REQUIRE(kernel.size() == count);

        const float minCos = std::sqrt(1.0f - kSsaoKernelMaxSinSquared);
        float previousLength = 0.0f;
        for (const glm::vec3 &sample : kernel)
        {
            const float length = glm::length(sample);
            CHECK(sample.z / length >= minCos - 1e-6f);
            CHECK(length >= kSsaoKernelMinScale - 1e-6f);
            CHECK(length <= 1.0f + 1e-6f);
            CHECK(length >= previousLength);
            previousLength = length;
        }
        // The last sample reaches the radius, or the knob would promise more
        // reach than the pass searches.
        CHECK(glm::length(kernel.back()) == doctest::Approx(1.0f));
    }
}

TEST_CASE("The kernel is the same every time it is built")
{
    // A random kernel would differ between two builds — and between two
    // frames, if anything rebuilt it — which is the flicker the technique
    // promises not to have.
    const std::vector<glm::vec3> first = BuildSsaoKernel(kDefaultSsaoSampleCount);
    const std::vector<glm::vec3> second = BuildSsaoKernel(kDefaultSsaoSampleCount);
    CHECK(first == second);
}

TEST_CASE("The kernel's elevations are not tied to its lengths")
{
    // If the short samples were all the steep ones, a crease would be searched
    // close in straight up and far out along the ground, and nowhere else. The
    // short half and the long half each have to reach both steep and shallow.
    const std::vector<glm::vec3> kernel = BuildSsaoKernel(kMaxSsaoSampleCount);
    const std::size_t half = kernel.size() / 2;
    const auto cosine = [](const glm::vec3 &sample) { return sample.z / glm::length(sample); };
    const auto spread = [&](std::size_t first, std::size_t last)
    {
        float lo = 1.0f;
        float hi = 0.0f;
        for (std::size_t i = first; i < last; ++i)
        {
            lo = std::min(lo, cosine(kernel[i]));
            hi = std::max(hi, cosine(kernel[i]));
        }
        return hi - lo;
    };
    // Half the available range of cosines, in each half of the kernel.
    const float available = 1.0f - std::sqrt(1.0f - kSsaoKernelMaxSinSquared);
    CHECK(spread(0, half) >= 0.5f * available);
    CHECK(spread(half, kernel.size()) >= 0.5f * available);
}

TEST_CASE("Every pixel of a tile turns the kernel a different way, and the tile repeats")
{
    std::vector<float> angles;
    for (uint32_t y = 0; y < kSsaoTileSize; ++y)
    {
        for (uint32_t x = 0; x < kSsaoTileSize; ++x)
        {
            angles.push_back(SsaoTileAngle(x, y));
            CHECK(SsaoTileAngle(x + kSsaoTileSize, y) == SsaoTileAngle(x, y));
            CHECK(SsaoTileAngle(x, y + kSsaoTileSize) == SsaoTileAngle(x, y));
        }
    }
    // Sixteen distinct steps of a sixteenth of a turn: a repeated angle would
    // leave one rotation twice in every blur footprint and print as banding.
    std::sort(angles.begin(), angles.end());
    const float step = glm::two_pi<float>() / static_cast<float>(kSsaoTileRotationCount);
    for (uint32_t i = 0; i < kSsaoTileRotationCount; ++i)
    {
        CHECK(angles[i] == doctest::Approx(static_cast<float>(i) * step));
    }
}

TEST_CASE("The blur covers exactly one period of the tile along each axis")
{
    // Each residue once: the horizontal pass then sees every column of the tile,
    // the vertical pass every row, and between them every rotation exactly once.
    // A tap too many counts one rotation twice, a tap too few misses one, and
    // either way the tile shows through.
    std::array<uint32_t, kSsaoTileSize> seen{};
    for (const int32_t offset : kSsaoBlurTapOffsets)
    {
        const int32_t size = static_cast<int32_t>(kSsaoTileSize);
        ++seen[static_cast<uint32_t>(((offset % size) + size) % size)];
    }
    for (const uint32_t count : seen)
    {
        CHECK(count == 1u);
    }
}

TEST_CASE("A stored depth gives back the distance it was stored from")
{
    const glm::mat4 projection = Projection();
    const SsaoProjection terms = MakeSsaoProjection(projection);
    for (const float distance : {kNear, 0.37f, 1.0f, 7.3f, 120.0f, kFar})
    {
        CAPTURE(distance);
        const float depth = StoredDepth(projection, glm::vec3(0.0f, 0.0f, -distance));
        CHECK(SsaoLinearDepth(depth, terms) == doctest::Approx(distance).epsilon(kRoundTripTolerance));
    }
    // The depth clear is the far plane, which is what keeps the sky out of
    // every range check and every blur.
    CHECK(SsaoLinearDepth(1.0f, terms) == doctest::Approx(kFar).epsilon(kRoundTripTolerance));
}

TEST_CASE("A pixel and its distance rebuild the point that was rasterized there")
{
    const glm::mat4 projection = Projection();
    const SsaoProjection terms = MakeSsaoProjection(projection);
    const glm::vec2 size(kWidth, kHeight);

    // Above and to the left of centre on purpose: a flipped row would put this
    // below the centre, and mirror every normal rebuilt from it.
    for (const glm::vec3 &point : {glm::vec3(-1.2f, 0.8f, -4.0f), glm::vec3(3.0f, -2.5f, -9.0f),
                                   glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.02f, 0.05f, -0.3f)})
    {
        CAPTURE(point);
        const glm::vec2 pixel = RasterizedPixel(projection, point);
        CHECK(SsaoPixelOf(point, size, terms).x == doctest::Approx(pixel.x).epsilon(kPixelTolerance));
        CHECK(SsaoPixelOf(point, size, terms).y == doctest::Approx(pixel.y).epsilon(kPixelTolerance));

        const glm::vec3 rebuilt = SsaoViewPosition(pixel, -point.z, size, terms);
        CHECK(rebuilt.x == doctest::Approx(point.x).epsilon(kRoundTripTolerance));
        CHECK(rebuilt.y == doctest::Approx(point.y).epsilon(kRoundTripTolerance));
        CHECK(rebuilt.z == doctest::Approx(point.z).epsilon(kRoundTripTolerance));
    }
    // The top-left pixel is up and to the left.
    const glm::vec3 corner = SsaoViewPosition(glm::vec2(0.0f), 1.0f, size, terms);
    CHECK(corner.x < 0.0f);
    CHECK(corner.y > 0.0f);
}

TEST_CASE("The radius is held to a pixel width close to the camera and honoured further out")
{
    const SsaoProjection terms = MakeSsaoProjection(Projection());
    const float radius = kDefaultSsaoRadius;
    const auto projectedPixels = [&](float r, float distance) { return r * terms.yScale * 0.5f * kHeight / distance; };

    // A wall a hand's breadth away: the knob's radius would span thousands of
    // pixels, and every sample would miss the cache.
    const float near = 0.2f;
    REQUIRE(projectedPixels(radius, near) > kSsaoMaxRadiusPixels);
    CHECK(projectedPixels(SsaoEffectiveRadius(radius, near, kHeight, terms), near) ==
          doctest::Approx(kSsaoMaxRadiusPixels));

    // Far enough that the radius fits: untouched.
    const float far = 20.0f;
    REQUIRE(projectedPixels(radius, far) < kSsaoMaxRadiusPixels);
    CHECK(SsaoEffectiveRadius(radius, far, kHeight, terms) == doctest::Approx(radius));

    // Never larger than asked, and never shrinking as the surface recedes.
    float previous = 0.0f;
    for (float distance = 0.05f; distance < 50.0f; distance *= 1.5f)
    {
        const float effective = SsaoEffectiveRadius(radius, distance, kHeight, terms);
        CHECK(effective <= radius);
        CHECK(effective >= previous);
        previous = effective;
    }
}

TEST_CASE("An occluder inside the radius counts fully, and one far behind a silhouette hardly at all")
{
    const float radius = 0.5f;
    CHECK(SsaoRangeWeight(radius, 0.0f) == doctest::Approx(1.0f));
    CHECK(SsaoRangeWeight(radius, 0.4f) == doctest::Approx(1.0f));
    CHECK(SsaoRangeWeight(radius, -0.4f) == doctest::Approx(1.0f));
    // A pillar five metres in front of the ground behind it.
    CHECK(SsaoRangeWeight(radius, 5.0f) < 0.05f);

    float previous = 1.0f;
    for (float delta = 0.0f; delta < 10.0f; delta += 0.1f)
    {
        const float weight = SsaoRangeWeight(radius, delta);
        CHECK(weight <= previous + 1e-6f);
        previous = weight;
    }
}

TEST_CASE("The blur keeps to the surface it is blurring")
{
    CHECK(SsaoBlurWeight(10.0f, 10.0f) == doctest::Approx(1.0f));
    // Neighbouring pixels on one surface differ by a sliver of the distance.
    CHECK(SsaoBlurWeight(10.0f, 10.05f) > 0.8f);
    // A silhouette: the next pixel is a wall two metres behind.
    CHECK(SsaoBlurWeight(10.0f, 12.0f) == doctest::Approx(0.0f));
    CHECK(SsaoBlurWeight(10.0f, 8.0f) == doctest::Approx(0.0f));
    // Relative, so the same step reads the same at every distance.
    CHECK(SsaoBlurWeight(100.0f, 100.5f) == doctest::Approx(SsaoBlurWeight(10.0f, 10.05f)));
}

TEST_CASE("Specular occlusion is nothing at full visibility and everything at none")
{
    for (float nDotV = 0.0f; nDotV <= 1.0f; nDotV += 0.125f)
    {
        for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.125f)
        {
            CAPTURE(nDotV);
            CAPTURE(alpha);
            // Exactly one: this is what makes the knob-off frame the frame
            // there was before.
            CHECK(SpecularOcclusion(nDotV, 1.0f, alpha) == 1.0f);
            CHECK(SpecularOcclusion(nDotV, 0.0f, alpha) == 0.0f);

            float previous = 0.0f;
            for (float ao = 0.0f; ao <= 1.0f; ao += 0.0625f)
            {
                const float occlusion = SpecularOcclusion(nDotV, ao, alpha);
                CHECK(occlusion >= previous);
                CHECK(occlusion <= 1.0f);
                previous = occlusion;
            }
        }
    }
}

TEST_CASE("A sharp reflection looking straight out of a crease keeps more than a rough one")
{
    const float ao = 0.5f;
    CHECK(SpecularOcclusion(1.0f, ao, 0.01f) > SpecularOcclusion(1.0f, ao, 1.0f));
    // A rough lobe takes nearly the diffuse answer.
    CHECK(SpecularOcclusion(1.0f, ao, 1.0f) == doctest::Approx(ao).epsilon(0.05));
}
