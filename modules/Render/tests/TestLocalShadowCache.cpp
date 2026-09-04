/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLocalShadowCache.cpp
/// @brief The stale-shadow cases, written down.
///
/// A tile kept when it should have been redrawn is a wrong image with no visual
/// tell — a shadow that quietly stopped following its object. Every test here is
/// one way that can happen: a caster moves, a caster settles, a light moves, a
/// tile changes size, a light loses its tile to somebody else. The cache is
/// device-free precisely so these can be asserted rather than looked for.

#include <doctest/doctest.h>

#include <Assisi/Render/LocalShadowCache.hpp>

#include <cstdint>
#include <vector>

using namespace Assisi::Render;
using Assisi::Geometry::BoundingSphere;

namespace
{
LocalShadowRequest SpotAt(std::uint32_t lightIndex, const glm::vec3 &position, float range = 10.f)
{
    LocalShadowRequest request;
    request.kind = LocalLightKind::Spot;
    request.lightIndex = lightIndex;
    request.pose.position = position;
    request.pose.direction = glm::vec3(0.f, -1.f, 0.f);
    request.pose.range = range;
    request.pose.outerAngleDegrees = 45.f;
    request.sizeClass = ShadowSizeClassOf(512);
    return request;
}

LocalShadowRequest PointAt(std::uint32_t lightIndex, const glm::vec3 &position, float range = 10.f)
{
    LocalShadowRequest request = SpotAt(lightIndex, position, range);
    request.kind = LocalLightKind::Point;
    return request;
}

ShadowMover CasterAt(std::uint64_t id, const glm::vec3 &position, float radius = 1.f)
{
    return ShadowMover{id, BoundingSphere{position, radius}};
}

/// One tile of 512 texels per face, laid out left to right. Enough to stand in
/// for an allocation without pulling the allocator into these tests.
std::vector<ShadowViewRect> RectsFor(std::span<const LocalShadowRequest> requests,
                                     std::span<const std::uint32_t> served)
{
    std::vector<ShadowViewRect> rects;
    std::uint32_t x = 0;
    for (const std::uint32_t index : served)
    {
        for (std::uint32_t face = 0; face < LocalShadowFaceCount(requests[index].kind); ++face)
        {
            rects.push_back(ShadowViewRect{.x = x, .y = 0, .width = 512, .height = 512});
            x += 512;
        }
    }
    return rects;
}

/// Serve every request whose plan the budget did not refuse, which is what the
/// pass does when the atlas has room for all of them.
std::vector<std::uint32_t> ServedOf(std::span<const LocalShadowTilePlan> plans)
{
    std::vector<std::uint32_t> served;
    for (std::uint32_t index = 0; index < plans.size(); ++index)
    {
        if (!plans[index].deferred)
        {
            served.push_back(index);
        }
    }
    return served;
}

LocalShadowCacheFrame FrameAt(std::uint32_t index, const LocalShadowCacheSettings &settings,
                              std::span<const LocalShadowRequest> requests, std::span<const ShadowMover> movers = {},
                              std::span<const ShadowMover> invalidations = {})
{
    return LocalShadowCacheFrame{.frameIndex = index,
                                 .settings = settings,
                                 .requests = requests,
                                 .movers = movers,
                                 .invalidations = invalidations};
}

/// Plan and commit one frame in which everything the budget allows is served.
void RunFrame(LocalShadowCache &cache, const LocalShadowCacheFrame &frame, std::vector<LocalShadowTilePlan> &plans)
{
    cache.Plan(frame, plans);
    const std::vector<std::uint32_t> served = ServedOf(plans);
    const std::vector<ShadowViewRect> rects = RectsFor(frame.requests, served);
    cache.Commit(frame.frameIndex, frame.requests, plans, served, rects);
}
} // namespace

TEST_CASE("A light's first frame has no tile to keep, so every face is drawn")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f)), PointAt(1, glm::vec3(50.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    REQUIRE(plans.size() == 2);
    CHECK_FALSE(plans[0].retained);
    CHECK(plans[0].dirtyFaces == 0b1u);
    CHECK_FALSE(plans[1].retained);
    CHECK(plans[1].dirtyFaces == 0b111111u); // a point light's whole cube
}

TEST_CASE("A resting light keeps its tile and draws nothing")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);
    const ShadowViewRect first = plans[0].rect[0];

    // The pay-for-what-you-place gate, per light: nothing moved, so there is
    // nothing to redraw and nothing to compose — the tile already holds it.
    for (std::uint32_t frame = 2; frame < 10; ++frame)
    {
        RunFrame(cache, FrameAt(frame, settings, requests), plans);
        CHECK(plans[0].retained);
        CHECK(plans[0].dirtyFaces == 0);
        CHECK_FALSE(plans[0].hasMovers);
    }
    CHECK(cache.Stats().restingLights == 1);
    // And it is the same rectangle throughout, which is the only thing that
    // makes the depth in it still this light's depth.
    CHECK(plans[0].rect[0].x == first.x);
    CHECK(plans[0].rect[0].y == first.y);
}

TEST_CASE("A light that moves cannot keep depth recorded from where it was")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);
    RunFrame(cache, FrameAt(2, settings, requests), plans);
    REQUIRE(plans[0].dirtyFaces == 0);

    SUBCASE("moving it")
    {
        requests[0].pose.position = glm::vec3(1.f, 0.f, 0.f);
    }
    SUBCASE("aiming it elsewhere")
    {
        requests[0].pose.direction = glm::vec3(1.f, 0.f, 0.f);
    }
    SUBCASE("changing its reach")
    {
        requests[0].pose.range = 20.f;
    }
    SUBCASE("opening its cone")
    {
        requests[0].pose.outerAngleDegrees = 60.f;
    }
    SUBCASE("resizing its tile")
    {
        requests[0].sizeClass = ShadowSizeClassOf(256);
    }

    cache.Plan(FrameAt(3, settings, requests), plans);
    CHECK_FALSE(plans[0].retained);
    CHECK(plans[0].dirtyFaces == 0b1u);
}

TEST_CASE("A caster crossing into a light's reach dirties it, and only it")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f), 10.f),
                                                   SpotAt(1, glm::vec3(100.f, 0.f, 0.f), 10.f)};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    // A caster leaving the cached layer invalidates where it was and where it
    // is. The far light saw neither, and must not pay for it.
    const std::vector<ShadowMover> leaving{CasterAt(7, glm::vec3(2.f, 0.f, 0.f))};
    cache.Plan(FrameAt(2, settings, requests, {}, leaving), plans);
    CHECK(plans[0].dirtyFaces == 0b1u);
    CHECK(plans[1].dirtyFaces == 0);
}

TEST_CASE("A caster that is merely moving dirties nothing")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f), 10.f)};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    // It is not in the cached layer — it draws over the copy every frame — so a
    // frame of its motion costs the moving layer and never a re-bake. That is
    // what makes a motion episode two bakes rather than one per frame.
    const std::vector<ShadowMover> moving{CasterAt(7, glm::vec3(2.f, 0.f, 0.f))};
    for (std::uint32_t frame = 2; frame < 30; ++frame)
    {
        RunFrame(cache, FrameAt(frame, settings, requests, moving), plans);
        CHECK(plans[0].dirtyFaces == 0);
        CHECK(plans[0].hasMovers);
    }
    CHECK(cache.Stats().bakedFaces == 0);
}

TEST_CASE("Out of a light's reach is out of its tile")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f), 10.f)};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    // Just past the sum of the two radii. A caster outside the light's sphere
    // occludes nothing for it, which is the sole reason a scene with hundreds of
    // lights does not cost hundreds of invalidations per mover.
    const std::vector<ShadowMover> far{CasterAt(7, glm::vec3(11.5f, 0.f, 0.f), 1.f)};
    cache.Plan(FrameAt(2, settings, requests, far, far), plans);
    CHECK(plans[0].dirtyFaces == 0);
    CHECK_FALSE(plans[0].hasMovers);
}

TEST_CASE("A mover dirties only the point-light faces it can cast into")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{PointAt(0, glm::vec3(0.f), 20.f)};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    // Straight down +X and small: it belongs to that face and to nothing on the
    // far side of the light. The definition of done says a mover dirties only
    // the faces whose frustum contains it.
    const std::vector<ShadowMover> mover{CasterAt(7, glm::vec3(10.f, 0.f, 0.f), 0.5f)};
    cache.Plan(FrameAt(2, settings, requests, {}, mover), plans);
    CHECK((plans[0].dirtyFaces & (1u << kPointLightFacePositiveX)) != 0u);
    CHECK((plans[0].dirtyFaces & (1u << kPointLightFaceNegativeX)) == 0u);
    CHECK(plans[0].dirtyFaces != 0b111111u);
}

TEST_CASE("An ambiguous caster dirties every face it might reach")
{
    const LocalShadowLightPose pose{
        .position = glm::vec3(0.f), .direction = glm::vec3(0.f, -1.f, 0.f), .range = 20.f, .outerAngleDegrees = 45.f};

    // On the corner three faces meet at, and so in all three.
    const std::uint32_t corner =
        LocalShadowFaceMask(pose, LocalLightKind::Point, BoundingSphere{glm::vec3(5.f, 5.f, 5.f), 0.5f});
    CHECK((corner & (1u << kPointLightFacePositiveX)) != 0u);
    CHECK((corner & (1u << kPointLightFacePositiveY)) != 0u);
    CHECK((corner & (1u << kPointLightFacePositiveZ)) != 0u);

    // Swallowing the light: it has no direction from it at all, so every face
    // records it. The conservative branch, and the one a divide would have got
    // wrong silently.
    CHECK(LocalShadowFaceMask(pose, LocalLightKind::Point, BoundingSphere{glm::vec3(0.f), 3.f}) == 0b111111u);

    // Large enough to span most of the sky from the light, which is a caster
    // every face sees some of.
    CHECK(LocalShadowFaceMask(pose, LocalLightKind::Point, BoundingSphere{glm::vec3(4.f, 0.f, 0.f), 8.f}) == 0b111111u);

    // A spot has one face, and it is dirtied or it is not.
    CHECK(LocalShadowFaceMask(pose, LocalLightKind::Spot, BoundingSphere{glm::vec3(5.f, 0.f, 0.f), 0.5f}) == 0b1u);
    CHECK(LocalShadowFaceMask(pose, LocalLightKind::Spot, BoundingSphere{glm::vec3(500.f, 0.f, 0.f), 0.5f}) == 0u);
}

TEST_CASE("A budget refuses a light rather than showing it a stale tile")
{
    LocalShadowCache cache;
    LocalShadowCacheSettings settings;
    settings.updateBudgetFaces = 7; // one point light's cube, and one spot

    // Importance order: the point light asks first, so it is the spots at the
    // end of the list that wait their turn.
    const std::vector<LocalShadowRequest> requests{PointAt(0, glm::vec3(0.f)), SpotAt(1, glm::vec3(50.f)),
                                                   SpotAt(2, glm::vec3(100.f))};

    std::vector<LocalShadowTilePlan> plans;
    cache.Plan(FrameAt(1, settings, requests), plans);

    CHECK_FALSE(plans[0].deferred);
    CHECK_FALSE(plans[1].deferred);
    CHECK(plans[2].deferred); // unshadowed this frame, never shadowed from a wrong tile
    CHECK(cache.Stats().bakedFaces == 7);
    CHECK(cache.Stats().deferredLights == 1);
}

TEST_CASE("A face the budget refused stays owed")
{
    LocalShadowCache cache;
    LocalShadowCacheSettings settings;
    settings.updateBudgetFaces = 1;

    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f)), SpotAt(1, glm::vec3(50.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);
    REQUIRE(plans[1].deferred);

    // The deferred light was never served, so it has no residency to carry a
    // debt in — it asks outright next frame, and now there is room.
    RunFrame(cache, FrameAt(2, settings, requests), plans);
    CHECK_FALSE(plans[0].deferred); // its tile is clean, so it costs nothing
    CHECK(plans[0].dirtyFaces == 0);
    CHECK_FALSE(plans[1].deferred);
    CHECK(plans[1].dirtyFaces == 0b1u);
}

TEST_CASE("A light that loses its tile forgets the depth in it")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> both{SpotAt(0, glm::vec3(0.f)), SpotAt(1, glm::vec3(50.f))};
    const std::vector<LocalShadowRequest> one{SpotAt(0, glm::vec3(0.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, both), plans);
    // Light 1 drops out of the selection — its rectangle is somebody else's from
    // here, so remembering it would hand back another light's depth.
    RunFrame(cache, FrameAt(2, settings, one), plans);
    RunFrame(cache, FrameAt(3, settings, both), plans);

    CHECK(plans[0].retained);
    CHECK_FALSE(plans[1].retained);
    CHECK(plans[1].dirtyFaces == 0b1u);
}

TEST_CASE("The throttle spares the lights that matter and never a fresh tile")
{
    LocalShadowCache cache;
    LocalShadowCacheSettings settings;
    settings.movingLightUpdateDivisor = 3;

    std::vector<LocalShadowRequest> requests;
    for (std::uint32_t index = 0; index < 9; ++index)
    {
        requests.push_back(SpotAt(index, glm::vec3(static_cast<float>(index) * 50.f, 0.f, 0.f)));
    }
    const std::vector<ShadowMover> movers{CasterAt(1, glm::vec3(0.f)), CasterAt(2, glm::vec3(50.f, 0.f, 0.f)),
                                          CasterAt(3, glm::vec3(400.f, 0.f, 0.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests, movers), plans);
    RunFrame(cache, FrameAt(2, settings, requests, movers), plans);

    // The top of the ordering redraws every frame whatever the divisor says: a
    // frame of lag on the light the shot is built around is the one place this
    // is not free.
    CHECK(plans[0].redrawMovers);
    CHECK(plans[1].redrawMovers);
    // A light with nothing moving under it has no moving layer to throttle.
    CHECK(plans[4].redrawMovers);

    // And a light whose tile is being cut fresh is never throttled — there is no
    // cached composite for it to fall back on.
    LocalShadowCache cold;
    cold.Plan(FrameAt(1, settings, requests, movers), plans);
    for (const LocalShadowTilePlan &plan : plans)
    {
        CHECK(plan.redrawMovers);
    }
}

TEST_CASE("Forget drops every tile")
{
    LocalShadowCache cache;
    const LocalShadowCacheSettings settings;
    const std::vector<LocalShadowRequest> requests{SpotAt(0, glm::vec3(0.f))};

    std::vector<LocalShadowTilePlan> plans;
    RunFrame(cache, FrameAt(1, settings, requests), plans);

    // A resize or a reformat makes every remembered rectangle a rectangle of a
    // texture that is gone.
    cache.Forget();
    cache.Plan(FrameAt(2, settings, requests), plans);
    CHECK_FALSE(plans[0].retained);
    CHECK(cache.Tiles().empty());
}

TEST_CASE("Mobility: a caster's first moved frame demotes it, and it invalidates both poses")
{
    ShadowCasterMobility mobility;
    mobility.NoteBaked(CasterAt(7, glm::vec3(0.f), 1.f));

    std::vector<ShadowMover> dynamic;
    std::vector<ShadowMover> invalidate;
    mobility.Update(1, 30, std::vector<ShadowMover>{CasterAt(7, glm::vec3(6.f, 0.f, 0.f), 1.f)}, dynamic, invalidate);

    CHECK(mobility.IsDynamic(7));
    REQUIRE(dynamic.size() == 1);
    REQUIRE(invalidate.size() == 1);

    // The cached layer holds it at the origin and it is now six metres away, so
    // the tiles to clear are the ones either pose reaches. Invalidating only the
    // new one leaves the old shadow standing where nothing is — which is the
    // defect with no visual tell.
    const BoundingSphere &span = invalidate[0].worldSphere;
    CHECK(span.radius >= 4.f);
    CHECK(glm::length(span.center - glm::vec3(0.f)) + 1.f <= span.radius + 0.001f);
    CHECK(glm::length(span.center - glm::vec3(6.f, 0.f, 0.f)) + 1.f <= span.radius + 0.001f);
}

TEST_CASE("Mobility: a caster never baked invalidates only where it now stands")
{
    ShadowCasterMobility mobility;

    std::vector<ShadowMover> dynamic;
    std::vector<ShadowMover> invalidate;
    mobility.Update(1, 30, std::vector<ShadowMover>{CasterAt(7, glm::vec3(6.f, 0.f, 0.f), 1.f)}, dynamic, invalidate);

    // It is in no cached layer, so there is no old pose to clear and widening
    // the invalidation would dirty tiles for nothing.
    REQUIRE(invalidate.size() == 1);
    CHECK(invalidate[0].worldSphere.radius == doctest::Approx(1.f));
    CHECK(invalidate[0].worldSphere.center.x == doctest::Approx(6.f));
}

TEST_CASE("Mobility: a caster that settles promotes once, and only after the wait")
{
    ShadowCasterMobility mobility;
    std::vector<ShadowMover> dynamic;
    std::vector<ShadowMover> invalidate;

    mobility.Update(1, 4, std::vector<ShadowMover>{CasterAt(7, glm::vec3(0.f))}, dynamic, invalidate);
    REQUIRE(mobility.IsDynamic(7));

    // Still, but not yet still enough. It keeps drawing with the movers and
    // costs no bake — a caster that pauses mid-motion must not re-bake every
    // tile around it and then immediately undo that.
    for (std::uint32_t frame = 2; frame < 5; ++frame)
    {
        mobility.Update(frame, 4, {}, dynamic, invalidate);
        CHECK(mobility.IsDynamic(7));
        CHECK(dynamic.size() == 1);
        CHECK(invalidate.empty());
    }

    mobility.Update(5, 4, {}, dynamic, invalidate);
    CHECK_FALSE(mobility.IsDynamic(7));
    CHECK(dynamic.empty());
    // One invalidation, because the cached layer was baked without it and now
    // has to hold it.
    CHECK(invalidate.size() == 1);
    CHECK(mobility.DynamicCount() == 0);

    // And it stays settled: a promotion that fired again every frame would be
    // an endless re-bake of a scene standing perfectly still.
    mobility.Update(6, 4, {}, dynamic, invalidate);
    CHECK(invalidate.empty());
}

TEST_CASE("Mobility: a motion episode costs two bakes however long it runs")
{
    ShadowCasterMobility mobility;
    mobility.NoteBaked(CasterAt(7, glm::vec3(0.f)));

    std::vector<ShadowMover> dynamic;
    std::vector<ShadowMover> invalidate;
    std::uint32_t invalidations = 0;

    for (std::uint32_t frame = 1; frame <= 40; ++frame)
    {
        // Twenty frames of motion, then twenty of rest.
        const std::vector<ShadowMover> moved =
            frame <= 20 ? std::vector<ShadowMover>{CasterAt(7, glm::vec3(static_cast<float>(frame), 0.f, 0.f))}
                        : std::vector<ShadowMover>{};
        mobility.Update(frame, 5, moved, dynamic, invalidate);
        invalidations += static_cast<std::uint32_t>(invalidate.size());
    }

    // One for the demotion, one for the promotion. This is the whole of the
    // "amortized static" claim, and it is why an authored mobility flag buys
    // nothing here.
    CHECK(invalidations == 2);
    CHECK_FALSE(mobility.IsDynamic(7));
}

TEST_CASE("Mobility: Clear forgets every caster")
{
    ShadowCasterMobility mobility;
    std::vector<ShadowMover> dynamic;
    std::vector<ShadowMover> invalidate;
    mobility.Update(1, 30, std::vector<ShadowMover>{CasterAt(7, glm::vec3(0.f))}, dynamic, invalidate);
    REQUIRE(mobility.DynamicCount() == 1);

    mobility.Clear();
    CHECK_FALSE(mobility.IsDynamic(7));
    CHECK(mobility.DynamicCount() == 0);
}
