/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/ShadowSettings.hpp>

#include <limits>

using namespace Assisi::Render;

TEST_CASE("Sun shadow settings are sanitized into their ranges")
{
    SunShadowSettings settings;
    settings.cascadeCount = 99;
    settings.resolution = 100000;
    settings.maxDistance = std::numeric_limits<float>::quiet_NaN();
    settings.splitLambda = 5.f;
    settings.depthBiasTexels = -3.f;
    settings.slopeBias = std::numeric_limits<float>::infinity();
    settings.normalOffsetTexels = 400.f;
    settings.cascadeBlend = 4.0f;
    settings.filter = static_cast<ShadowFilter>(77);
    settings.format = static_cast<ShadowMapFormat>(9);

    const SunShadowSettings safe = Sanitized(settings);
    const SunShadowSettings defaults;

    CHECK(safe.cascadeCount == kMaxShadowCascades);
    CHECK(safe.resolution == kMaxShadowResolution);
    CHECK(safe.maxDistance == doctest::Approx(defaults.maxDistance)); // NaN falls back, never clamps
    CHECK(safe.splitLambda == doctest::Approx(kMaxSplitLambda));
    CHECK(safe.depthBiasTexels == doctest::Approx(kMinDepthBiasTexels));
    CHECK(safe.slopeBias == doctest::Approx(defaults.slopeBias));
    CHECK(safe.normalOffsetTexels == doctest::Approx(kMaxNormalOffsetTexels));
    CHECK(safe.cascadeBlend == doctest::Approx(kMaxCascadeBlend));
    CHECK(safe.filter == defaults.filter);
    CHECK(safe.format == defaults.format);

    // A resolution that is not a power of two rounds down, so the texel lattice
    // the snap quantises to divides the box evenly.
    SunShadowSettings odd;
    odd.resolution = 3000;
    CHECK(Sanitized(odd).resolution == 2048);

    SunShadowSettings tiny;
    tiny.resolution = 1;
    CHECK(Sanitized(tiny).resolution == kMinShadowResolution);
}

TEST_CASE("Local shadow settings are sanitized on their own terms")
{
    // The atlas reaches higher than a cascade does, so a size a cascade would
    // have clamped away survives here. The two halves have separate bounds
    // precisely because one number could not serve both.
    LocalShadowSettings large;
    large.atlasResolution = 8192;
    CHECK(Sanitized(large).atlasResolution == 8192);
    CHECK(kMaxShadowAtlasResolution > kMaxShadowResolution);

    LocalShadowSettings settings;
    settings.atlasResolution = 100000;
    settings.faceResolution = 3000;
    settings.depthBiasTexels = std::numeric_limits<float>::quiet_NaN();
    settings.slopeBias = -1.f;
    settings.normalOffsetTexels = 99.f;
    settings.filter = static_cast<ShadowFilter>(77);
    settings.format = static_cast<ShadowMapFormat>(9);

    const LocalShadowSettings safe = Sanitized(settings);
    const LocalShadowSettings defaults;

    CHECK(safe.atlasResolution == kMaxShadowAtlasResolution);
    CHECK(safe.faceResolution == 2048); // rounded down to a power of two
    CHECK(safe.depthBiasTexels == doctest::Approx(defaults.depthBiasTexels));
    CHECK(safe.slopeBias == doctest::Approx(kMinSlopeBias));
    CHECK(safe.normalOffsetTexels == doctest::Approx(kMaxNormalOffsetTexels));
    CHECK(safe.filter == defaults.filter);
    CHECK(safe.format == defaults.format);

    // A face larger than the atlas it is cut from cannot be allocated, and the
    // allocator would have nothing to demote it to.
    LocalShadowSettings oversized;
    oversized.atlasResolution = 512;
    oversized.faceResolution = 2048;
    CHECK(Sanitized(oversized).faceResolution == 512);
}

TEST_CASE("The two halves sanitize independently")
{
    // The whole-struct overload must reach both, or a hand-edited local half
    // would arrive at an allocation unchecked.
    ShadowSettings settings;
    settings.sun.resolution = 100000;
    settings.local.atlasResolution = 100000;

    const ShadowSettings safe = Sanitized(settings);
    CHECK(safe.sun.resolution == kMaxShadowResolution);
    CHECK(safe.local.atlasResolution == kMaxShadowAtlasResolution);

    // And a nonsense value in one half does not disturb the other.
    ShadowSettings oneBad;
    oneBad.sun.cascadeCount = 99;
    oneBad.local.faceResolution = 512;
    CHECK(Sanitized(oneBad).local.faceResolution == 512);
}

TEST_CASE("Tiers round-trip through the knobs they set")
{
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        const auto tier = static_cast<ShadowTier>(i);
        CHECK(Tier(TierSettings(tier)) == tier);
    }

    // Editing a knob no tier has an opinion about does not leave the tier.
    ShadowSettings high = TierSettings(ShadowTier::High);
    high.sun.normalOffsetTexels = 4.f;
    high.sun.cascadeBlend = 0.2f;
    high.local.slopeBias = 3.f;
    CHECK(Tier(high) == ShadowTier::High);

    // Editing one a tier does set makes it custom.
    high.sun.resolution = 1024;
    CHECK(Tier(high) == ShadowTier::Custom);
}

TEST_CASE("A tier names both halves, so half a tier is no tier")
{
    // The readout compares the whole knob space. A caller that wrote only the
    // sun's fields would leave the panel reporting Custom the moment its own
    // button was pressed, which is the bug this asserts against.
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        const auto tier = static_cast<ShadowTier>(i);
        ShadowSettings sunOnly;
        sunOnly.sun = TierSettings(tier).sun;
        if (tier != ShadowTier::Medium) // Medium is the struct's own default in both halves
        {
            CHECK(Tier(sunOnly) == ShadowTier::Custom);
        }

        ShadowSettings both = TierSettings(tier);
        CHECK(Tier(both) == tier);
    }

    // Every tier's atlas is D16: a local light's depth range is its own reach,
    // and 16 bits over metres is millimetres.
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        CHECK(TierSettings(static_cast<ShadowTier>(i)).local.format == ShadowMapFormat::D16);
    }
}

TEST_CASE("The selection knobs are sanitized on their own terms")
{
    LocalShadowSelectionSettings hostile;
    hostile.capSpot = 100000;
    hostile.capPoint = 100000;
    hostile.capHysteresis = std::numeric_limits<float>::quiet_NaN();
    hostile.classHysteresis = 9.f;

    // options.json is hand-editable, and these values order what renders — a
    // NaN margin would make every comparison against it false and the ordering
    // arbitrary.
    const LocalShadowSelectionSettings safe = Sanitized(hostile);
    CHECK(safe.capSpot == kMaxShadowCap);
    CHECK(safe.capPoint == kMaxShadowCap);
    CHECK(safe.capHysteresis == LocalShadowSelectionSettings{}.capHysteresis);
    CHECK(safe.classHysteresis == 1.f);

    // Zero is a real setting on both caps — it means this light type never
    // shadows — which is why the floor is not one.
    LocalShadowSelectionSettings none;
    none.capSpot = 0;
    none.capPoint = 0;
    CHECK(Sanitized(none).capSpot == 0);
    CHECK(Sanitized(none).capPoint == 0);

    // And the aggregate sanitizes all three halves, not two.
    ShadowSettings settings;
    settings.selection.capSpot = 100000;
    CHECK(Sanitized(settings).selection.capSpot == kMaxShadowCap);
}

TEST_CASE("A tier sets the caps, and a cap edit leaves the tier")
{
    // Point caps are about a quarter the spot caps at every tier, and that is
    // arithmetic rather than taste: a point light is six shadow renders against
    // a spot's one, so equal caps would mean six times the work for the same
    // number.
    for (std::uint32_t i = 0; i < kShadowTierCount; ++i)
    {
        const ShadowSettings preset = TierSettings(static_cast<ShadowTier>(i));
        CHECK(preset.selection.capPoint < preset.selection.capSpot);
        CHECK(preset.selection.capPoint * 4u == preset.selection.capSpot);
    }

    // Raising a tier raises both caps: a higher tier shadows more lights, which
    // is most of what the tier means here.
    CHECK(TierSettings(ShadowTier::Low).selection.capSpot < TierSettings(ShadowTier::Medium).selection.capSpot);
    CHECK(TierSettings(ShadowTier::Medium).selection.capSpot < TierSettings(ShadowTier::High).selection.capSpot);
    CHECK(TierSettings(ShadowTier::High).selection.capSpot < TierSettings(ShadowTier::Ultra).selection.capSpot);

    // A cap is a knob the tier sets, so moving it is leaving the tier — unlike
    // a bias, which no tier has an opinion about.
    ShadowSettings high = TierSettings(ShadowTier::High);
    high.selection.capPoint += 1;
    CHECK(Tier(high) == ShadowTier::Custom);

    // The hysteresis margins are not tier knobs: they are correctness settings,
    // and an author who tunes one has not left the tier they picked.
    ShadowSettings tuned = TierSettings(ShadowTier::High);
    tuned.selection.capHysteresis = 0.4f;
    tuned.selection.classHysteresis = 0.05f;
    CHECK(Tier(tuned) == ShadowTier::High);
}

TEST_CASE("Shadow memory is reported per half and in total")
{
    // The memory column of the tier table, computed rather than quoted.
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Low).sun) == 4ull * 1024 * 1024 * 2);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Medium).sun) == 4ull * 2048 * 2048 * 4);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::High).sun) == 4ull * 4096 * 4096 * 4);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Ultra).sun) == 6ull * 4096 * 4096 * 4);

    // One atlas whatever the light count is — that is what an atlas is for —
    // and a second one holding the still geometry's depth while tiles are
    // cached. That doubling is the whole price of the cache, and it is the one
    // number a tier's memory column has to move by when it is turned off.
    LocalShadowSettings uncached = TierSettings(ShadowTier::Low).local;
    uncached.cache.enabled = false;
    CHECK(LocalShadowMemoryBytes(uncached) == 2048ull * 2048 * 2);
    CHECK(LocalShadowMemoryBytes(TierSettings(ShadowTier::Low).local) == 2 * LocalShadowMemoryBytes(uncached));
    CHECK(LocalShadowMemoryBytes(TierSettings(ShadowTier::Ultra).local) == 2ull * 8192 * 8192 * 2);

    const ShadowSettings ultra = TierSettings(ShadowTier::Ultra);
    CHECK(ShadowMemoryBytes(ultra) == SunShadowMemoryBytes(ultra.sun) + LocalShadowMemoryBytes(ultra.local));

    // Shadows off allocate nothing at all — the blank-scene rule, in the one
    // place a reader would look for the number. Each half answers for itself,
    // so turning one off leaves the other's memory standing.
    ShadowSettings off = TierSettings(ShadowTier::Ultra);
    off.sun.enabled = false;
    off.local.enabled = false;
    CHECK(ShadowMemoryBytes(off) == 0);

    ShadowSettings sunOff = TierSettings(ShadowTier::Ultra);
    sunOff.sun.enabled = false;
    CHECK(ShadowMemoryBytes(sunOff) == LocalShadowMemoryBytes(sunOff.local));
    CHECK(ShadowMemoryBytes(sunOff) > 0);
}
