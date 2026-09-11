/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/App/OptionsConfig.hpp>

#include <string>

using namespace Assisi::App;
using namespace Assisi::Render;

TEST_CASE("Shadow settings survive a write and a read")
{
    OptionsConfig written;
    written.shadows.sun.enabled = false;
    written.shadows.sun.cascadeCount = 6;
    written.shadows.sun.resolution = 1024;
    written.shadows.sun.format = ShadowMapFormat::D16;
    written.shadows.sun.maxDistance = 55.f;
    written.shadows.sun.splitLambda = 0.6f;
    written.shadows.sun.filter = ShadowFilter::Vogel;
    written.shadows.sun.depthBiasTexels = 2.25f;
    written.shadows.sun.slopeBias = 3.5f;
    written.shadows.sun.normalOffsetTexels = 0.75f;
    written.shadows.sun.cascadeBlend = 0.1f;
    written.shadows.sun.cadence.enabled = false;
    written.shadows.sun.cadence.driftTexels = 1.5f;

    written.shadows.local.enabled = false;
    written.shadows.local.atlasResolution = 8192;
    written.shadows.local.format = ShadowMapFormat::D32;
    written.shadows.local.faceResolution = 1024;
    written.shadows.local.filter = ShadowFilter::Pcf5x5;
    written.shadows.local.depthBiasTexels = 0.5f;
    written.shadows.local.slopeBias = 1.25f;
    written.shadows.local.normalOffsetTexels = 4.f;
    written.shadows.local.sourceRadius = 0.3f;
    written.shadows.pcss = ShadowPcss::SunAndLocals;

    const OptionsConfig read = OptionsConfig::FromJsonText(written.ToJsonText());

    CHECK(read.shadows.sun.enabled == written.shadows.sun.enabled);
    CHECK(read.shadows.sun.cascadeCount == written.shadows.sun.cascadeCount);
    CHECK(read.shadows.sun.resolution == written.shadows.sun.resolution);
    CHECK(read.shadows.sun.format == written.shadows.sun.format);
    CHECK(read.shadows.sun.maxDistance == doctest::Approx(written.shadows.sun.maxDistance));
    CHECK(read.shadows.sun.splitLambda == doctest::Approx(written.shadows.sun.splitLambda));
    CHECK(read.shadows.sun.filter == written.shadows.sun.filter);
    CHECK(read.shadows.sun.depthBiasTexels == doctest::Approx(written.shadows.sun.depthBiasTexels));
    CHECK(read.shadows.sun.slopeBias == doctest::Approx(written.shadows.sun.slopeBias));
    CHECK(read.shadows.sun.normalOffsetTexels == doctest::Approx(written.shadows.sun.normalOffsetTexels));
    CHECK(read.shadows.sun.cascadeBlend == doctest::Approx(written.shadows.sun.cascadeBlend));
    CHECK(read.shadows.sun.cadence.enabled == written.shadows.sun.cadence.enabled);
    CHECK(read.shadows.sun.cadence.driftTexels == doctest::Approx(written.shadows.sun.cadence.driftTexels));

    CHECK(read.shadows.local.enabled == written.shadows.local.enabled);
    CHECK(read.shadows.local.atlasResolution == written.shadows.local.atlasResolution);
    CHECK(read.shadows.local.format == written.shadows.local.format);
    CHECK(read.shadows.local.faceResolution == written.shadows.local.faceResolution);
    CHECK(read.shadows.local.filter == written.shadows.local.filter);
    CHECK(read.shadows.local.depthBiasTexels == doctest::Approx(written.shadows.local.depthBiasTexels));
    CHECK(read.shadows.local.slopeBias == doctest::Approx(written.shadows.local.slopeBias));
    CHECK(read.shadows.local.normalOffsetTexels == doctest::Approx(written.shadows.local.normalOffsetTexels));
    CHECK(read.shadows.local.sourceRadius == doctest::Approx(written.shadows.local.sourceRadius));
    CHECK(read.shadows.pcss == written.shadows.pcss);
}

TEST_CASE("Every contact-hardening setting survives a write and a read, and a typo is off")
{
    for (const ShadowPcss pcss : {ShadowPcss::Off, ShadowPcss::Sun, ShadowPcss::SunAndLocals})
    {
        OptionsConfig written;
        written.shadows.pcss = pcss;
        CHECK(OptionsConfig::FromJsonText(written.ToJsonText()).shadows.pcss == pcss);
    }

    // An unrecognised value costs that one field, and what it costs it is the
    // cheap answer rather than a search nobody asked for.
    const OptionsConfig typo = OptionsConfig::FromJsonText(R"({ "shadows": { "pcss": "sunAndLocal" } })");
    CHECK(typo.shadows.pcss == ShadowPcss::Off);
}

TEST_CASE("The two shadow halves are written to their own sections")
{
    // They are separate documents' worth of knobs, and a reader that pointed
    // both at one section would silently have each overwrite the other's
    // same-named fields — filter, and all three biases.
    OptionsConfig differing;
    differing.shadows.sun.filter = ShadowFilter::Point;
    differing.shadows.local.filter = ShadowFilter::Vogel;
    differing.shadows.sun.slopeBias = 1.f;
    differing.shadows.local.slopeBias = 7.f;

    const std::string text = differing.ToJsonText();
    CHECK(text.find("\"sun\"") != std::string::npos);
    CHECK(text.find("\"local\"") != std::string::npos);

    const OptionsConfig read = OptionsConfig::FromJsonText(text);
    CHECK(read.shadows.sun.filter == ShadowFilter::Point);
    CHECK(read.shadows.local.filter == ShadowFilter::Vogel);
    CHECK(read.shadows.sun.slopeBias == doctest::Approx(1.f));
    CHECK(read.shadows.local.slopeBias == doctest::Approx(7.f));
}

TEST_CASE("A settings file from before the split loads defaults rather than failing")
{
    // The shape that shipped before the sun and local halves were separated:
    // the sun's knobs sitting directly under "shadows". It names no half, so
    // neither half can be read from it — but the rest of the document still
    // loads, and nothing downstream sees a half-built value.
    const std::string legacy = R"({
        "antiAliasing": { "mode": "fxaa", "msaaSamples": 8 },
        "shadows": {
            "enabled": true,
            "cascades": 6,
            "resolution": 4096,
            "format": "d16",
            "filter": "vogel"
        },
        "frameSync": { "mode": "fpsLimit", "fpsLimit": 144 }
    })";

    const OptionsConfig read = OptionsConfig::FromJsonText(legacy);
    const OptionsConfig defaults;

    CHECK(read.shadows.sun.cascadeCount == defaults.shadows.sun.cascadeCount);
    CHECK(read.shadows.sun.resolution == defaults.shadows.sun.resolution);
    CHECK(read.shadows.sun.filter == defaults.shadows.sun.filter);
    CHECK(read.shadows.local.atlasResolution == defaults.shadows.local.atlasResolution);

    // The document is not rejected over it: everything outside the shadow
    // section is read exactly as before.
    CHECK(read.aaMode == AaMode::FXAA);
    CHECK(read.msaaSamples == 8);
    CHECK(read.frameSync == FrameSyncMode::FpsLimit);
    CHECK(read.fpsLimit == 144);
}

TEST_CASE("Hand-typed shadow values are clamped before they size an allocation")
{
    // options.json is hand-editable and these numbers reach createTexture, so
    // the read is the last place that can refuse an absurd one.
    const std::string absurd = R"({
        "shadows": {
            "sun": { "resolution": 100000, "cascades": 99, "maxDistance": 1e9 },
            "local": { "atlasResolution": 100000, "faceResolution": 65536 }
        }
    })";

    const OptionsConfig read = OptionsConfig::FromJsonText(absurd);

    CHECK(read.shadows.sun.resolution == kMaxShadowResolution);
    CHECK(read.shadows.sun.cascadeCount == kMaxShadowCascades);
    CHECK(read.shadows.sun.maxDistance == doctest::Approx(kMaxShadowDistance));
    CHECK(read.shadows.local.atlasResolution == kMaxShadowAtlasResolution);
    CHECK(read.shadows.local.faceResolution == kMaxShadowFaceResolution);
}

TEST_CASE("Sky reflection settings survive a write and a read")
{
    OptionsConfig written;
    written.environment.enabled = false;
    written.environment.resolution = 256;
    written.environment.sampleCount = 128;
    written.environment.rebakeDegrees = 2.5f;

    const OptionsConfig read = OptionsConfig::FromJsonText(written.ToJsonText());
    CHECK(read.environment.enabled == written.environment.enabled);
    CHECK(read.environment.resolution == written.environment.resolution);
    CHECK(read.environment.sampleCount == written.environment.sampleCount);
    CHECK(read.environment.rebakeDegrees == doctest::Approx(written.environment.rebakeDegrees));
}

TEST_CASE("A hand-typed probe size is clamped before it sizes an allocation")
{
    const std::string absurd = R"({ "environment": { "resolution": 100000, "samples": 0 } })";
    const OptionsConfig read = OptionsConfig::FromJsonText(absurd);
    CHECK(read.environment.resolution == kMaxProbeResolution);
    CHECK(read.environment.sampleCount == kMinProbeSampleCount);
}

TEST_CASE("A settings file from before sky reflections existed turns them on at their defaults")
{
    const OptionsConfig read = OptionsConfig::FromJsonText(R"({ "antiAliasing": { "mode": "fxaa" } })");
    CHECK(read.environment == EnvironmentSettings{});
}

TEST_CASE("Ambient occlusion settings survive a write and a read")
{
    OptionsConfig written;
    written.ambientOcclusion.enabled = true;
    written.ambientOcclusion.sampleCount = 20;
    written.ambientOcclusion.radius = 1.25f;
    written.ambientOcclusion.strength = 1.75f;

    const OptionsConfig read = OptionsConfig::FromJsonText(written.ToJsonText());
    CHECK(read.ambientOcclusion.enabled == written.ambientOcclusion.enabled);
    CHECK(read.ambientOcclusion.sampleCount == written.ambientOcclusion.sampleCount);
    CHECK(read.ambientOcclusion.radius == doctest::Approx(written.ambientOcclusion.radius));
    CHECK(read.ambientOcclusion.strength == doctest::Approx(written.ambientOcclusion.strength));
}

TEST_CASE("A hand-typed occlusion kernel is clamped before it reaches a shader")
{
    // The sample count is a loop bound over a fixed-size kernel in a constant
    // buffer; past its capacity the shader reads off the end.
    const std::string absurd = R"({ "ambientOcclusion": { "samples": 100000, "radius": -3.0 } })";
    const OptionsConfig read = OptionsConfig::FromJsonText(absurd);
    CHECK(read.ambientOcclusion.sampleCount == kMaxSsaoSampleCount);
    CHECK(read.ambientOcclusion.radius == doctest::Approx(kMinSsaoRadius));
}

TEST_CASE("A settings file from before ambient occlusion existed loads it off, at its defaults")
{
    const OptionsConfig read = OptionsConfig::FromJsonText(R"({ "antiAliasing": { "mode": "fxaa" } })");
    CHECK(read.ambientOcclusion == SsaoSettings{});
}

TEST_CASE("Only settings that differ from the defaults are written, so the rest follow the defaults")
{
    // Nothing changed writes nothing: a default written out would pin it, and a
    // later change to that default would never reach this file.
    CHECK(OptionsConfig{}.ToJsonText() == "{}");

    OptionsConfig one;
    one.shadows.local.filter = ShadowFilter::Point;
    const std::string text = one.ToJsonText();
    CHECK(text.find("\"filter\"") != std::string::npos);
    CHECK(text.find("depthBiasTexels") == std::string::npos);
    CHECK(text.find("\"sun\"") == std::string::npos);
    CHECK(text.find("antiAliasing") == std::string::npos);
}

TEST_CASE("A float is saved to four decimal places, so a slider dragged back to its default is the default")
{
    OptionsConfig nearly;
    nearly.shadows.local.depthBiasTexels += 0.00001f;
    CHECK(nearly.ToJsonText() == "{}");

    OptionsConfig moved;
    moved.shadows.local.depthBiasTexels = 0.123456f;
    const std::string text = moved.ToJsonText();
    CHECK(text.find("0.1235") != std::string::npos);
    CHECK(text.find("0.123456") == std::string::npos);
    CHECK(OptionsConfig::FromJsonText(text).shadows.local.depthBiasTexels == doctest::Approx(0.1235f));
}

TEST_CASE("A document that will not parse costs the settings, not the launch")
{
    const OptionsConfig read = OptionsConfig::FromJsonText("{ this is not json");
    const OptionsConfig defaults;
    CHECK(read.shadows.sun.cascadeCount == defaults.shadows.sun.cascadeCount);
    CHECK(read.aaMode == defaults.aaMode);
}
