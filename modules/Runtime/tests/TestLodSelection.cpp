/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <array>
#include <vector>

#include <doctest/doctest.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/LodSelection.hpp>

using Assisi::ECS::Entity;
using Assisi::Geometry::BoundingSphere;
using Assisi::Geometry::DefaultLodScreenSize;
using Assisi::Geometry::LodRange;
using Assisi::Runtime::DescribeLodSelection;
using Assisi::Runtime::LodReport;
using Assisi::Runtime::LodScreenSize;
using Assisi::Runtime::LodSelector;
using Assisi::Runtime::LodSettings;
using Assisi::Runtime::LodView;
using Assisi::Runtime::SelectLodLevel;

namespace
{
// A chain with explicit thresholds — 0.5, 0.25, 0.125 — so the cases below read
// against numbers they state rather than against whatever the authored default
// happens to be. The default series has its own test.
std::vector<LodRange> Chain(uint32_t levels)
{
    std::vector<LodRange> lods;
    float threshold = 0.5f;
    for (uint32_t level = 0; level < levels; ++level)
    {
        lods.push_back(LodRange{.FirstSubMesh = level, .SubMeshCount = 1, .ScreenSizeThreshold = threshold});
        threshold *= 0.5f;
    }
    return lods;
}

// A camera at the origin, 60 degrees vertical: tan(30 deg).
LodView Camera()
{
    return LodView{.cameraPosition = glm::vec3(0.f), .tanHalfFovY = 0.5773502692f};
}

BoundingSphere SphereAt(float distance, float radius)
{
    return BoundingSphere{.center = glm::vec3(0.f, 0.f, -distance), .radius = radius};
}
} // namespace

TEST_CASE("LodScreenSize is the sphere's diameter over the viewport height")
{
    const LodView view = Camera();

    // radius / (distance * tanHalfFovY): a unit-radius sphere ten metres out
    // through a 60-degree lens covers 1 / (10 * tan30) of the height.
    CHECK(LodScreenSize(SphereAt(10.f, 1.f), view) == doctest::Approx(1.f / (10.f * view.tanHalfFovY)));

    // Twice as far is half the size; twice as big is twice the size.
    CHECK(LodScreenSize(SphereAt(20.f, 1.f), view) ==
          doctest::Approx(0.5f * LodScreenSize(SphereAt(10.f, 1.f), view)));
    CHECK(LodScreenSize(SphereAt(10.f, 2.f), view) ==
          doctest::Approx(2.f * LodScreenSize(SphereAt(10.f, 1.f), view)));
}

TEST_CASE("LodScreenSize saturates when the camera is inside the sphere")
{
    // Not an unbounded ratio and not a division by zero: the thing fills the
    // view, which is all the selection needs to know.
    CHECK(LodScreenSize(SphereAt(0.f, 1.f), Camera()) == doctest::Approx(1.f));
    CHECK(LodScreenSize(SphereAt(0.5f, 1.f), Camera()) == doctest::Approx(1.f));
}

TEST_CASE("LodScreenSize never passes 1 and never falls as the camera closes in")
{
    // The object fills the view at radius / tan(fovY/2) — 1.73 radii through a
    // 60-degree lens, well outside the sphere. Capping only once the camera was
    // inside let the ratio climb past 1 over that stretch and then snap back to
    // it at the surface: a number that overshot and ran backwards.
    const LodView view = Camera();
    float previous = 0.f;
    for (float distance = 20.f; distance > 0.f; distance -= 0.01f)
    {
        const float size = LodScreenSize(SphereAt(distance, 1.f), view);
        CHECK(size <= 1.f);
        CHECK(size >= previous);
        previous = size;
    }

    // Exactly filling it is 1, and not a moment before.
    CHECK(LodScreenSize(SphereAt(1.f / view.tanHalfFovY, 1.f), view) == doctest::Approx(1.f));
    CHECK(LodScreenSize(SphereAt(1.05f / view.tanHalfFovY, 1.f), view) < 1.f);
}

TEST_CASE("An unset view measures nothing and selects LOD0")
{
    // Zero is "no measurement", not "vanishingly small" — the difference is the
    // whole chain, since the smallest size selects the coarsest level.
    CHECK(LodScreenSize(SphereAt(10.f, 1.f), LodView{}) == doctest::Approx(0.f));

    // A selector that was never given a frame's view is in exactly that state.
    LodSelector selector;
    const Entity entity{.index = 1, .generation = 1};
    CHECK(selector.Select(entity, Chain(3), SphereAt(10.f, 1.f)) == 0);
}

TEST_CASE("SelectLodLevel takes the finest level the instance is big enough for")
{
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    CHECK(SelectLodLevel(lods, 0.9f, 0, settings) == 0);
    CHECK(SelectLodLevel(lods, 0.5f, 0, settings) == 0);  // exactly at LOD0's threshold
    CHECK(SelectLodLevel(lods, 0.4f, 0, settings) == 1);
    CHECK(SelectLodLevel(lods, 0.25f, 0, settings) == 1);
    CHECK(SelectLodLevel(lods, 0.2f, 0, settings) == 2);
    CHECK(SelectLodLevel(lods, 0.01f, 0, settings) == 2); // past the last threshold, clamped
}

TEST_CASE("SelectLodLevel keeps a single-level mesh on LOD0 at every size")
{
    const std::vector<LodRange> lods = Chain(1);
    const LodSettings settings;

    CHECK(SelectLodLevel(lods, 1.f, 0, settings) == 0);
    CHECK(SelectLodLevel(lods, 0.001f, 0, settings) == 0);
    CHECK(SelectLodLevel({}, 0.001f, 0, settings) == 0); // no table at all
}

TEST_CASE("SelectLodLevel holds the dead band around the level it was last at")
{
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings; // hysteresis 0.02

    // Coming down from LOD0 the switch happens at the threshold itself.
    CHECK(SelectLodLevel(lods, 0.499f, 0, settings) == 1);

    // Going back up it does not, until the threshold plus the band: sitting at
    // LOD1 anywhere below 0.51 stays at LOD1.
    CHECK(SelectLodLevel(lods, 0.499f, 1, settings) == 1);
    CHECK(SelectLodLevel(lods, 0.505f, 1, settings) == 1);
    CHECK(SelectLodLevel(lods, 0.51f, 1, settings) == 0);
}

TEST_CASE("SelectLodLevel returns its own answer unchanged")
{
    // What lets draw-extract and the shadow gathers both select the same
    // instance in one frame without handing each other different levels.
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    for (float size = 0.01f; size < 1.f; size += 0.005f)
    {
        for (uint32_t previous = 0; previous < 3; ++previous)
        {
            const uint32_t once = SelectLodLevel(lods, size, previous, settings);
            CHECK(SelectLodLevel(lods, size, once, settings) == once);
        }
    }
}

TEST_CASE("An instance jittering on a boundary settles instead of flickering")
{
    // The acceptance criterion, as a loop: park an instance where the raw
    // compare would flip, shake it by a tenth of a percent, and count switches.
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    // The first frame under the threshold gives LOD0 up, which is the feature
    // working; what must not happen is it being taken back on the next frame up.
    uint32_t level = SelectLodLevel(lods, 0.5f * 0.999f, 0, settings);
    CHECK(level == 1);

    uint32_t switches = 0;
    for (uint32_t frame = 0; frame < 200; ++frame)
    {
        const float jitter = (frame % 2 == 0) ? 1.001f : 0.999f;
        const uint32_t next = SelectLodLevel(lods, 0.5f * jitter, level, settings);
        if (next != level)
        {
            ++switches;
        }
        level = next;
    }
    CHECK(switches == 0);
    CHECK(level == 1);
}

TEST_CASE("The bias moves every switch point at once")
{
    const std::vector<LodRange> lods = Chain(3);

    LodSettings higher;
    higher.bias = 2.f;
    // Twice the bias holds LOD0 down to half the screen size it otherwise would.
    CHECK(SelectLodLevel(lods, 0.26f, 0, higher) == 0);

    LodSettings lower;
    lower.bias = 0.5f;
    CHECK(SelectLodLevel(lods, 0.9f, 0, lower) == 1);
}

TEST_CASE("Selection off pins every instance to LOD0")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.enabled = false;

    CHECK(SelectLodLevel(lods, 0.001f, 2, settings) == 0);
}

TEST_CASE("A forced level draws at that level whatever the instance measures")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.forcedLevel = 2;

    // Filling the screen and a speck in the distance both draw LOD2: that is
    // what looking at a level in place means.
    CHECK(SelectLodLevel(lods, 1.f, 0, settings) == 2);
    CHECK(SelectLodLevel(lods, 0.001f, 0, settings) == 2);

    // And the finest level is forceable too, which is the comparison an artist
    // is actually making.
    settings.forcedLevel = 0;
    CHECK(SelectLodLevel(lods, 0.001f, 2, settings) == 0);
}

TEST_CASE("A forced level clamps to the end of the chain")
{
    LodSettings settings;
    settings.forcedLevel = 7;

    // One control over meshes with chains of different depths, so the level it
    // names is past the end of most of them.
    CHECK(SelectLodLevel(Chain(3), 1.f, 0, settings) == 2);
    CHECK(SelectLodLevel(Chain(1), 1.f, 0, settings) == 0);
    CHECK(SelectLodLevel({}, 1.f, 0, settings) == 0);
}

TEST_CASE("A forced level ignores the dead band and the bias")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.forcedLevel = 1;
    settings.bias = 4.f;

    // Nothing about where the instance was, or about the knobs that move the
    // switch points, moves a level that was named rather than measured.
    for (uint32_t previous = 0; previous < 3; ++previous)
    {
        CHECK(SelectLodLevel(lods, 0.9f, previous, settings) == 1);
        CHECK(SelectLodLevel(lods, 0.01f, previous, settings) == 1);
    }
}

TEST_CASE("Selection off outranks a forced level")
{
    // The off switch is the A/B against the whole feature, so it has to mean
    // LOD0 with no exception left in it.
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.enabled = false;
    settings.forcedLevel = 2;

    CHECK(SelectLodLevel(lods, 1.f, 2, settings) == 0);
}

TEST_CASE("A forced level needs no view, and is the level the selector remembers")
{
    // A named level is not a measurement, so it holds in a frame that measured
    // nothing. Remembering it is what keeps the selection outline on the
    // geometry the mesh pass drew.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    LodSettings settings;
    settings.forcedLevel = 2;
    selector.SetSettings(settings);

    const Entity entity{.index = 1, .generation = 1};
    CHECK(selector.Select(entity, lods, SphereAt(10.f, 1.f)) == 2);
    CHECK(selector.Remembered(entity) == 2);

    selector.BeginFrame(Camera());
    CHECK(selector.Select(entity, lods, SphereAt(2.f, 1.f)) == 2);
}

TEST_CASE("The default thresholds hold full detail past arm's length")
{
    // The defaults are a viewing distance, and that is how they went wrong once:
    // at half the screen's height a tabletop prop dropped a level about a metre
    // away, where the pop is unmissable. Stated here in metres so the next change
    // to them has to say what it does to the object sizes people actually place.
    const LodView view = Camera();
    std::vector<LodRange> lods;
    for (uint32_t level = 0; level < 3; ++level)
    {
        lods.push_back(LodRange{.FirstSubMesh = level,
                                .SubMeshCount = 1,
                                .ScreenSizeThreshold = DefaultLodScreenSize(level)});
    }

    const LodSettings settings;
    auto levelAt = [&](float radius, float distance)
                   { return SelectLodLevel(lods, LodScreenSize(SphereAt(distance, radius), view), 0, settings); };

    // A 0.3 m prop: full detail well past the distance you would stand from it.
    CHECK(levelAt(0.3f, 1.0f) == 0);
    CHECK(levelAt(0.3f, 3.0f) == 0);

    // And it does still drop eventually, or the chain would be decoration.
    CHECK(levelAt(0.3f, 6.0f) > 0);

    // A 5 m building holds LOD0 across a plaza, because the metric is size.
    CHECK(levelAt(5.f, 50.f) == 0);
}

TEST_CASE("An authored threshold overrides the default for its level")
{
    const LodSettings settings;

    // On the defaults 0.6 is comfortably LOD0. Authoring LOD0's threshold up to
    // 0.9 gives the level away at that same size, which the default never would.
    std::vector<LodRange> lods = Chain(2);
    CHECK(SelectLodLevel(lods, 0.6f, 0, settings) == 0);
    lods[0].ScreenSizeThreshold = 0.9f;
    CHECK(SelectLodLevel(lods, 0.6f, 0, settings) == 1);
}

TEST_CASE("LodSelector remembers a level per entity")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    const Entity closeUp{.index = 1, .generation = 1};
    const Entity distant{.index = 2, .generation = 1};

    // Big on screen, and tiny: the two must not share an answer.
    CHECK(selector.Select(closeUp, lods, SphereAt(2.f, 1.f)) == 0);
    CHECK(selector.Select(distant, lods, SphereAt(60.f, 1.f)) == 2);

    CHECK(selector.Remembered(closeUp) == 0);
    CHECK(selector.Remembered(distant) == 2);

    // Never selected at all is LOD0, not whatever the neighbouring slot holds.
    CHECK(selector.Remembered(Entity{.index = 99, .generation = 1}) == 0);
}

TEST_CASE("A recycled entity index does not inherit the old entity's level")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    const Entity original{.index = 4, .generation = 1};
    CHECK(selector.Select(original, lods, SphereAt(60.f, 1.f)) == 2);

    const Entity recycled{.index = 4, .generation = 2};
    CHECK(selector.Remembered(recycled) == 0);
}

TEST_CASE("LodSelector::Clear forgets every level")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());
    const Entity entity{.index = 3, .generation = 1};

    CHECK(selector.Select(entity, lods, SphereAt(60.f, 1.f)) == 2);
    selector.Clear();
    CHECK(selector.Remembered(entity) == 0);
}

TEST_CASE("A selector walking an instance away and back switches once each way")
{
    // The end-to-end shape: one instance, a camera pulling back and returning,
    // and the level moving monotonically rather than oscillating on the way.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());
    const Entity entity{.index = 7, .generation = 1};

    std::vector<uint32_t> outbound;
    for (float distance = 2.f; distance < 40.f; distance += 0.25f)
    {
        outbound.push_back(selector.Select(entity, lods, SphereAt(distance, 1.f)));
    }
    for (size_t i = 1; i < outbound.size(); ++i)
    {
        CHECK(outbound[i] >= outbound[i - 1]); // never regains detail while retreating
    }
    CHECK(outbound.front() == 0);
    CHECK(outbound.back() == 2);

    std::vector<uint32_t> inbound;
    for (float distance = 40.f; distance > 2.f; distance -= 0.25f)
    {
        inbound.push_back(selector.Select(entity, lods, SphereAt(distance, 1.f)));
    }
    for (size_t i = 1; i < inbound.size(); ++i)
    {
        CHECK(inbound[i] <= inbound[i - 1]); // never loses detail while approaching
    }
    CHECK(inbound.back() == 0);
}

TEST_CASE("Released, the dead band stops holding a remembered level")
{
    // Inside the band above LOD0's threshold, coming in from LOD1: held, the
    // instance stays on LOD1; released, it takes LOD0 as plain thresholds do,
    // which is what the GPU cull draws.
    const std::vector<LodRange> lods = Chain(3);
    const Entity entity{.index = 5, .generation = 1};
    const float inBand = 3.43f; // measures 0.505, between 0.5 and 0.5 * 1.02

    LodSelector held;
    held.BeginFrame(Camera());
    REQUIRE(held.Select(entity, lods, SphereAt(5.f, 1.f)) == 1);
    CHECK(held.Select(entity, lods, SphereAt(inBand, 1.f)) == 1);

    LodSelector released;
    released.SetHoldsDeadBand(false);
    released.BeginFrame(Camera());
    REQUIRE(released.Select(entity, lods, SphereAt(5.f, 1.f)) == 1);
    CHECK(released.Select(entity, lods, SphereAt(inBand, 1.f)) == 0);
}

TEST_CASE("Preview answers as Select would without remembering the answer")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());
    const Entity entity{.index = 6, .generation = 1};

    CHECK(selector.Preview(entity, lods, SphereAt(60.f, 1.f)) == 2);
    CHECK(selector.Remembered(entity) == 0);

    // With a level remembered, the preview holds the same band Select would.
    REQUIRE(selector.Select(entity, lods, SphereAt(5.f, 1.f)) == 1);
    CHECK(selector.Preview(entity, lods, SphereAt(3.43f, 1.f)) == 1);
    CHECK(selector.Remembered(entity) == 1);

    // A named level is previewed like any other answer.
    selector.Pin(entity, 2);
    CHECK(selector.Preview(entity, lods, SphereAt(2.f, 1.f)) == 2);
    CHECK(selector.Remembered(entity) == 1);
}

TEST_CASE("A pinned entity draws at its level while the rest of the scene measures")
{
    // The point of the pin: judge one instance at a level without moving every
    // other instance off the level it earned.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    const Entity pinned{.index = 1, .generation = 1};
    const Entity neighbour{.index = 2, .generation = 1};

    selector.Pin(pinned, 2);
    CHECK(selector.Select(pinned, lods, SphereAt(2.f, 1.f)) == 2);      // huge on screen, drawn coarse
    CHECK(selector.Select(neighbour, lods, SphereAt(2.f, 1.f)) == 0);   // the same size, measured

    // And it is the level the selector remembers, so the outline traces the
    // geometry the mesh pass drew.
    CHECK(selector.Remembered(pinned) == 2);
}

TEST_CASE("A pin outranks the viewport's forced level")
{
    // The more specific of the two wins: an artist pinning one instance while
    // the viewport is forced is asking to see that one differently.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    LodSettings settings;
    settings.forcedLevel = 1;
    selector.SetSettings(settings);

    const Entity pinned{.index = 3, .generation = 1};
    const Entity other{.index = 4, .generation = 1};
    selector.Pin(pinned, 0);

    CHECK(selector.Select(pinned, lods, SphereAt(60.f, 1.f)) == 0);
    CHECK(selector.Select(other, lods, SphereAt(60.f, 1.f)) == 1);
}

TEST_CASE("Selection off outranks a pin")
{
    // Same rule as the forced level: off is the A/B against the whole feature
    // and has to mean LOD0 with nothing left over.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    LodSettings settings;
    settings.enabled = false;
    selector.SetSettings(settings);

    const Entity entity{.index = 5, .generation = 1};
    selector.Pin(entity, 2);
    CHECK(selector.Select(entity, lods, SphereAt(2.f, 1.f)) == 0);
}

TEST_CASE("Pinning one entity releases the previous one")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());

    const Entity first{.index = 6, .generation = 1};
    const Entity second{.index = 7, .generation = 1};

    selector.Pin(first, 2);
    selector.Pin(second, 1);

    CHECK(selector.PinnedLevel(first) == -1);
    CHECK(selector.PinnedLevel(second) == 1);
    CHECK(selector.Select(first, lods, SphereAt(2.f, 1.f)) == 0); // measured again
    CHECK(selector.Select(second, lods, SphereAt(2.f, 1.f)) == 1);
}

TEST_CASE("A pin is released by hand and by a scene change")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());
    const Entity entity{.index = 8, .generation = 1};

    selector.Pin(entity, 2);
    selector.Pin(entity, -1);
    CHECK(selector.PinnedLevel(entity) == -1);
    CHECK_FALSE(selector.HasPin());
    CHECK(selector.Select(entity, lods, SphereAt(2.f, 1.f)) == 0);

    // A kept pin would land on whatever entity reused the index, which the
    // generation cannot catch across a scene that starts counting again.
    selector.Pin(entity, 2);
    selector.Clear();
    CHECK_FALSE(selector.HasPin());
    CHECK(selector.Select(entity, lods, SphereAt(2.f, 1.f)) == 0);
}

TEST_CASE("A pin belongs to the entity that was pinned, not to its index")
{
    LodSelector selector;
    const Entity original{.index = 9, .generation = 1};
    selector.Pin(original, 2);

    CHECK(selector.PinnedLevel(Entity{.index = 9, .generation = 2}) == -1);
    CHECK(selector.PinnedLevel(Entity{.index = 10, .generation = 1}) == -1);
    CHECK(selector.PinnedLevel(Assisi::ECS::NullEntity) == -1);
}

TEST_CASE("A named level is the pin, then the force, and nothing when it must be measured")
{
    // What a consumer that cannot measure reads — the GPU cull path takes this
    // and has no other way to reach a level.
    LodSelector selector;
    const Entity pinned{.index = 11, .generation = 1};
    const Entity other{.index = 12, .generation = 1};

    CHECK(selector.NamedLevelFor(other) == -1); // measure it

    LodSettings settings;
    settings.forcedLevel = 1;
    selector.SetSettings(settings);
    CHECK(selector.NamedLevelFor(other) == 1);

    selector.Pin(pinned, 3);
    CHECK(selector.NamedLevelFor(pinned) == 3);
    CHECK(selector.NamedLevelFor(other) == 1);

    // Off names LOD0 rather than "measure it", which is the whole of what off
    // means to a path that cannot measure.
    settings.enabled = false;
    selector.SetSettings(settings);
    CHECK(selector.NamedLevelFor(pinned) == 0);
    CHECK(selector.NamedLevelFor(other) == 0);
}

TEST_CASE("A report names a pinned level as one nothing measured")
{
    // The readout must not print thresholds beside a pinned level: nothing is
    // comparing against them, and showing them would read as a pick that agreed.
    const std::vector<LodRange> lods = Chain(3);
    LodSelector selector;
    selector.BeginFrame(Camera());
    const Entity entity{.index = 13, .generation = 1};
    selector.Pin(entity, 2);

    LodSettings resolved = selector.Settings();
    resolved.forcedLevel = selector.NamedLevelFor(entity);
    const LodReport report = DescribeLodSelection(lods, SphereAt(2.f, 1.f), 2, Camera(), resolved);

    CHECK(report.forced);
    CHECK(report.dropBelow == doctest::Approx(0.f));
    CHECK(report.regainAt == doctest::Approx(0.f));
}

TEST_CASE("A report names the level drawn and the sizes on either side of it")
{
    // The numbers an artist tunes a threshold against: what the instance
    // measures, and the two sizes where it would change level.
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings; // hysteresis 0.02

    const LodReport report = DescribeLodSelection(lods, SphereAt(5.f, 1.f), 1, Camera(), settings);

    CHECK(report.level == 1);
    CHECK(report.levelCount == 3);
    CHECK(report.screenSize == doctest::Approx(LodScreenSize(SphereAt(5.f, 1.f), Camera())));
    CHECK(report.biasedScreenSize == doctest::Approx(report.screenSize));
    CHECK(report.dropBelow == doctest::Approx(0.25f));
    CHECK(report.regainAt == doctest::Approx(0.5f * 1.02f)); // the dead band, included
    CHECK_FALSE(report.forced);

    // The two switch points bracket the size, which is what saying "it is on
    // this level" means.
    CHECK(report.biasedScreenSize >= report.dropBelow);
    CHECK(report.biasedScreenSize < report.regainAt);
}

TEST_CASE("A report describes the level it is given rather than re-selecting one")
{
    // It reports what was drawn. Re-deriving the level here would show a
    // different answer from the one on screen exactly when the two disagree,
    // which is the moment the readout is being consulted.
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    const LodReport report = DescribeLodSelection(lods, SphereAt(2.f, 1.f), 2, Camera(), settings);
    CHECK(report.level == 2);
    CHECK(report.screenSize > 0.5f);
}

TEST_CASE("A report leaves a switch point at zero where there is no level to switch to")
{
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    // Nothing is finer than LOD0 to take back...
    const LodReport finest = DescribeLodSelection(lods, SphereAt(2.f, 1.f), 0, Camera(), settings);
    CHECK(finest.dropBelow == doctest::Approx(0.5f));
    CHECK(finest.regainAt == doctest::Approx(0.f));

    // ...and nothing is coarser than the last level to fall to.
    const LodReport coarsest = DescribeLodSelection(lods, SphereAt(60.f, 1.f), 2, Camera(), settings);
    CHECK(coarsest.dropBelow == doctest::Approx(0.f));
    CHECK(coarsest.regainAt == doctest::Approx(0.25f * 1.02f));
}

TEST_CASE("A report says a level was named rather than measured")
{
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.forcedLevel = 2;

    const LodReport report = DescribeLodSelection(lods, SphereAt(2.f, 1.f), 2, Camera(), settings);

    // The size is still measured and still worth reading — it is what the level
    // would have been picked by. The thresholds are not: nothing is comparing
    // against them, and printing them would read as a pick that agrees.
    CHECK(report.forced);
    CHECK(report.screenSize > 0.f);
    CHECK(report.dropBelow == doctest::Approx(0.f));
    CHECK(report.regainAt == doctest::Approx(0.f));
}

TEST_CASE("A report on a mesh with no chain says so and compares against nothing")
{
    const LodSettings settings;

    const LodReport single = DescribeLodSelection(Chain(1), SphereAt(5.f, 1.f), 0, Camera(), settings);
    CHECK(single.levelCount == 1);
    CHECK(single.level == 0);
    CHECK(single.dropBelow == doctest::Approx(0.f));
    CHECK(single.regainAt == doctest::Approx(0.f));

    // No table at all is the factory primitive, and it is one level too.
    CHECK(DescribeLodSelection({}, SphereAt(5.f, 1.f), 0, Camera(), settings).levelCount == 1);
}

TEST_CASE("A report shows the bias in the size it compares, not in the thresholds")
{
    // The bias multiplies the measurement; the thresholds are the asset's and
    // do not move. Showing it the other way round would have an artist edit a
    // threshold to chase a number the asset never held.
    const std::vector<LodRange> lods = Chain(3);
    LodSettings settings;
    settings.bias = 2.f;

    const LodReport report = DescribeLodSelection(lods, SphereAt(10.f, 1.f), 1, Camera(), settings);

    CHECK(report.biasedScreenSize == doctest::Approx(2.f * report.screenSize));
    CHECK(report.dropBelow == doctest::Approx(0.25f));
}

TEST_CASE("A report agrees with the level selection actually picks")
{
    // The two are separate walks over the same thresholds, so a change to one
    // that misses the other shows up as a readout that brackets the wrong size.
    const std::vector<LodRange> lods = Chain(3);
    const LodSettings settings;

    for (float distance = 1.f; distance < 60.f; distance += 0.5f)
    {
        const BoundingSphere sphere = SphereAt(distance, 1.f);
        const uint32_t level = SelectLodLevel(lods, LodScreenSize(sphere, Camera()), 0, settings);
        const LodReport report = DescribeLodSelection(lods, sphere, level, Camera(), settings);

        if (report.dropBelow > 0.f)
        {
            CHECK(report.biasedScreenSize >= report.dropBelow);
        }
        if (report.regainAt > 0.f)
        {
            CHECK(report.biasedScreenSize < report.regainAt);
        }
    }
}
