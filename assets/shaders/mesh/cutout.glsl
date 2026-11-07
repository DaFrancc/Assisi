// A cutout's alpha test, shared by the depth prepass and the lit pass without
// one.
//
// Without multisampling a texel is kept or cut whole, exactly as alpha against
// the cutoff says. With it, the pipeline turns alpha-to-coverage on and the
// fragment keeps a share of its pixel's samples instead: the test is widened to
// one pixel's worth of the alpha's change across the surface, so an edge fades
// over a pixel rather than stepping, and multisampling smooths cutouts as it
// smooths triangle edges (Golus, "Anti-aliased Alpha Test: The Esoteric Alpha
// To Coverage", 2017).

// The scene target's samples per pixel, fixed when the pipeline is built.
layout(constant_id = 0) const uint kSceneSamples = 1u;

// The least change in alpha across a pixel the widened test divides by, so a
// surface of even alpha keeps a sharp edge rather than dividing by zero.
const float kMinAlphaChangePerPixel = 1.0 / 4096.0;

// What the fragment writes as its alpha, the share of its pixel's samples it
// covers; negative when it covers none and is cut away.
float CutoutCoverage(float alpha, float cutoff)
{
    // The widened test would cut at the same alpha; this one is exact to the
    // bit and takes no derivatives.
    if (kSceneSamples == 1u)
    {
        return alpha < cutoff ? -1.0 : 1.0;
    }
    float coverage = clamp((alpha - cutoff) / max(fwidth(alpha), kMinAlphaChangePerPixel) + 0.5, 0.0, 1.0);
    // Less than half a sample's share covers no sample.
    return coverage < 0.5 / float(kSceneSamples) ? -1.0 : coverage;
}
