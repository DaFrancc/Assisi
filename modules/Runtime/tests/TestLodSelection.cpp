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
