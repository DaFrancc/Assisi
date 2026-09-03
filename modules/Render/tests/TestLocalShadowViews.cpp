/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

#include <array>
#include <cmath>
#include <cstdint>

using namespace Assisi::Render;

namespace
{
constexpr std::uint32_t kAtlas = 4096;

ShadowViewRect TileAt(std::uint32_t x, std::uint32_t y, std::uint32_t size)
{
    return ShadowViewRect{.x = x, .y = y, .width = size, .height = size};
}

/// Where @p world lands in @p view's clip space, as NDC, plus whether it is in
/// front of the near plane at all.
struct Projected
{
    glm::vec3 ndc{0.f};
    bool inFront = false;

    /// Whether the point is inside this view's frustum, which is what decides
    /// whether the map holds anything about it.
    [[nodiscard]] bool Inside() const
    {
        return inFront && std::abs(ndc.x) <= 1.f && std::abs(ndc.y) <= 1.f && ndc.z >= 0.f && ndc.z <= 1.f;
    }
};

Projected Project(const ShadowView &view, const glm::vec3 &world)
{
    const glm::vec4 clip = view.viewProjection * glm::vec4(world, 1.f);
    Projected result;
    result.inFront = clip.w > 0.f;
    if (result.inFront)
    {
        result.ndc = glm::vec3(clip) / clip.w;
    }
    return result;
}

/// The six views a point light at the origin gets, all sharing one tile — the
/// rect is irrelevant to what a face can see, and reusing it keeps the tests
/// about the frusta.
std::array<ShadowView, kPointLightFaceCount> PointFaces(float range, const LocalShadowSettings &settings)
{
    std::array<ShadowView, kPointLightFaceCount> faces{};
    for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
    {
        faces[face] = PointFaceShadowView(glm::vec3(0.f), range, face, TileAt(0, 0, 512), kAtlas, settings);
    }
    return faces;
}
} // namespace

TEST_CASE("A point light's six faces see every direction around it")
{
    const LocalShadowSettings settings;
    const auto faces = PointFaces(10.f, settings);

    // Sampled over the whole sphere rather than on the axes: the axes are the
    // easy case, and what a cube map gets wrong is the directions between them.
    std::uint32_t sampled = 0;
    for (int32_t i = 0; i < 12; ++i)
    {
        for (int32_t j = 0; j < 24; ++j)
        {
            const float polar = (static_cast<float>(i) + 0.5f) * glm::pi<float>() / 12.f;
            const float azimuth = static_cast<float>(j) * glm::two_pi<float>() / 24.f;
            const glm::vec3 direction(std::sin(polar) * std::cos(azimuth), std::cos(polar),
                                      std::sin(polar) * std::sin(azimuth));
            const glm::vec3 world = direction * 5.f;

            const std::uint32_t face = PointLightFaceOf(direction);
            REQUIRE(face < kPointLightFaceCount);
            // The face the shader will pick has to be a face that can actually
            // see the point. A disagreement here is a fragment sampling a
            // neighbouring face's depth, which reads as a shadow from nowhere.
            CHECK(Project(faces[face], world).Inside());
            ++sampled;
        }
    }
    CHECK(sampled == 288);
}

TEST_CASE("Neighbouring faces overlap, so a filter at a seam has somewhere to land")
{
    const LocalShadowSettings settings;
    const auto faces = PointFaces(10.f, settings);

    // Exactly on the seam between +X and +Y. Six 90-degree faces tile a cube
    // exactly, and exactly is the problem: a PCF kernel here reaches past the
    // tile, and past it is another light's depth. The widened faces are what
    // make both sides of the seam hold this geometry.
    const glm::vec3 seam = glm::normalize(glm::vec3(1.f, 1.f, 0.f)) * 5.f;
    CHECK(Project(faces[0], seam).Inside());
    CHECK(Project(faces[2], seam).Inside());

    // The three-face corner too, which is where the overlap is thinnest.
    const glm::vec3 corner = glm::normalize(glm::vec3(1.f, 1.f, 1.f)) * 5.f;
    CHECK(Project(faces[0], corner).Inside());
    CHECK(Project(faces[2], corner).Inside());
    CHECK(Project(faces[4], corner).Inside());
}

TEST_CASE("A caster close to a point light is still recorded")
{
    const LocalShadowSettings settings;
    const float range = 10.f;
    const auto faces = PointFaces(range, settings);

    // The six faces' near planes bound a cube around the light, and nothing
    // inside it is drawn into the map at all — so a caster in there casts no
    // shadow. That cube swallows exactly the objects nearest a lamp: the shade
    // around a bulb, the table under it, the wall a torch is held against.
    //
    // Ten centimetres from a light of ten metres' reach is the shade case, and
    // it has to be recorded.
    const float nearPlane = range * 0.01f;
    CHECK(nearPlane <= 0.1f);
    CHECK(Project(faces[kPointLightFacePositiveX], glm::vec3(0.1f, 0.f, 0.f)).Inside());

    // The zone is a cube rather than a sphere, because a near plane is a plane:
    // its corner stands sqrt(3) times further from the light than its face does.
    // A caster on the diagonal therefore has to clear more distance than one on
    // an axis, and quoting the dead zone as a radius would understate it.
    const glm::vec3 diagonal = glm::normalize(glm::vec3(1.f, 1.f, 1.f));
    const std::uint32_t corner = PointLightFaceOf(diagonal);
    CHECK_FALSE(Project(faces[corner], diagonal * (nearPlane * 1.6f)).Inside());
    CHECK(Project(faces[corner], diagonal * (nearPlane * 1.8f)).Inside());

    // Sitting on the light is still excluded, and must be: a zero near plane
    // makes the projection singular.
    CHECK_FALSE(Project(faces[kPointLightFacePositiveX], glm::vec3(0.f)).Inside());
}

TEST_CASE("A point light's faces stop at its range")
{
    const LocalShadowSettings settings;
    const auto faces = PointFaces(10.f, settings);

    // Nothing past the light's reach is lit by it, so nothing past it needs to
    // occlude — and a frustum that ran on would spend depth precision on
    // geometry that can never shadow anything.
    CHECK(Project(faces[0], glm::vec3(5.f, 0.f, 0.f)).Inside());
    CHECK_FALSE(Project(faces[0], glm::vec3(11.f, 0.f, 0.f)).Inside());
    // And behind the light, which is another face's business entirely.
    CHECK_FALSE(Project(faces[0], glm::vec3(-5.f, 0.f, 0.f)).Inside());
}

TEST_CASE("A spot light's map covers its cone and a little past it")
{
    const LocalShadowSettings settings;
    const ShadowView view = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, /*outerAngle=*/ 30.f,
                                           TileAt(1024, 512, 512), kAtlas, settings);

    // Straight down the cone's axis.
    CHECK(Project(view, glm::vec3(0.f, -10.f, 0.f)).Inside());
    // At the cone's rim: 30 degrees off axis at ten metres is tan(30) * 10.
    const float rim = std::tan(glm::radians(30.f)) * 10.f;
    CHECK(Project(view, glm::vec3(rim * 0.99f, -10.f, 0.f)).Inside());
    // Just past the rim is still recorded, which is the widening: the filter at
    // the cone's edge must read depth this light drew rather than the tile next
    // to it.
    CHECK(Project(view, glm::vec3(rim * 1.02f, -10.f, 0.f)).Inside());
    // Far outside it is not, so the map is not spending texels on a cone the
    // light does not have.
    CHECK_FALSE(Project(view, glm::vec3(rim * 2.f, -10.f, 0.f)).Inside());
    // Behind the light.
    CHECK_FALSE(Project(view, glm::vec3(0.f, 10.f, 0.f)).Inside());
}

TEST_CASE("A spot aimed straight up still has a basis")
{
    const LocalShadowSettings settings;
    // The degenerate case for the usual +Y up axis. A collapsed basis produces a
    // NaN matrix, and a NaN matrix takes every fragment the light touches with it.
    const ShadowView up = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f), 20.f, 30.f,
                                         TileAt(0, 0, 512), kAtlas, settings);
    CHECK(Project(up, glm::vec3(0.f, 10.f, 0.f)).Inside());

    const ShadowView down = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                           TileAt(0, 0, 512), kAtlas, settings);
    CHECK(Project(down, glm::vec3(0.f, -10.f, 0.f)).Inside());

    // A zero direction comes straight out of a hand-edited level file.
    const ShadowView degenerate = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f), 20.f, 30.f, TileAt(0, 0, 512),
                                                 kAtlas, settings);
    for (int32_t i = 0; i < 4; ++i)
    {
        CHECK(std::isfinite(degenerate.viewProjection[i][0]));
        CHECK(std::isfinite(degenerate.viewProjection[i][3]));
    }
}

TEST_CASE("A view's rectangle maps onto its own tile and no other")
{
    const LocalShadowSettings settings;
    const ShadowView view = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                           TileAt(1024, 2048, 512), kAtlas, settings);

    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    CHECK(scaleOffset.x == doctest::Approx(512.f / 4096.f));
    CHECK(scaleOffset.y == doctest::Approx(512.f / 4096.f));
    CHECK(scaleOffset.z == doctest::Approx(1024.f / 4096.f));
    CHECK(scaleOffset.w == doctest::Approx(2048.f / 4096.f));

    // Every corner of the view's own UV square lands inside its tile: the
    // transform is what stops a lookup reaching the light beside it.
    for (const glm::vec2 corner : {glm::vec2(0.f, 0.f), glm::vec2(1.f, 0.f), glm::vec2(0.f, 1.f),
                                   glm::vec2(1.f, 1.f)})
    {
        const glm::vec2 uv = corner * glm::vec2(scaleOffset.x, scaleOffset.y) +
                             glm::vec2(scaleOffset.z, scaleOffset.w);
        CHECK(uv.x >= doctest::Approx(1024.f / 4096.f));
        CHECK(uv.x <= doctest::Approx(1536.f / 4096.f));
        CHECK(uv.y >= doctest::Approx(2048.f / 4096.f));
        CHECK(uv.y <= doctest::Approx(2560.f / 4096.f));
    }
}

TEST_CASE("A lookup is clamped inside its tile by the filter's own reach")
{
    LocalShadowSettings settings;
    settings.filter = ShadowFilter::Pcf5x5;

    const ShadowView view = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                           TileAt(1024, 2048, 512), kAtlas, settings);

    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    const glm::vec2 tileMin(scaleOffset.z, scaleOffset.w);
    const glm::vec2 tileMax = tileMin + glm::vec2(scaleOffset.x, scaleOffset.y);

    // Strictly inside the tile on every side. The sampler's own clamp is to the
    // whole atlas, so this rectangle is the only thing between a kernel at the
    // tile's edge and the next light's depth.
    CHECK(view.clampUv.x > tileMin.x);
    CHECK(view.clampUv.y > tileMin.y);
    CHECK(view.clampUv.z < tileMax.x);
    CHECK(view.clampUv.w < tileMax.y);

    // The inset is the kernel's reach plus the half texel the hardware's own
    // bilinear comparison covers on top of wherever a tap lands.
    const float expected = view.filterTapStepUv * (FilterRadiusTaps(ShadowFilter::Pcf5x5) + 0.5f);
    CHECK(view.clampUv.x - tileMin.x == doctest::Approx(expected));

    // A wider filter insets further, which is the whole point of it depending on
    // the filter at all.
    LocalShadowSettings narrow = settings;
    narrow.filter = ShadowFilter::Point;
    const ShadowView narrowView = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                                 TileAt(1024, 2048, 512), kAtlas, narrow);
    CHECK(narrowView.clampUv.x < view.clampUv.x);
}

TEST_CASE("The smallest tile the allocator can hand out still has an interior")
{
    // The inset is a few texels and the smallest class is 128 of them, so no
    // real tile is ever swallowed by its own kernel. Worth stating: it is why
    // the collapse below is a guard against a malformed view rather than a case
    // the allocator can reach.
    LocalShadowSettings settings;
    settings.filter = ShadowFilter::Vogel;

    const ShadowView view = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                           TileAt(0, 0, kMinShadowFaceResolution), kMaxShadowAtlasResolution,
                                           settings);
    CHECK(view.clampUv.x < view.clampUv.z);
    CHECK(view.clampUv.y < view.clampUv.w);
}

TEST_CASE("A tile narrower than its kernel collapses to its centre rather than inverting")
{
    // Four texels, which no allocator hands out — this is a view built by hand
    // or left half-initialised. Collapsing to the centre keeps every tap inside
    // this light's own depth, which is a harder shadow rather than a wrong one;
    // an inverted rectangle would clamp a lookup to nothing meaningful.
    ShadowView view;
    view.rect = TileAt(0, 0, 4);
    view.targetResolution = 8192;
    view.filterTapStepUv = LocalFilterTapStepUv(8192);
    const glm::vec4 clampUv = ShadowViewClampUv(view, ShadowFilter::Vogel);

    CHECK(clampUv.x <= clampUv.z);
    CHECK(clampUv.y <= clampUv.w);
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    CHECK(clampUv.x == doctest::Approx(scaleOffset.z + scaleOffset.x * 0.5f));
    CHECK(clampUv.z == doctest::Approx(clampUv.x));
}

TEST_CASE("A cascade's clamp is its whole slice")
{
    // A cascade owns its target outright, so there is nothing beside it to read
    // into and insetting would trim a border off every shadow for nothing.
    SunShadowSettings sun;
    ShadowCascade cascade;
    cascade.radius = 10.f;
    cascade.depthRange = 20.f;
    cascade.worldUnitsPerTexel = 20.f / static_cast<float>(sun.resolution);

    const ShadowView view = CascadeShadowView(cascade, 0, sun);
    CHECK(view.clampUv.x == doctest::Approx(0.f));
    CHECK(view.clampUv.y == doctest::Approx(0.f));
    CHECK(view.clampUv.z == doctest::Approx(1.f));
    CHECK(view.clampUv.w == doctest::Approx(1.f));
}

TEST_CASE("A tile's biases are scaled to the tile it actually got")
{
    LocalShadowSettings settings;
    settings.depthBiasTexels = 2.f;
    settings.normalOffsetTexels = 2.f;

    const float tanHalfFov = std::tan(glm::radians(45.f));

    // Halving the tile's edge doubles what one of its texels covers, so both
    // biases double from the same setting. Without this a demoted light is
    // biased for a map four times sharper than the one it has, and shows acne
    // exactly where it was already the worst served.
    const float full = LocalTexelsPerUnitDistance(512, tanHalfFov);
    const float half = LocalTexelsPerUnitDistance(256, tanHalfFov);
    CHECK(half == doctest::Approx(full * 2.f));

    CHECK(LocalNormalOffsetPerDistance(256, tanHalfFov, settings) ==
          doctest::Approx(LocalNormalOffsetPerDistance(512, tanHalfFov, settings) * 2.f));
    CHECK(LocalDepthBiasNdcTimesDistance(256, 1.f, 20.f, tanHalfFov, settings) ==
          doctest::Approx(LocalDepthBiasNdcTimesDistance(512, 1.f, 20.f, tanHalfFov, settings) * 2.f));

    // Both are proportional to their setting, so turning a knob to zero turns
    // the bias off rather than leaving a floor under it.
    LocalShadowSettings none = settings;
    none.depthBiasTexels = 0.f;
    none.normalOffsetTexels = 0.f;
    CHECK(LocalDepthBiasNdcTimesDistance(512, 1.f, 20.f, tanHalfFov, none) == doctest::Approx(0.f));
    CHECK(LocalNormalOffsetPerDistance(512, tanHalfFov, none) == doctest::Approx(0.f));
}

TEST_CASE("A local light's biases follow the receiver, not the light's range")
{
    LocalShadowSettings settings;
    settings.normalOffsetTexels = 1.5f;
    settings.depthBiasTexels = 1.5f;

    const float tanHalfFov = std::tan(glm::radians(48.f));
    const std::uint32_t tile = 512;

    // The normal offset is quoted per unit of distance, so a receiver twice as
    // far from the light gets twice the push — which is what one texel of a
    // perspective map is out there.
    //
    // The bug this replaced quoted it at the far plane instead, cascade-style.
    // A floor three metres under a lamp of fifty metres' range then got the push
    // meant for fifty: about a third of a metre of *sideways* movement, which
    // walks the lookup clean out from under whatever was shading it.
    const float perDistance = LocalNormalOffsetPerDistance(tile, tanHalfFov, settings);
    const float atThreeMetres = perDistance * 3.f;
    const float atFiftyMetres = perDistance * 50.f;
    CHECK(atFiftyMetres == doctest::Approx(atThreeMetres * (50.f / 3.f)));
    // Small enough at the receiver to be a texel rather than a displacement: one
    // texel three metres from the light is about a centimetre.
    CHECK(atThreeMetres < 0.05f);
    // And the figure the old code would have used is an order of magnitude more.
    CHECK(atFiftyMetres > atThreeMetres * 10.f);

    // The depth bias is quoted times a distance, so the shader's divide leaves a
    // bias that falls as the receiver recedes — the two effects that vary with
    // distance are a texel's growing footprint and a world unit's shrinking worth
    // in depth, and they do not cancel.
    const float nearPlane = 0.5f;
    const float farPlane = 50.f;
    const float coefficient = LocalDepthBiasNdcTimesDistance(tile, nearPlane, farPlane, tanHalfFov, settings);
    const float biasNear = coefficient / 3.f;
    const float biasFar = coefficient / 30.f;
    CHECK(biasNear == doctest::Approx(biasFar * 10.f));

    // In world terms that is the same number of texels at either distance, which
    // is what "a bias of 1.5 texels" is supposed to mean and what the far-plane
    // form could not deliver.
    const auto worldBias = [&](float distance)
                           {
                               const float ndcPerWorld = nearPlane * farPlane / ((farPlane - nearPlane) * distance * distance);
                               return (coefficient / distance) / ndcPerWorld;
                           };
    // Against the texel's own footprint, not the normal offset's — they differ by
    // the offset setting, and the question here is how many texels the depth bias
    // is worth.
    const float texelPerDistance = LocalTexelsPerUnitDistance(tile, tanHalfFov);
    const auto texelsAt = [&](float distance) { return worldBias(distance) / (texelPerDistance * distance); };
    CHECK(texelsAt(3.f) == doctest::Approx(texelsAt(30.f)));
    CHECK(texelsAt(3.f) == doctest::Approx(settings.depthBiasTexels));
}

TEST_CASE("The slope-bias cap is a perspective texel, not an orthographic one")
{
    // A cascade's cap is 1 / resolution, because an orthographic depth is linear
    // and one texel is always that fraction of the range. A perspective depth is
    // not, and borrowing the cascade figure lets a grazing surface be pushed
    // metres behind itself — which prints as light under everything it should
    // have shadowed. The cap here has to be far tighter than the cascade one.
    const float perspective = LocalSlopeBiasClampNdc(512);
    const float orthographic = 1.f / 512.f;
    CHECK(perspective < orthographic * 0.05f);
    CHECK(perspective > 0.f);

    // Still a texel's worth, so a demoted tile gets a proportionally looser cap
    // from the same rule — halving the edge doubles what a texel covers.
    CHECK(LocalSlopeBiasClampNdc(256) == doctest::Approx(perspective * 2.f));

    // The light's own range does not appear: the near plane is a fixed fraction
    // of the far one, so every local light's depth curve has the same shape and
    // one cap serves them all. A cap that varied with range would have to be
    // baked into a pipeline per light.
    const float tanHalfFov = std::tan(glm::radians((90.f + kPointLightFaceOverlapDegrees) * 0.5f));
    for (const float range : {1.f, 10.f, 100.f})
    {
        const float nearPlane = range * 0.01f;
        // One texel at the far plane, in world units, times what a world unit is
        // worth in [0, 1] depth there — the two figures the cap is the product of.
        const float worldPerTexel = 2.f * range * tanHalfFov / 512.f;
        const float ndcPerWorld = nearPlane / ((range - nearPlane) * range);
        CHECK(worldPerTexel * ndcPerWorld == doctest::Approx(perspective));
    }

    CHECK(LocalSlopeBiasClampNdc(0) == doctest::Approx(0.f));
}

TEST_CASE("The filter's tap step is one atlas texel whatever the tile's size")
{
    // A tile's texels *are* the atlas's texels — a tile is a rectangle of the
    // same grid, not a resampling of it. A step quoted against the tile would
    // make a demoted light's kernel skip several atlas texels at a time.
    CHECK(LocalFilterTapStepUv(4096) == doctest::Approx(1.f / 4096.f));
    CHECK(LocalFilterTapStepUv(8192) == doctest::Approx(1.f / 8192.f));
    CHECK(LocalFilterTapStepUv(0) == doctest::Approx(0.f));

    LocalShadowSettings settings;
    const ShadowView big = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                          TileAt(0, 0, 512), kAtlas, settings);
    const ShadowView small = SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                            TileAt(0, 0, 128), kAtlas, settings);
    CHECK(big.filterTapStepUv == doctest::Approx(small.filterTapStepUv));
}

TEST_CASE("Every local view is packed into the row the shader reads")
{
    LocalShadowSettings settings;
    const ShadowView view = SpotShadowView(glm::vec3(1.f, 2.f, 3.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f,
                                           TileAt(512, 1024, 256), kAtlas, settings);

    const ShadowViewGpu packed = PackShadowView(view);
    CHECK(packed.viewProjection == view.viewProjection);
    CHECK(packed.uvScaleOffset == ShadowViewUvScaleOffset(view));
    CHECK(packed.params.x == doctest::Approx(view.depthBias));
    CHECK(packed.params.y == doctest::Approx(view.normalOffset));
    CHECK(packed.params.z == doctest::Approx(view.filterTapStepUv));
    // Every atlas tile shares slice zero: the atlas is one texture, and the
    // slice lane exists for the cascade array that is not.
    CHECK(packed.params.w == doctest::Approx(0.f));
    CHECK(packed.clampUv == view.clampUv);
}

TEST_CASE("A local view never claims to be orthographic")
{
    const LocalShadowSettings settings;

    // Two things in the depth pass are valid only under an orthographic
    // projection, and both are silent when they are wrong: pancaking a caster
    // onto the near plane, which is a clamp in comparison depth only while w is
    // 1, and dropping the near plane from the cull, which under perspective
    // admits the mirrored cone behind the light. A local view saying yes here
    // would turn both on.
    CHECK_FALSE(SpotShadowView(glm::vec3(0.f), glm::vec3(0.f, -1.f, 0.f), 20.f, 30.f, TileAt(0, 0, 512), kAtlas,
                               settings)
                .orthographic);
    for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
    {
        CHECK_FALSE(PointFaceShadowView(glm::vec3(0.f), 10.f, face, TileAt(0, 0, 512), kAtlas, settings)
                    .orthographic);
    }

    // A cascade does, and must: it is the case the pancaking was written for.
    SunShadowSettings sun;
    ShadowCascade cascade;
    cascade.radius = 10.f;
    cascade.depthRange = 20.f;
    CHECK(CascadeShadowView(cascade, 0, sun).orthographic);

    // And a default-constructed view does not, so a caller that forgets loses a
    // caster's pancaking rather than corrupting a map.
    CHECK_FALSE(ShadowView{}.orthographic);
}

TEST_CASE("A face's index and its axis are the same six, in the same order")
{
    // The shader recomputes the face from a direction, so the two selections
    // have to agree — and the axis a face looks along is what makes that
    // checkable without rendering.
    for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
    {
        CHECK(PointLightFaceOf(PointLightFaceDirection(face)) == face);
    }

    // A zero direction lands somewhere rather than nowhere.
    CHECK(PointLightFaceOf(glm::vec3(0.f)) < kPointLightFaceCount);
}
