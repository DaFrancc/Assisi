/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/LocalShadowCache.hpp>
#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowDiagnostics.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowImportance.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

using namespace Assisi::Render;

namespace
{
LocalShadowCandidate Candidate(LocalLightKind kind, std::uint32_t index, float coverage = 0.5f)
{
    return LocalShadowCandidate{.kind = kind, .lightIndex = index, .screenCoverage = coverage, .intensity = 1.f};
}

LocalShadowRequest Request(LocalLightKind kind, std::uint32_t index, std::uint32_t sizeClass)
{
    return LocalShadowRequest{.kind = kind, .lightIndex = index, .pose = {}, .sizeClass = sizeClass};
}

/// The state the report gives light @p kind / @p index, or CappedOut when there
/// is no report at all — which the tests distinguish by asserting on Find()
/// directly where that matters.
LocalShadowState StateOf(const ShadowDiagnostics &diagnostics, LocalLightKind kind, std::uint32_t index)
{
    const LocalShadowLightReport *report = diagnostics.Find(kind, index);
    REQUIRE(report != nullptr);
    return report->state;
}

/// A caster the frustum tests can accept or reject on its sphere alone.
ShadowCaster CasterAt(const glm::vec3 &center, float radius, std::uint32_t viewMask)
{
    ShadowCaster caster;
    caster.geometryKey = ShadowGeometryKey(1u, 0u);
    caster.indexCount = 3u;
    caster.worldSphere = Assisi::Geometry::BoundingSphere{center, radius};
    caster.model = glm::translate(glm::mat4(1.f), center);
    caster.viewMask = viewMask;
    return caster;
}

/// An orthographic view looking down -Z at @p center, wide enough to hold a
/// caster standing there and tight enough to exclude one standing far off.
ShadowView ViewAt(const glm::vec3 &center, float halfExtent)
{
    const glm::mat4 projection = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent, 0.1f, 100.f);
    const glm::mat4 view = glm::lookAt(center + glm::vec3(0.f, 0.f, 10.f), center, glm::vec3(0.f, 1.f, 0.f));
    ShadowView out;
    out.viewProjection = projection * view;
    out.rect = ShadowViewRect{.x = 0, .y = 0, .width = 256, .height = 256};
    return out;
}
} // namespace

TEST_CASE("A light that holds the tile it asked for reports as shadowed")
{
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Spot, 0, 2u)};
    const std::vector<LocalShadowServedTile> served{
        LocalShadowServedTile{.requestIndex = 0u, .resolution = ShadowSizeClassResolution(2u)}};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = requests,
                                                            .plans = {},
                                                            .served = served},
                                diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 0u) == LocalShadowState::Shadowed);
    CHECK(diagnostics.shadowedLights == 1u);
    CHECK(diagnostics.castingLights == 1u);
}

TEST_CASE("A light the cap turned away is named, not merely missing")
{
    // The whole point of the readout: the light is absent from the requests
    // entirely, so without this it would have no row at all and the author would
    // be left comparing the panel's count against their own scene.
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0),
                                                       Candidate(LocalLightKind::Spot, 1)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Spot, 0, 2u)};
    const std::vector<LocalShadowServedTile> served{
        LocalShadowServedTile{.requestIndex = 0u, .resolution = ShadowSizeClassResolution(2u)}};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = requests,
                                                            .plans = {},
                                                            .served = served},
                                diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 1u) == LocalShadowState::CappedOut);
    CHECK(diagnostics.Find(LocalLightKind::Spot, 1u)->resolution == 0u);
    CHECK(diagnostics.shadowedLights == 1u);
    CHECK(diagnostics.castingLights == 2u);
}

TEST_CASE("A smaller tile than the light asked for reports as demoted, with both sizes")
{
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Point, 3)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Point, 3, 3u)};
    const std::vector<LocalShadowServedTile> served{
        LocalShadowServedTile{.requestIndex = 0u, .resolution = ShadowSizeClassResolution(1u)}};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = requests,
                                                            .plans = {},
                                                            .served = served},
                                diagnostics);

    const LocalShadowLightReport *report = diagnostics.Find(LocalLightKind::Point, 3u);
    REQUIRE(report != nullptr);
    CHECK(report->state == LocalShadowState::Demoted);
    CHECK(report->resolution == ShadowSizeClassResolution(1u));
    CHECK(report->requestedResolution == ShadowSizeClassResolution(3u));
    // A demoted light still holds a tile, so it counts toward the N of "N of M".
    CHECK(diagnostics.shadowedLights == 1u);
}

TEST_CASE("A light the budget refused is told apart from one the atlas could not fit")
{
    // Two lights that both went unshadowed, for two different reasons and two
    // different settings. One number for both would name neither.
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0),
                                                       Candidate(LocalLightKind::Spot, 1)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Spot, 0, 2u),
                                                   Request(LocalLightKind::Spot, 1, 2u)};
    std::vector<LocalShadowTilePlan> plans(2);
    plans[0].deferred = true;
    plans[1].deferred = false;

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = requests,
                                                            .plans = plans,
                                                            .served = {},
                                                            .deferredFaces = 1u,
                                                            .budgetFaces = 4u},
                                diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 0u) == LocalShadowState::Deferred);
    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 1u) == LocalShadowState::AtlasFull);
    CHECK(diagnostics.shadowedLights == 0u);
    CHECK(diagnostics.BudgetSaturated());
    CHECK(diagnostics.deferredFaces == 1u);
    CHECK(diagnostics.budgetFaces == 4u);
}

TEST_CASE("With no plans at all an unserved light reports the atlas, not the budget")
{
    // The uncached path plans nothing, and nothing there can defer a light — so
    // reading a missing plan as "deferred" would blame a budget that never ran.
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Spot, 0, 2u)};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(
        LocalShadowDiagnosticsFrame{.candidates = candidates, .requests = requests, .plans = {}, .served = {}},
        diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 0u) == LocalShadowState::AtlasFull);
    CHECK_FALSE(diagnostics.BudgetSaturated());
}

TEST_CASE("Spot and point lights of the same buffer row are different lights")
{
    // Both kinds index their own GPU buffer from zero, so a lookup on the index
    // alone would hand a spot light the point light's verdict.
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0),
                                                       Candidate(LocalLightKind::Point, 0)};
    const std::vector<LocalShadowRequest> requests{Request(LocalLightKind::Point, 0, 2u)};
    const std::vector<LocalShadowServedTile> served{
        LocalShadowServedTile{.requestIndex = 0u, .resolution = ShadowSizeClassResolution(2u)}};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = requests,
                                                            .plans = {},
                                                            .served = served},
                                diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Point, 0u) == LocalShadowState::Shadowed);
    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 0u) == LocalShadowState::CappedOut);
}

TEST_CASE("A light nothing gathered has no report rather than a wrong one")
{
    ShadowDiagnostics diagnostics;
    CHECK(diagnostics.Find(LocalLightKind::Spot, 0u) == nullptr);

    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0)};
    BuildLocalShadowDiagnostics(
        LocalShadowDiagnosticsFrame{.candidates = candidates, .requests = {}, .plans = {}, .served = {}}, diagnostics);
    CHECK(diagnostics.Find(LocalLightKind::Spot, 7u) == nullptr);
}

TEST_CASE("Rebuilding the report leaves nothing of the last one")
{
    const std::vector<LocalShadowCandidate> many{Candidate(LocalLightKind::Spot, 0),
                                                 Candidate(LocalLightKind::Spot, 1),
                                                 Candidate(LocalLightKind::Spot, 2)};
    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(
        LocalShadowDiagnosticsFrame{.candidates = many, .requests = {}, .plans = {}, .served = {}}, diagnostics);
    REQUIRE(diagnostics.lights.size() == 3u);

    const std::vector<LocalShadowCandidate> few{Candidate(LocalLightKind::Spot, 0)};
    BuildLocalShadowDiagnostics(
        LocalShadowDiagnosticsFrame{.candidates = few, .requests = {}, .plans = {}, .served = {}}, diagnostics);
    CHECK(diagnostics.lights.size() == 1u);
    CHECK(diagnostics.castingLights == 1u);
    CHECK(diagnostics.Find(LocalLightKind::Spot, 2u) == nullptr);
}

TEST_CASE("Shadows switched off is not the cap turning every light away")
{
    // With the pass inactive nothing was selected, so every light is absent
    // from the requests — which is exactly what being capped out looks like.
    // Blaming the cap for a feature the author switched off names the wrong
    // knob, and the count of lights wanting a shadow is still a fact about the
    // level rather than about the checkbox.
    const std::vector<LocalShadowCandidate> candidates{Candidate(LocalLightKind::Spot, 0),
                                                       Candidate(LocalLightKind::Point, 1)};

    ShadowDiagnostics diagnostics;
    BuildLocalShadowDiagnostics(LocalShadowDiagnosticsFrame{.candidates = candidates,
                                                            .requests = {},
                                                            .plans = {},
                                                            .served = {},
                                                            .deferredFaces = 0,
                                                            .budgetFaces = 0,
                                                            .active = false},
                                diagnostics);

    CHECK(StateOf(diagnostics, LocalLightKind::Spot, 0u) == LocalShadowState::Disabled);
    CHECK(StateOf(diagnostics, LocalLightKind::Point, 1u) == LocalShadowState::Disabled);
    CHECK(diagnostics.shadowedLights == 0u);
    CHECK(diagnostics.castingLights == 2u);
}

TEST_CASE("Every state describes itself in words an author can act on")
{
    // A state name alone says something is wrong without saying what to do, and
    // an empty string in the panel would be worse than no line at all.
    for (const LocalShadowState state : {LocalShadowState::Shadowed, LocalShadowState::Demoted,
                                         LocalShadowState::Deferred, LocalShadowState::AtlasFull,
                                         LocalShadowState::CappedOut, LocalShadowState::Disabled})
    {
        const std::string_view text = DescribeLocalShadowState(state);
        CHECK_FALSE(text.empty());
    }
}

TEST_CASE("A view's caster count is what that view actually drew")
{
    // The number the panel prints per sun cascade. Two views over the same
    // caster span, one holding both casters and one holding neither of the pair
    // outside it — a count taken off the whole span would report the same
    // number twice and hide exactly the near/far split it exists to show.
    const std::vector<ShadowDepthTarget> targets{ShadowDepthTarget{.view = ViewAt(glm::vec3(0.f), 4.f)},
                                                 ShadowDepthTarget{.view = ViewAt(glm::vec3(100.f, 0.f, 0.f), 4.f)}};
    const std::vector<ShadowCaster> casters{CasterAt(glm::vec3(0.f), 1.f, ~0u),
                                            CasterAt(glm::vec3(1.f, 0.f, 0.f), 1.f, ~0u),
                                            CasterAt(glm::vec3(100.f, 0.f, 0.f), 1.f, ~0u)};

    ShadowDrawList list;
    BuildShadowDrawList(targets, casters, list);

    CHECK(ShadowViewCasterCount(list, 0u) == 2u);
    CHECK(ShadowViewCasterCount(list, 1u) == 1u);
    // A view the list does not describe counts nothing rather than reading off
    // the end of it.
    CHECK(ShadowViewCasterCount(list, 2u) == 0u);
}
