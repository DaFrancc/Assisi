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

TEST_CASE("Shadow memory is reported per half and in total")
{
    // The memory column of the tier table, computed rather than quoted.
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Low).sun) == 4ull * 1024 * 1024 * 2);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Medium).sun) == 4ull * 2048 * 2048 * 4);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::High).sun) == 4ull * 4096 * 4096 * 4);
    CHECK(SunShadowMemoryBytes(TierSettings(ShadowTier::Ultra).sun) == 6ull * 4096 * 4096 * 4);

    // One atlas whatever the light count is — that is what an atlas is for.
    CHECK(LocalShadowMemoryBytes(TierSettings(ShadowTier::Low).local) == 2048ull * 2048 * 2);
    CHECK(LocalShadowMemoryBytes(TierSettings(ShadowTier::Ultra).local) == 8192ull * 8192 * 2);

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
