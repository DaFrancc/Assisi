// Light from everything that is not a light: the sky's hemisphere, the
// prefiltered environment, and screen-space occlusion of both.

// Radiance onto a surface at @p worldPos facing @p N. Mirrors
// Render::EvaluateIndirect. The position is unused by the hemisphere and is there
// for a provider that varies by place.
vec3 IndirectRadiance(vec3 N, vec3 worldPos)
{
    float upward = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(uFrame.indirectGround.rgb, uFrame.indirectSky.rgb, upward);
}

// The sky prefiltered by roughness: mip m is blurred by a GGX lobe of roughness
// m / last mip (see SkyProbe).
layout(binding = 10) uniform textureCube uEnvironmentSpecular;

// Mean radiance along a GGX lobe of @p roughness about the reflection @p R.
// Mirrors IndirectLighting::SpecularRadiance.
vec3 IndirectSpecular(vec3 R, float roughness, vec3 worldPos)
{
    float lod = roughness * uFrame.indirectSpecular.y;
    return textureLod(samplerCube(uEnvironmentSpecular, uClampSampler), R, lod).rgb;
}

// The fraction of each pixel's hemisphere left open (SsaoPass). Read only while
// uFrame says occlusion ran.
layout(binding = 12) uniform texture2D uScreenOcclusion;

// Applies to the indirect term only; shadow maps already answer direct light.
float ScreenOcclusion()
{
    return texelFetch(uScreenOcclusion, ivec2(gl_FragCoord.xy), 0).r;
}

// The share of the environment's reflection that survives an occlusion of @p ao,
// for a GGX lobe of @p alpha seen at @p nDotV (Lagarde's fit). Mirrors
// Render::SpecularOcclusion.
float SpecularOcclusion(float nDotV, float ao, float alpha)
{
    const float kExponentPerAlpha = -16.0;
    const float kMirrorExponent   = -1.0;
    float exponent = exp2(kExponentPerAlpha * alpha + kMirrorExponent);
    return clamp(pow(nDotV + ao, exponent) - 1.0 + ao, 0.0, 1.0);
}
