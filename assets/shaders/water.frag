#version 450
#extension GL_GOOGLE_include_directive : require
// texelFetch on the shadow maps and the scene's distance.
#extension GL_EXT_samplerless_texture_functions : require

// Shades open water, after Atlas's (Mihelich and Tcheblokov, GDC 2019): the
// surface is a field of tiny facets whose slopes the wave textures give as
// moments, so the sun's highlight and the sky's reflection come from the
// distribution of slopes over the pixel, and detail too small to draw becomes
// roughness instead of shimmer. Under the surface, light the water scatters
// back, and the ground through it, fading with depth.

#include "sky/fog.glsl"
#include "sky/fog_volumes.glsl"
#include "mesh/cloud_shadow.glsl"

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vGridPos;
layout(location = 3) in float vViewZ;
layout(location = 4) in float vPlaneDistance;
layout(location = 5) in float vCrest;

// What the water sends toward the eye, added to the scene; and how much of
// what is behind it still shows, per colour, which the blend multiplies it by.
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outBehind;

// What mesh.frag takes from brdf.glsl, whose BRDF needs a material water
// does not have.
const float kEps = 1.0e-7;
layout(binding = 130) uniform sampler uClampSampler;

#include "mesh/frame.glsl"
#include "mesh/shadow_filters.glsl"
#include "mesh/sun_shadows.glsl"
#include "mesh/indirect.glsl"
#include "water/surface.glsl"

// Water's reflectance head-on: ((1.33 - 1) / (1.33 + 1))².
const float kWaterF0 = 0.02;

// Below mip 0 the sampler's blend between texels makes up variance the waves
// do not have; it is trusted fully from this mip.
const float kTrustedVarianceLod = 4.0;

// Reflections looking below the horizon would see the sea's own underside in
// a sky probe; they are lifted to just above it.
const float kHorizonLift = 0.02;

// A slope variance this small is a mirror: the floor keeps the inverse finite.
const float kMinSlopeVariance = 1.0e-5;

// Walter's fit of Smith's masking for a Beckmann surface,
// (1 - A·a + B·a²) / (C·a + D·a²), which holds below the limit and is zero past it.
const float kMaskingFitLimit = 1.6;
const float kMaskingFitA     = 1.259;
const float kMaskingFitB     = 0.396;
const float kMaskingFitC     = 3.535;
const float kMaskingFitD     = 2.181;

// Atlas's fake of light scattered under the surface toward the eye: how much
// the sun lights the water's body from above, and how brightly a crest glows
// with the sun behind it.
const float kSunScatter   = 0.05;
const float kCrestScatter = 6.0;

// Atlas's roughness term for the Fresnel of a rough surface.
const float kFresnelRoughFade  = 2.69;
const float kFresnelRoughScale = 22.7;
const float kFresnelRoughPower = 1.5;

// Fog thinner than this does not show, as in mesh.frag.
const float kMinVisibleFog = 1.0 / 512.0;

// The fastest any colour is absorbed, as a multiple of the colour the water
// passes best.
const float kMaxAbsorptionShape = 8.0;

// How much faster each colour is absorbed than the one the water passes best.
// The crest colour is light that has crossed a little water, so its hue is the
// one that survives: blue-green water loses red first, a peaty lake blue.
vec3 AbsorptionShape()
{
    vec3 hue = max(uWater.crestColor.rgb, vec3(kEps));
    return clamp(vec3(max(hue.r, max(hue.g, hue.b))) / hue, vec3(1.0), vec3(kMaxAbsorptionShape));
}

// The slope distribution over this pixel: its mean, and the covariance of its
// slopes along x and z.
struct Slopes
{
    vec2 mean;
    vec3 covariance; // x = var x, y = var z, z = covariance
};

Slopes SlopesHere()
{
    Slopes slopes;
    slopes.mean       = vec2(0.0);
    slopes.covariance = vec3(uWater.surface.z, uWater.surface.z, 0.0);
    for (uint cascade = 0u; cascade < kSurfaceCascadeCount; ++cascade)
    {
        vec3  uv     = vec3(WaterUv(vGridPos, cascade), float(cascade));
        vec4  moment = texture(sampler2DArray(uSlopes, uWaterSampler), uv);
        float crossMoment = texture(sampler2DArray(uDisplacement, uWaterSampler), uv).w;
        float trust = clamp(textureQueryLod(sampler2DArray(uSlopes, uWaterSampler), uv.xy).y / kTrustedVarianceLod, 0.0, 1.0);
        // Bands are independent: their means add, and so do their variances.
        slopes.mean += moment.xy;
        slopes.covariance +=
            trust * vec3(max(moment.zw - moment.xy * moment.xy, vec2(0.0)), crossMoment - moment.x * moment.y);
    }
    slopes.covariance.xy = max(slopes.covariance.xy, vec2(kMinSlopeVariance));
    return slopes;
}

// The variance of the slopes along the horizontal direction of @p direction.
float ProjectedVariance(Slopes slopes, vec3 direction)
{
    vec2  across = direction.xz;
    float length2 = max(dot(across, across), kEps);
    return (slopes.covariance.x * across.x * across.x + slopes.covariance.y * across.y * across.y +
            2.0 * slopes.covariance.z * across.x * across.y) / length2;
}

// Smith's Λ for a Beckmann surface seen along @p direction, by Walter's fit.
float Masking(Slopes slopes, vec3 direction)
{
    float alpha   = sqrt(2.0 * ProjectedVariance(slopes, direction));
    float tangent = length(direction.xz) / max(direction.y, kEps);
    float a       = 1.0 / max(alpha * tangent, kEps);
    return a < kMaskingFitLimit ? (1.0 - kMaskingFitA * a + kMaskingFitB * a * a) / (kMaskingFitC * a + kMaskingFitD * a * a)
                                : 0.0;
}

// How likely a facet of slope @p slope is: the slopes' Gaussian.
float SlopeDensity(Slopes slopes, vec2 slope)
{
    vec3  c           = slopes.covariance;
    float determinant = max(c.x * c.y - c.z * c.z, kEps);
    vec2  d           = slope - slopes.mean;
    float exponent    = (c.y * d.x * d.x - 2.0 * c.z * d.x * d.y + c.x * d.y * d.y) / determinant;
    return exp(-0.5 * exponent) / (2.0 * PI * sqrt(determinant));
}

float Schlick(float cosine)
{
    float x  = clamp(1.0 - cosine, 0.0, 1.0);
    float x2 = x * x;
    return kWaterF0 + (1.0 - kWaterF0) * x2 * x2 * x;
}

// The sun's light off the facets that face between it and the eye.
float SunSpecular(Slopes slopes, vec3 V, vec3 L)
{
    if (L.y <= 0.0 || V.y <= 0.0)
    {
        return 0.0;
    }
    vec3  H      = normalize(V + L);
    float cosH   = max(H.y, kEps);
    float cosH2  = cosH * cosH;
    float facets = SlopeDensity(slopes, -H.xz / cosH) / (cosH2 * cosH2);
    return Schlick(dot(V, H)) * facets / (4.0 * V.y * (1.0 + Masking(slopes, L) + Masking(slopes, V)));
}

void main()
{
    CaptureSurfacePlane();
    Slopes slopes = SlopesHere();

    vec3  N        = normalize(vec3(-slopes.mean.x, 1.0, -slopes.mean.y));
    vec3  toEye    = uFrame.cameraPosition.xyz - vWorldPos;
    float distance = length(toEye);
    vec3  V        = toEye / max(distance, kEps);

    // The sun, and every other directional light, shadowed as the lit pass
    // shadows them.
    vec3           Ng       = vNormal;
    ShadowReceiver receiver = ShadowReceiverAt(Ng, false);
    float          cloudLight = CloudShadow(uFrame.cloudShadow, vWorldPos - uFrame.cameraPosition.xyz, uClampSampler);
    uint           shadowCascadeCount = uFrame.shadowCounts.x;
    uint           shadowLightIndex   = uFrame.shadowCounts.y;
    vec3           specular = vec3(0.0);
    vec3           sunLight = vec3(0.0);
    vec3           crestGlow = vec3(0.0);
    for (uint i = 0u; i < uFrame.lightCounts.x; ++i)
    {
        vec3 L        = -dirLights[i].directionIntensity.xyz;
        vec3 radiance = dirLights[i].colorPad.xyz * (dirLights[i].directionIntensity.w * cloudLight);
        if (shadowCascadeCount > 0u && i == shadowLightIndex && L.y > 0.0)
        {
            uint cascade = SelectCascade(abs(vViewZ));
            radiance *= SunVisibility(cascade, ShadowSurfaceToward(Ng, receiver, L, L.y));
        }
        specular += radiance * SunSpecular(slopes, V, L);
        sunLight += radiance * max(L.y, 0.0);
        // Light crossing a crest toward the eye, strongest with the sun low
        // behind it.
        float behind = pow(max(dot(L, -V), 0.0), 4.0);
        float facing = pow(0.5 - 0.5 * dot(L, N), 3.0);
        crestGlow += radiance * (kCrestScatter * max(vCrest, 0.0) * behind * facing / (1.0 + Masking(slopes, L)));
    }

    // The sky in the reflection, as rough as the slopes this pixel averages.
    float alpha   = sqrt(slopes.covariance.x + slopes.covariance.y);
    vec3  R       = reflect(-V, N);
    R             = normalize(vec3(R.x, max(R.y, kHorizonLift), R.z));
    vec3  sky     = uFrame.indirectSpecular.x != 0.0 ? IndirectSpecular(R, sqrt(alpha), vWorldPos)
                                                     : IndirectRadiance(R, vWorldPos);
    float viewAlpha = sqrt(2.0 * ProjectedVariance(slopes, V));
    float x         = clamp(1.0 - max(dot(N, V), 0.0), 0.0, 1.0);
    float x2        = x * x;
    float fresnel   = kWaterF0 + (1.0 - kWaterF0) * x2 * x2 * x * exp(-kFresnelRoughFade * viewAlpha) /
                                     (1.0 + kFresnelRoughScale * pow(viewAlpha, kFresnelRoughPower));

    // What the water's body sends back: the sky's light and the sun's
    // scattered in it, and crests glowing against the sun.
    vec3 ambient = IndirectRadiance(vec3(0.0, 1.0, 0.0), vWorldPos);
    vec3 scatter = uWater.color.rgb * (ambient + kSunScatter * sunLight) + uWater.crestColor.rgb * crestGlow;

    // How much water lies between the surface and the ground behind it, along
    // the line of sight: the scene's distance is a depth along the view.
    float depthHere  = max(abs(vViewZ), kEps);
    float depthBehind = texelFetch(uSceneDistance, ivec2(gl_FragCoord.xy), 0).r;
    float thickness  = max(depthBehind - depthHere, 0.0) * distance / depthHere;
    vec3  through    = exp(-thickness * AbsorptionShape() / uWater.color.a);
    float edge       = smoothstep(0.0, uWater.surface.w, thickness);

    // What is behind shows by (1 - F)·T, and the water's own light takes the
    // rest; at the very edge, the ground shows as if there were no water.
    vec3 color  = (1.0 - fresnel) * (1.0 - through) * scatter + specular + fresnel * sky;
    vec3 behind = (1.0 - fresnel) * through;
    color *= edge;
    behind = mix(vec3(1.0), behind, edge);
    vec3 cover = vec3(1.0) - behind;

    // Fog over it, as over any see-through surface: what shows through already
    // carries the fog in front of it.
    vec3 viewRay = -V;
    if (uFrame.fog.view.x != 0.0)
    {
        float opacity = FogOpacity(uFrame.fog, distance, uFrame.fog.view.y + vWorldPos.y);
        if (opacity > kMinVisibleFog)
        {
            color = color * (1.0 - opacity) + FogRadiance(uFrame.fog, viewRay) * (opacity * cover);
        }
    }
    FogVolumesEffect volumes =
        FogVolumesAlong(uFrame.fogVolumes, uFrame.fog, uFrame.cameraPosition.xyz, viewRay, distance, gl_FragCoord.xy);
    color = color * volumes.through + volumes.added * cover;

    // The scene's alpha is left as it was.
    outColor  = vec4(color, 0.0);
    outBehind = vec4(behind, 1.0);
}
