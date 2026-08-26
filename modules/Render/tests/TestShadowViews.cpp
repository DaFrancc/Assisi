/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

#include <vector>

using namespace Assisi::Render;

namespace
{
/// An orthographic view looking down -Z from z = 50, bounding [-10, 10] in x
/// and y. Built the way a fitted cascade is, so it carries the same clip-space
/// convention the frustum extraction assumes.
glm::mat4 BoxView(float halfExtent = 10.f)
{
    return glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.f, 100.f) *
           glm::lookAt(glm::vec3(0.f, 0.f, 50.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
}

ShadowView ViewOf(const glm::mat4 &viewProjection)
{
    ShadowView view;
    view.viewProjection = viewProjection;
    view.rect = ShadowViewRect{.x = 0, .y = 0, .width = 1024, .height = 1024};
    view.targetResolution = 1024;
    return view;
}

/// A caster at @p center of radius @p radius, drawing @p indexCount indices of
/// the geometry named by @p key. The buffer handles are never dereferenced by
/// anything under test — only compared — so a distinct address per arena is all
/// they have to be.
ShadowCaster CasterAt(const glm::vec3 &center, float radius, std::uint64_t key, nvrhi::IBuffer *vertexBuffer = nullptr,
                      nvrhi::IBuffer *indexBuffer = nullptr)
{
    ShadowCaster caster;
    caster.geometryKey = key;
    caster.vertexBuffer = vertexBuffer;
    caster.indexBuffer = indexBuffer;
    caster.indexCount = 36;
    caster.startIndexLocation = static_cast<std::uint32_t>(key) * 36u;
    caster.baseVertexLocation = 0;
    caster.model = glm::translate(glm::mat4(1.f), center);
    caster.worldSphere = Assisi::Geometry::BoundingSphere{center, radius};
    return caster;
}

ShadowDepthTarget TargetOf(const ShadowView &view)
{
    return ShadowDepthTarget{.view = view, .framebuffer = nullptr};
}
} // namespace

TEST_CASE("A view owning its whole target samples with the identity transform")
{
    const ShadowView view = ViewOf(BoxView());
    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);

    CHECK(scaleOffset.x == doctest::Approx(1.f));
    CHECK(scaleOffset.y == doctest::Approx(1.f));
    CHECK(scaleOffset.z == doctest::Approx(0.f));
    CHECK(scaleOffset.w == doctest::Approx(0.f));

    const nvrhi::Viewport viewport = ShadowViewViewport(view);
    CHECK(viewport.minX == doctest::Approx(0.f));
    CHECK(viewport.maxX == doctest::Approx(1024.f));
    CHECK(viewport.minY == doctest::Approx(0.f));
    CHECK(viewport.maxY == doctest::Approx(1024.f));
}

TEST_CASE("A view into a sub-rect maps its own corners and nothing else")
{
    // A 512-texel tile at (1024, 2048) of a 4096 atlas — the shape a local
    // light's face takes. The UV transform and the viewport are derived from
    // one rectangle, so they cannot disagree about where the tile is.
    ShadowView view = ViewOf(BoxView());
    view.rect = ShadowViewRect{.x = 1024, .y = 2048, .width = 512, .height = 512};
    view.targetResolution = 4096;

    const glm::vec4 scaleOffset = ShadowViewUvScaleOffset(view);
    CHECK(scaleOffset.x == doctest::Approx(0.125f));
    CHECK(scaleOffset.y == doctest::Approx(0.125f));
    CHECK(scaleOffset.z == doctest::Approx(0.25f));
    CHECK(scaleOffset.w == doctest::Approx(0.5f));

    // The view's own [0, 1] maps onto exactly the tile, so a lookup at its edge
    // lands on the tile's edge rather than in a neighbour.
    const auto sample = [&scaleOffset](glm::vec2 uv)
    { return uv * glm::vec2(scaleOffset.x, scaleOffset.y) + glm::vec2(scaleOffset.z, scaleOffset.w); };
    CHECK(sample({0.f, 0.f}).x == doctest::Approx(1024.f / 4096.f));
    CHECK(sample({0.f, 0.f}).y == doctest::Approx(2048.f / 4096.f));
    CHECK(sample({1.f, 1.f}).x == doctest::Approx(1536.f / 4096.f));
    CHECK(sample({1.f, 1.f}).y == doctest::Approx(2560.f / 4096.f));

    const nvrhi::Viewport viewport = ShadowViewViewport(view);
    CHECK(viewport.minX == doctest::Approx(1024.f));
    CHECK(viewport.maxX == doctest::Approx(1536.f));
    CHECK(viewport.minY == doctest::Approx(2048.f));
    CHECK(viewport.maxY == doctest::Approx(2560.f));

    // An unfitted view divides by nothing rather than by zero.
    ShadowView empty;
    CHECK(ShadowViewUvScaleOffset(empty).x == doctest::Approx(1.f));
}

TEST_CASE("A packed view carries every lane the table promises")
{
    ShadowView view = ViewOf(BoxView());
    view.rect = ShadowViewRect{.x = 512, .y = 0, .width = 512, .height = 512};
    view.targetResolution = 2048;
    view.arraySlice = 3;
    view.depthBias = 0.002f;
    view.normalOffset = 0.05f;
    view.filterTapStepUv = 1.f / 2048.f;

    const ShadowViewGpu packed = PackShadowView(view);

    CHECK(packed.viewProjection == view.viewProjection);
    CHECK(packed.uvScaleOffset == ShadowViewUvScaleOffset(view));
    CHECK(packed.params.x == doctest::Approx(view.depthBias));
    CHECK(packed.params.y == doctest::Approx(view.normalOffset));
    CHECK(packed.params.z == doctest::Approx(view.filterTapStepUv));
    // The slice rides in a float lane because that is the form the array
    // sampler takes it in — no conversion at the sample site.
    CHECK(packed.params.w == doctest::Approx(3.f));
}

TEST_CASE("A cascade becomes a view that agrees with the cascade math")
{
    CascadeFitParams params;
    params.cameraView = glm::lookAt(glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f), glm::vec3(0.f, 1.f, 0.f));
    params.tanHalfFovY = 0.5773502692f;
    params.aspectRatio = 16.f / 9.f;
    params.nearZ = 0.1f;
    params.farZ = 200.f;
    params.lightDirection = glm::normalize(glm::vec3(-0.4f, -1.f, -0.3f));
    params.settings.cascadeCount = 4;
    params.settings.resolution = 2048;

    const CascadeFit fit = FitCascades(params);
    REQUIRE(fit.count == 4);

    for (std::uint32_t i = 0; i < fit.count; ++i)
    {
        CAPTURE(i);
        const ShadowView view = CascadeShadowView(fit.cascades[i], i, params.settings);

        // A cascade owns its whole slice, so its rect is the target and its
        // sampling transform is the identity.
        CHECK(view.viewProjection == fit.cascades[i].viewProjection);
        CHECK(view.rect.width == params.settings.resolution);
        CHECK(view.rect.height == params.settings.resolution);
        CHECK(view.targetResolution == params.settings.resolution);
        CHECK(view.arraySlice == i);
        CHECK(ShadowViewUvScaleOffset(view).x == doctest::Approx(1.f));

        // The biases arrive already scaled by this cascade's texel size, which
        // is the whole reason one setting holds across all of them.
        CHECK(view.depthBias == doctest::Approx(CascadeDepthBiasNdc(fit.cascades[i], params.settings)));
        CHECK(view.normalOffset == doctest::Approx(CascadeNormalOffsetWorld(fit.cascades[i], params.settings)));
        CHECK(view.filterTapStepUv == doctest::Approx(FilterTapStepUv(params.settings)));
    }

    // Far cascades cover more world per texel, so their world-space offsets are
    // larger from the same texel-quoted setting. A view that lost the scaling
    // would report the same offset for every cascade.
    const ShadowView near = CascadeShadowView(fit.cascades[0], 0, params.settings);
    const ShadowView far = CascadeShadowView(fit.cascades[3], 3, params.settings);
    CHECK(far.normalOffset > near.normalOffset);
}

TEST_CASE("A view culls the casters its frustum does not reach")
{
    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f), 1.f, 1),           // inside
        CasterAt(glm::vec3(500.f, 0.f, 0.f), 1.f, 2), // far outside
        CasterAt(glm::vec3(5.f, 5.f, 0.f), 1.f, 3),  // inside
    };

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    CHECK(list.culled == 1);
    CHECK(list.instances.size() == 2);
    CHECK(list.commands.size() == 2); // two distinct geometries, so two batches
    REQUIRE(list.viewCommandStart.size() == 2);
    CHECK(list.viewCommandStart[0] == 0);
    CHECK(list.viewCommandStart[1] == 2);
}

TEST_CASE("Consecutive instances of one geometry coalesce into a single draw")
{
    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    // Three of one geometry, then two of another — the geometry-major order the
    // gather sorts into.
    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f, 0.f, 0.f), 1.f, 7),  CasterAt(glm::vec3(1.f, 0.f, 0.f), 1.f, 7),
        CasterAt(glm::vec3(2.f, 0.f, 0.f), 1.f, 7),  CasterAt(glm::vec3(3.f, 0.f, 0.f), 1.f, 9),
        CasterAt(glm::vec3(4.f, 0.f, 0.f), 1.f, 9),
    };

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    REQUIRE(list.commands.size() == 2);
    CHECK(list.commands[0].instanceCount == 3);
    CHECK(list.commands[0].startInstanceLocation == 0);
    CHECK(list.commands[1].instanceCount == 2);
    CHECK(list.commands[1].startInstanceLocation == 3);
    CHECK(list.instances.size() == 5);

    // A command draws the geometry its key named, at the offsets the gather
    // resolved.
    CHECK(list.commands[0].indexCount == 36);
    CHECK(list.commands[0].startIndexLocation == 7u * 36u);
    CHECK(list.commands[1].startIndexLocation == 9u * 36u);
}

TEST_CASE("A culled caster breaks the run it was in the middle of")
{
    // The defect this guards: coalescing on the key alone would merge the two
    // surviving instances into one draw whose instance range spans the culled
    // one, drawing a caster this view rejected.
    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f, 0.f, 0.f), 1.f, 5),
        CasterAt(glm::vec3(500.f, 0.f, 0.f), 1.f, 5), // same geometry, out of frustum
        CasterAt(glm::vec3(2.f, 0.f, 0.f), 1.f, 5),
    };

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    CHECK(list.culled == 1);
    CHECK(list.instances.size() == 2);
    REQUIRE(list.commands.size() == 2);
    CHECK(list.commands[0].instanceCount == 1);
    CHECK(list.commands[1].instanceCount == 1);
    CHECK(list.commands[1].startInstanceLocation == 1);
}

TEST_CASE("Every view owns its own range of one frame's commands")
{
    // The property that lets a frame upload once however many views it draws:
    // all of them share one instance buffer, and each view's commands index
    // only its own run of it.
    const ShadowView wide = ViewOf(BoxView(10.f));
    const ShadowView narrow = ViewOf(BoxView(2.f));
    const ShadowDepthTarget targets[] = {TargetOf(wide), TargetOf(narrow)};

    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f), 0.5f, 1),           // in both
        CasterAt(glm::vec3(6.f, 0.f, 0.f), 0.5f, 2), // in the wide view only
    };

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    REQUIRE(list.viewCommandStart.size() == 3);
    CHECK(list.viewCommandStart[0] == 0);
    CHECK(list.viewCommandStart[1] == 2); // the wide view kept both
    CHECK(list.viewCommandStart[2] == 3); // the narrow view kept one
    CHECK(list.culled == 1);

    // An instance record per caster-view pair, not per caster: a caster in two
    // views is submitted twice, with a different matrix slot each time.
    CHECK(list.instances.size() == 3);

    // The second view's commands index the second view's instances, which start
    // after the first view's.
    CHECK(list.commands[list.viewCommandStart[1]].startInstanceLocation == 2);

    // Every command's instance range stays inside the buffer it indexes.
    for (const nvrhi::DrawIndexedIndirectArguments &command : list.commands)
    {
        CHECK(command.startInstanceLocation + command.instanceCount <= list.instances.size());
    }
}

TEST_CASE("A view into a sub-rect of a larger target draws like any other")
{
    // The shape the local-light atlas needs: the same renderer, the same
    // casters, a view that owns a rectangle rather than a whole target. Nothing
    // about the cull or the batching may depend on which of the two it is.
    const glm::mat4 viewProjection = BoxView();

    ShadowView whole = ViewOf(viewProjection);

    ShadowView tile;
    tile.viewProjection = viewProjection;
    tile.rect = ShadowViewRect{.x = 2048, .y = 512, .width = 256, .height = 256};
    tile.targetResolution = 4096;

    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f), 1.f, 1),
        CasterAt(glm::vec3(500.f, 0.f, 0.f), 1.f, 2),
        CasterAt(glm::vec3(3.f, 0.f, 0.f), 1.f, 3),
    };

    ShadowDrawList wholeList;
    ShadowDrawList tileList;
    const ShadowDepthTarget wholeTargets[] = {TargetOf(whole)};
    const ShadowDepthTarget tileTargets[] = {TargetOf(tile)};
    BuildShadowDrawList(wholeTargets, casters, wholeList);
    BuildShadowDrawList(tileTargets, casters, tileList);

    CHECK(wholeList.culled == tileList.culled);
    CHECK(wholeList.instances.size() == tileList.instances.size());
    REQUIRE(wholeList.commands.size() == tileList.commands.size());
    for (std::size_t i = 0; i < wholeList.commands.size(); ++i)
    {
        CAPTURE(i);
        CHECK(wholeList.commands[i].indexCount == tileList.commands[i].indexCount);
        CHECK(wholeList.commands[i].instanceCount == tileList.commands[i].instanceCount);
        CHECK(wholeList.commands[i].startInstanceLocation == tileList.commands[i].startInstanceLocation);
    }

    // What differs is only where it lands, and that is the viewport alone.
    const nvrhi::Viewport viewport = ShadowViewViewport(tile);
    CHECK(viewport.minX == doctest::Approx(2048.f));
    CHECK(viewport.maxX == doctest::Approx(2304.f));
    CHECK(viewport.minY == doctest::Approx(512.f));
    CHECK(viewport.maxY == doctest::Approx(768.f));
}

TEST_CASE("Casters that only look alike are not merged")
{
    // A shared key with a different draw range is what a mesh that never got an
    // id would produce. Merging those draws one caster's geometry at the
    // other's place, and nothing downstream would report it — so the batch test
    // is the draw itself, not the key alone.
    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    ShadowCaster first = CasterAt(glm::vec3(0.f), 1.f, 0);
    ShadowCaster second = CasterAt(glm::vec3(2.f, 0.f, 0.f), 1.f, 0);
    second.startIndexLocation = 720; // same key, different geometry
    REQUIRE(first.geometryKey == second.geometryKey);
    CHECK_FALSE(SameShadowGeometry(first, second));

    ShadowDrawList list;
    BuildShadowDrawList(targets, std::vector<ShadowCaster>{first, second}, list);
    CHECK(list.commands.size() == 2);

    // And two that really are the same geometry still coalesce.
    ShadowDrawList same;
    BuildShadowDrawList(targets, std::vector<ShadowCaster>{first, CasterAt(glm::vec3(2.f, 0.f, 0.f), 1.f, 0)}, same);
    REQUIRE(same.commands.size() == 1);
    CHECK(same.commands[0].instanceCount == 2);
}

TEST_CASE("Commands record the buffers they draw from, so runs split on the arena")
{
    // Two arenas is the case a single multi-draw cannot serve: the vertex and
    // index bindings differ, so the submission has to break between them.
    auto *const arenaA = reinterpret_cast<nvrhi::IBuffer *>(std::uintptr_t{0x1000});
    auto *const arenaAIndices = reinterpret_cast<nvrhi::IBuffer *>(std::uintptr_t{0x2000});
    auto *const arenaB = reinterpret_cast<nvrhi::IBuffer *>(std::uintptr_t{0x3000});
    auto *const arenaBIndices = reinterpret_cast<nvrhi::IBuffer *>(std::uintptr_t{0x4000});

    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    const std::vector<ShadowCaster> casters = {
        CasterAt(glm::vec3(0.f), 1.f, 1, arenaA, arenaAIndices),
        CasterAt(glm::vec3(2.f, 0.f, 0.f), 1.f, 2, arenaA, arenaAIndices),
        CasterAt(glm::vec3(4.f, 0.f, 0.f), 1.f, 3, arenaB, arenaBIndices),
    };

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    REQUIRE(list.commands.size() == 3);
    REQUIRE(list.commandVertexBuffers.size() == 3);
    CHECK(list.commandVertexBuffers[0] == arenaA);
    CHECK(list.commandVertexBuffers[1] == arenaA);
    CHECK(list.commandVertexBuffers[2] == arenaB);
    CHECK(list.commandIndexBuffers[2] == arenaBIndices);
}

TEST_CASE("Rebuilding the draw list leaves nothing of the last one")
{
    // The list is kept across frames so a steady state allocates nothing, which
    // only works if a refill is a full reset — a stale command range would draw
    // last frame's geometry into this frame's map.
    const ShadowView view = ViewOf(BoxView());
    const ShadowDepthTarget targets[] = {TargetOf(view)};

    ShadowDrawList list;
    BuildShadowDrawList(targets, std::vector<ShadowCaster>{CasterAt(glm::vec3(0.f), 1.f, 1)}, list);
    REQUIRE(list.commands.size() == 1);
    REQUIRE(list.culled == 0);

    BuildShadowDrawList(targets, std::vector<ShadowCaster>{CasterAt(glm::vec3(500.f, 0.f, 0.f), 1.f, 1)}, list);
    CHECK(list.commands.empty());
    CHECK(list.instances.empty());
    CHECK(list.commandVertexBuffers.empty());
    CHECK(list.culled == 1);
    REQUIRE(list.viewCommandStart.size() == 2);
    CHECK(list.viewCommandStart[1] == 0);
}
