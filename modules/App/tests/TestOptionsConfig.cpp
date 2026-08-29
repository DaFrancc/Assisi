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

    written.shadows.local.enabled = false;
    written.shadows.local.atlasResolution = 8192;
    written.shadows.local.format = ShadowMapFormat::D32;
    written.shadows.local.faceResolution = 1024;
    written.shadows.local.filter = ShadowFilter::Pcf5x5;
    written.shadows.local.depthBiasTexels = 0.5f;
    written.shadows.local.slopeBias = 1.25f;
    written.shadows.local.normalOffsetTexels = 4.f;

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

    CHECK(read.shadows.local.enabled == written.shadows.local.enabled);
    CHECK(read.shadows.local.atlasResolution == written.shadows.local.atlasResolution);
    CHECK(read.shadows.local.format == written.shadows.local.format);
    CHECK(read.shadows.local.faceResolution == written.shadows.local.faceResolution);
    CHECK(read.shadows.local.filter == written.shadows.local.filter);
    CHECK(read.shadows.local.depthBiasTexels == doctest::Approx(written.shadows.local.depthBiasTexels));
    CHECK(read.shadows.local.slopeBias == doctest::Approx(written.shadows.local.slopeBias));
    CHECK(read.shadows.local.normalOffsetTexels == doctest::Approx(written.shadows.local.normalOffsetTexels));
}

TEST_CASE("The two shadow halves are written to their own sections")
{
    // They are separate documents' worth of knobs, and a reader that pointed
    // both at one section would silently have each overwrite the other's
    // same-named fields — filter, and all three biases.
    const std::string text = OptionsConfig{}.ToJsonText();
    CHECK(text.find("\"sun\"") != std::string::npos);
    CHECK(text.find("\"local\"") != std::string::npos);

    OptionsConfig differing;
    differing.shadows.sun.filter = ShadowFilter::Point;
    differing.shadows.local.filter = ShadowFilter::Vogel;
    differing.shadows.sun.slopeBias = 1.f;
    differing.shadows.local.slopeBias = 7.f;

    const OptionsConfig read = OptionsConfig::FromJsonText(differing.ToJsonText());
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

TEST_CASE("A document that will not parse costs the settings, not the launch")
{
    const OptionsConfig read = OptionsConfig::FromJsonText("{ this is not json");
    const OptionsConfig defaults;
    CHECK(read.shadows.sun.cascadeCount == defaults.shadows.sun.cascadeCount);
    CHECK(read.aaMode == defaults.aaMode);
}
