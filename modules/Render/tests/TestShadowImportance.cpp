/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/ShadowAtlas.hpp>
#include <Assisi/Render/ShadowImportance.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace Assisi::Render;

namespace
{
LocalShadowCandidate SpotAt(std::uint32_t index, float coverage, float intensity = 1.f)
{
    return LocalShadowCandidate{.kind = LocalLightKind::Spot,
                                .lightIndex = index,
                                .screenCoverage = coverage,
                                .intensity = intensity};
}

LocalShadowCandidate PointAt(std::uint32_t index, float coverage, float intensity = 1.f)
{
    return LocalShadowCandidate{.kind = LocalLightKind::Point,
                                .lightIndex = index,
                                .screenCoverage = coverage,
                                .intensity = intensity};
}

/// Whether @p selection holds the light @p kind / @p index.
bool Holds(const LocalShadowSelection &selection, LocalLightKind kind, std::uint32_t index)
{
    return std::any_of(selection.lights.begin(), selection.lights.end(),
                       [&](const LocalShadowAssignment &light)
                       { return light.kind == kind && light.lightIndex == index; });
}

/// Settings whose caps bind at the given counts, with the size-class hysteresis
/// out of the way so a test about the cap is only about the cap.
LocalShadowSelectionSettings CapsOf(std::uint32_t spots, std::uint32_t points, float capHysteresis = 0.f)
{
    LocalShadowSelectionSettings settings;
    settings.capSpot = spots;
    settings.capPoint = points;
    settings.capHysteresis = capHysteresis;
    settings.classHysteresis = 0.f;
    return settings;
}
} // namespace

TEST_CASE("A light's score is its coverage squared times its intensity")
{
    // Coverage is a length across the screen and what matters is the area, so
    // halving the coverage quarters the score. A score linear in coverage would
    // over-value distant lights by the same factor.
    const float near = LocalShadowScore(SpotAt(0, 0.4f));
    const float far = LocalShadowScore(SpotAt(1, 0.2f));
    CHECK(far == doctest::Approx(near * 0.25f));

    // Intensity is linear in it.
    CHECK(LocalShadowScore(SpotAt(0, 0.4f, 2.f)) == doctest::Approx(near * 2.f));
}

TEST_CASE("A subtractive light is scored by how much it changes the image, not its sign")
{
    // A negative-intensity light occludes exactly as much as a positive one, and
    // its shadow is as visible — scoring it by the raw intensity would sort it
    // below every unlit light in the level.
    CHECK(LocalShadowScore(SpotAt(0, 0.5f, -2.f)) == doctest::Approx(LocalShadowScore(SpotAt(0, 0.5f, 2.f))));
    CHECK(LocalShadowScore(SpotAt(0, 0.5f, -2.f)) > 0.f);
}

TEST_CASE("Priority biases the score in octaves")
{
    const float plain = LocalShadowScore(SpotAt(0, 0.5f));

    LocalShadowCandidate doubled = SpotAt(0, 0.5f);
    doubled.priority = 1.f;
    CHECK(LocalShadowScore(doubled) == doctest::Approx(plain * 2.f));

    LocalShadowCandidate halved = SpotAt(0, 0.5f);
    halved.priority = -1.f;
    CHECK(LocalShadowScore(halved) == doctest::Approx(plain * 0.5f));

    // An octave means the same thing at every range, which is the point of the
    // knob being octaves rather than an addition: a +1 light at a quarter the
    // coverage still sorts below a plain one at full coverage.
    LocalShadowCandidate distantBoosted = SpotAt(0, 0.25f);
    distantBoosted.priority = 1.f;
    CHECK(LocalShadowScore(distantBoosted) < plain);
}

TEST_CASE("An invisible or non-finite light scores nothing")
{
    LocalShadowCandidate offscreen = SpotAt(0, 0.9f);
    offscreen.visible = false;
    CHECK(LocalShadowScore(offscreen) == 0.f);

    // A NaN would sort unpredictably against everything and take an arbitrary
    // light's shadow with it.
    CHECK(LocalShadowScore(SpotAt(0, std::numeric_limits<float>::quiet_NaN())) == 0.f);
    CHECK(LocalShadowScore(SpotAt(0, 0.5f, std::numeric_limits<float>::infinity())) == 0.f);
}

TEST_CASE("The caps are per type, so a spot never displaces a point")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    std::vector<LocalShadowCandidate> candidates;
    for (std::uint32_t i = 0; i < 4; ++i)
    {
        candidates.push_back(SpotAt(i, 0.5f));
        candidates.push_back(PointAt(i, 0.5f));
    }

    selector.Select(candidates, LocalShadowSettings{}, CapsOf(2, 1), selection);

    // Two spots and one point, whatever their scores relative to each other: a
    // point light is six renders against a spot's one, so a single combined cap
    // would mean wildly different work depending on the mix.
    CHECK(selection.spotCount == 2);
    CHECK(selection.pointCount == 1);
    CHECK(selection.droppedByCap == 5);
}

TEST_CASE("The cap keeps the lights that matter most")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    const std::vector<LocalShadowCandidate> candidates{SpotAt(0, 0.1f), SpotAt(1, 0.9f), SpotAt(2, 0.5f)};
    selector.Select(candidates, LocalShadowSettings{}, CapsOf(2, 0), selection);

    REQUIRE(selection.lights.size() == 2);
    // Most important first, because the allocator serves them in that order —
    // which is what makes an overflow demote the least important light without a
    // separate demotion pass.
    CHECK(selection.lights[0].lightIndex == 1);
    CHECK(selection.lights[1].lightIndex == 2);
    CHECK_FALSE(Holds(selection, LocalLightKind::Spot, 0));
}

TEST_CASE("A pinned light keeps its shadow under pressure that drops its neighbours")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    // The dimmest light in the scene, and the only one pinned.
    LocalShadowCandidate keyLight = SpotAt(0, 0.05f);
    keyLight.pinned = true;

    const std::vector<LocalShadowCandidate> candidates{keyLight, SpotAt(1, 0.9f), SpotAt(2, 0.8f),
                                                       SpotAt(3, 0.7f)};
    selector.Select(candidates, LocalShadowSettings{}, CapsOf(2, 0), selection);

    // The pin outranks the cap outright rather than raising the light's score,
    // because what a score has to beat depends on what else the level places.
    CHECK(Holds(selection, LocalLightKind::Spot, 0));
    CHECK(selection.lights.front().lightIndex == 0);
    CHECK(Holds(selection, LocalLightKind::Spot, 1));
    CHECK_FALSE(Holds(selection, LocalLightKind::Spot, 3));
}

TEST_CASE("Hysteresis keeps a holder against a challenger that barely outscores it")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    const LocalShadowSelectionSettings settings = CapsOf(1, 0, /*capHysteresis=*/ 0.15f);

    // Light 0 wins the first frame and becomes the holder.
    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, 0.50f), SpotAt(1, 0.40f)}, LocalShadowSettings{},
                    settings, selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().lightIndex == 0);

    // Now light 1 edges ahead — but by less than the margin, so nothing moves.
    // Without this the two swap every frame their scores cross, and swapping is
    // visible: the loser's shadow disappears.
    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, 0.50f), SpotAt(1, 0.52f)}, LocalShadowSettings{},
                    settings, selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().lightIndex == 0);

    // Clearly ahead, and it takes the tile: the margin resists a flutter, it
    // does not freeze the ordering.
    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, 0.50f), SpotAt(1, 0.90f)}, LocalShadowSettings{},
                    settings, selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().lightIndex == 1);
}

TEST_CASE("Disabling the cap drops the ordering, not the lights")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    LocalShadowSelectionSettings settings = CapsOf(1, 1);
    settings.capEnabled = false;

    const std::vector<LocalShadowCandidate> candidates{SpotAt(0, 0.1f), SpotAt(1, 0.9f), PointAt(0, 0.5f),
                                                       PointAt(1, 0.4f)};
    selector.Select(candidates, LocalShadowSettings{}, settings, selection);

    // Everyone is a winner, in arrival order — the atlas is still a fixed
    // texture, so the allocator is what runs out, and whoever came last goes
    // without rather than whoever matters least.
    CHECK(selection.lights.size() == 4);
    CHECK(selection.droppedByCap == 0);
    CHECK(selection.lights[0].lightIndex == 0);
    CHECK(selection.lights[1].lightIndex == 1);
}

TEST_CASE("An invisible light holds no tile")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    LocalShadowCandidate behindCamera = SpotAt(0, 0.9f);
    behindCamera.visible = false;

    selector.Select(std::vector<LocalShadowCandidate>{behindCamera, SpotAt(1, 0.2f)}, LocalShadowSettings{},
                    CapsOf(4, 0), selection);

    // Not merely outranked — absent. A light nothing can see would otherwise
    // hold a tile against a light that is on screen.
    CHECK(selection.lights.size() == 1);
    CHECK(selection.lights.front().lightIndex == 1);
}

TEST_CASE("Shadows off selects nothing at all")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    LocalShadowSettings local;
    local.enabled = false;

    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, 0.9f)}, local, CapsOf(8, 8), selection);
    CHECK(selection.lights.empty());
    CHECK(selection.spotCount == 0);
}

TEST_CASE("A light too small to fill its tile takes a smaller class")
{
    const std::uint32_t baseClass = ShadowSizeClassOf(512);

    // At the reference coverage the tier's face resolution is what the light
    // earns, and every halving below it drops one class — which keeps a map's
    // texels roughly matched to the screen pixels that will read them.
    CHECK(StableSizeClass(baseClass, kFaceCoverageReference, kNoPreviousSizeClass, 0.f) == baseClass);
    CHECK(StableSizeClass(baseClass, kFaceCoverageReference * 0.5f, kNoPreviousSizeClass, 0.f) == baseClass - 1u);
    CHECK(StableSizeClass(baseClass, kFaceCoverageReference * 0.25f, kNoPreviousSizeClass, 0.f) == baseClass - 2u);

    // A light filling the screen does not get more than the tier allows: the
    // face resolution is what the tier's memory figure was computed from.
    CHECK(StableSizeClass(baseClass, 1.f, kNoPreviousSizeClass, 0.f) == baseClass);

    // And it never goes below the smallest class there is.
    CHECK(StableSizeClass(baseClass, 1e-6f, kNoPreviousSizeClass, 0.f) == 0);
}

TEST_CASE("A tile is not resized until demand clears the boundary by the margin")
{
    const std::uint32_t baseClass = ShadowSizeClassOf(512);
    const float hysteresis = 0.25f;

    // Holding the base class, with coverage just under the boundary that would
    // demote it. A resize throws the tile's contents away, so a light hovering
    // on a boundary must not resize every other frame.
    const float boundary = kFaceCoverageReference;
    CHECK(StableSizeClass(baseClass, boundary * 0.95f, baseClass, hysteresis) == baseClass);
    // Clearly below, and it demotes.
    CHECK(StableSizeClass(baseClass, boundary * 0.5f, baseClass, hysteresis) == baseClass - 1u);

    // The margin works in both directions: a light holding the smaller class and
    // creeping back up keeps it until it is properly past the boundary.
    CHECK(StableSizeClass(baseClass, boundary * 1.05f, baseClass - 1u, hysteresis) == baseClass - 1u);
    CHECK(StableSizeClass(baseClass, boundary * 2.f, baseClass - 1u, hysteresis) == baseClass);

    // Without a margin the same coverage moves immediately, which is what says
    // the margin above is the thing doing the work.
    CHECK(StableSizeClass(baseClass, boundary * 0.95f, baseClass, 0.f) == baseClass - 1u);
}

TEST_CASE("The selection remembers a light's class across frames")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    LocalShadowSelectionSettings settings = CapsOf(4, 0);
    settings.classHysteresis = 0.25f;

    LocalShadowSettings local;
    local.faceResolution = 512;
    const std::uint32_t baseClass = ShadowSizeClassOf(512);

    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, kFaceCoverageReference)}, local, settings,
                    selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().sizeClass == baseClass);

    // A small drift in coverage must not resize the tile the light already holds
    // — and it can only know that by remembering last frame's answer.
    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, kFaceCoverageReference * 0.95f)}, local, settings,
                    selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().sizeClass == baseClass);

    // Forget() drops the memory, so the next answer is the raw demand — what a
    // settings change or a level load wants, since the remembered class was
    // sized against an atlas that no longer exists.
    selector.Forget();
    selector.Select(std::vector<LocalShadowCandidate>{SpotAt(0, kFaceCoverageReference * 0.95f)}, local, settings,
                    selection);
    REQUIRE(selection.lights.size() == 1);
    CHECK(selection.lights.front().sizeClass == baseClass - 1u);
}

TEST_CASE("A frame's selection is the same however the candidates are ordered")
{
    LocalShadowSelector forward;
    LocalShadowSelector reversed;
    LocalShadowSelection forwardSelection;
    LocalShadowSelection reversedSelection;

    std::vector<LocalShadowCandidate> candidates{SpotAt(0, 0.9f), SpotAt(1, 0.5f), SpotAt(2, 0.7f),
                                                 SpotAt(3, 0.3f)};
    forward.Select(candidates, LocalShadowSettings{}, CapsOf(2, 0), forwardSelection);

    std::reverse(candidates.begin(), candidates.end());
    reversed.Select(candidates, LocalShadowSettings{}, CapsOf(2, 0), reversedSelection);

    // The scene query's order is not something a level author controls, so an
    // ordering that depended on it would make the same scene shadow differently
    // for reasons nobody could see.
    REQUIRE(forwardSelection.lights.size() == reversedSelection.lights.size());
    for (std::size_t i = 0; i < forwardSelection.lights.size(); ++i)
    {
        CHECK(forwardSelection.lights[i].lightIndex == reversedSelection.lights[i].lightIndex);
    }
}

TEST_CASE("A cap above the scene's light count decides nothing")
{
    LocalShadowSelector selector;
    LocalShadowSelection selection;

    const LocalShadowSelectionSettings defaults;

    std::vector<LocalShadowCandidate> candidates;
    for (std::uint32_t i = 0; i < defaults.capSpot; ++i)
    {
        candidates.push_back(SpotAt(i, 0.3f));
    }
    for (std::uint32_t i = 0; i < defaults.capPoint; ++i)
    {
        candidates.push_back(PointAt(i, 0.3f));
    }

    // A scene right at the defaults is served whole. Nothing is turned away,
    // which is what "the cap does not bind" looks like as a number rather than
    // an impression — and the counter it is read off is droppedByCap.
    selector.Select(candidates, LocalShadowSettings{}, defaults, selection);
    CHECK(selection.droppedByCap == 0);
    CHECK(selection.spotCount == defaults.capSpot);
    CHECK(selection.pointCount == defaults.capPoint);

    // One more of each and the cap is exactly what turns them away, which is
    // what says the zero above is the scene fitting rather than the cap being
    // absent.
    candidates.push_back(SpotAt(defaults.capSpot, 0.3f));
    candidates.push_back(PointAt(defaults.capPoint, 0.3f));
    selector.Select(candidates, LocalShadowSettings{}, defaults, selection);
    CHECK(selection.droppedByCap == 2);
}
