#version 450
#extension GL_EXT_nonuniform_qualifier : require
// texelFetch and textureSize on textures read without a sampler: the shadow
// maps' stored depth for the debug views and blocker searches, and the
// screen-space targets.
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_GOOGLE_include_directive : require

// Each piece may use anything the pieces before it declare.
#include "mesh/frame.glsl"
#include "mesh/material.glsl"
#include "mesh/brdf.glsl"
#include "mesh/shadow_filters.glsl"
#include "mesh/sun_shadows.glsl"
#include "mesh/local_shadows.glsl"
#include "mesh/indirect.glsl"

void main()
{
    // Derivatives while control flow is still uniform, before the masked build's
    // discard; the sun's lookup that reads them runs inside a per-light branch.
    if (uFrame.shadowPcss.x > 0.0)
    {
        gWorldPosDx = dFdx(vWorldPos);
        gWorldPosDy = dFdy(vWorldPos);
    }

    Surface surf = SampleMaterial();

#ifdef ASSISI_ALPHA_MASK
    // Only this build can discard, because a shader that can loses early depth
    // rejection for every draw through its pipeline.
    if (surf.alpha < surf.alphaCutoff)
    {
        discard;
    }
#endif

    // Material debug views write one channel straight out; the tone map passes
    // them through untouched. Colour channels are gamma-encoded for display.
    uint debugMode = uFrame.lightCounts.y;
    if (debugMode != kDebugNone)
    {
        if (debugMode == kDebugBaseColor)
            outColor = vec4(pow(surf.albedo, vec3(1.0 / 2.2)), 1.0);
        else if (debugMode == kDebugMetallic)
            outColor = vec4(vec3(surf.metallic), 1.0);
        else if (debugMode == kDebugRoughness)
            outColor = vec4(vec3(surf.roughness), 1.0);
        else if (debugMode == kDebugNormal)
            outColor = vec4(surf.normal * 0.5 + 0.5, 1.0);
        else if (debugMode == kDebugOcclusion)
            outColor = vec4(vec3(surf.occlusion), 1.0);
        else if (debugMode == kDebugEmissive)
            outColor = vec4(pow(surf.emissive, vec3(1.0 / 2.2)), 1.0);
        else if (debugMode == kDebugScreenOcclusion)
            outColor = vec4(vec3(uFrame.indirectSpecular.z != 0.0 ? ScreenOcclusion() : 1.0), 1.0);
        else
            outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3  albedo   = surf.albedo;
    vec3  N        = surf.normal;
    float metallic = surf.metallic;
    vec3  V        = normalize(uFrame.cameraPosition.xyz - vWorldPos);

    // Dielectrics reflect what their IOR says; metals reflect their base colour.
    vec3 F0 = mix(DielectricF0(surf.specIor, surf.specWeight, surf.specColor), albedo, metallic);
    vec3 Lo = vec3(0.0);

    BrdfContext brdf = MakeBrdfContext(N, V, surf, F0);

    // Only the sun casts among directional lights; shadowCounts.y is its index.
    uint shadowCascadeCount = uFrame.shadowCounts.x;
    uint shadowLightIndex   = uFrame.shadowCounts.y;
    uint shadowCascade      = 0u;

    uint dirLightCount = uFrame.lightCounts.x;
    for (uint i = 0u; i < dirLightCount; i++)
    {
        vec3 L        = -dirLights[i].directionIntensity.xyz;
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        if (shadowCascadeCount > 0u && i == shadowLightIndex)
        {
            shadowCascade = SelectCascade(abs(vViewZ));

            // Shadows are looked up with the geometric normal: the depth map holds
            // rasterized geometry, not the normal-mapped surface. A surface facing
            // away by either normal is skipped; one lit only by its normal map is
            // unlit, since the map knows nothing about it.
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= SunVisibility(Ng, L, shadowCascade, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                radiance = vec3(0.0);
            }
        }
        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    uint localShadows = uFrame.localShadowCounts.y;

    uint clusterIdx  = ClusterIndex();
    uint pointOffset = lightGrids[clusterIdx].pointOffset;
    uint pointCount  = lightGrids[clusterIdx].pointCount;
    uint spotOffset  = lightGrids[clusterIdx].spotOffset;
    uint spotCount   = lightGrids[clusterIdx].spotCount;

    for (uint i = 0u; i < pointCount; i++)
    {
        uint  li   = lightIndexList[pointOffset + i];
        vec3  lPos = pointLights[li].positionRadius.xyz;
        float r    = pointLights[li].positionRadius.w;

        // max() guards a fragment exactly on the light.
        vec3  toLight = lPos - vWorldPos;
        float d2      = max(dot(toLight, toLight), 1e-8);

        // The cluster can list a light for fragments past its radius.
        float att = AttenuationSq(d2, r * r);
        if (att == 0.0)
        {
            continue;
        }

        vec3  lCol     = pointLights[li].colorIntensity.xyz;
        float lInt     = pointLights[li].colorIntensity.w;
        vec3  L        = toLight * inversesqrt(d2);
        vec3  radiance = lCol * lInt * att;

        // The same two-normal rule as the sun.
        if (localShadows == 1u)
        {
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= PointVisibility(pointLights[li].shadowView.x, lPos, vWorldPos, Ng, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                radiance = vec3(0.0);
            }
        }

        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    for (uint i = 0u; i < spotCount; i++)
    {
        uint  li     = lightIndexList[kSpotIndexBase + spotOffset + i];
        vec3  lPos   = spotLights[li].positionRadius.xyz;
        float r      = spotLights[li].positionRadius.w;
        vec3  lDir   = spotLights[li].directionInner.xyz; // normalised on upload
        float inner  = spotLights[li].directionInner.w;
        float outer  = spotLights[li].outerCutoff;

        vec3  toLight = lPos - vWorldPos;
        float d2      = max(dot(toLight, toLight), 1e-8);
        vec3  L       = toLight * inversesqrt(d2);

        // max() keeps a cone authored with inner == outer from dividing by zero.
        float theta = dot(L, -lDir);
        float cone  = smoothstep(outer, max(inner, outer + 1e-4), theta);

        float att = AttenuationSq(d2, r * r);
        if (att == 0.0 || cone == 0.0)
        {
            continue;
        }

        vec3  lCol     = spotLights[li].colorIntensity.xyz;
        float lInt     = spotLights[li].colorIntensity.w;
        vec3  radiance = lCol * lInt * att * cone;

        if (localShadows == 1u)
        {
            vec3  Ng     = GeometricNormal();
            float NgdotL = dot(Ng, L);
            float NdotL  = dot(N, L);
            if (NdotL > 0.0 && NgdotL > 0.0)
            {
                radiance *= SpotVisibility(spotLights[li].shadowView, lDir, lPos, vWorldPos, Ng, NgdotL);
            }
            else if (NdotL > 0.0)
            {
                radiance = vec3(0.0);
            }
        }

        Lo += CookTorrance(brdf, N, V, L, radiance);
    }

    // The provider answers with cosine-weighted mean radiance, which is what a
    // Lambertian albedo multiplies, so no factor of pi.
    vec3 indirect = IndirectRadiance(N, vWorldPos);

    // Screen-space occlusion takes the darker of itself and the material's map:
    // both see the asset's own creases.
    bool  screenOccluded  = uFrame.indirectSpecular.z != 0.0;
    float screenOcclusion = 1.0;
    float occlusion       = surf.occlusion;
    if (screenOccluded)
    {
        screenOcclusion = ScreenOcclusion();
        occlusion = min(occlusion, screenOcclusion);
    }

    vec3 color;
    if (uFrame.indirectSpecular.x == 0.0)
    {
        // No environment to reflect.
        color = indirect * albedo * occlusion + Lo + surf.emissive;
    }
    else
    {
        // The split sum: the table gives how much of the environment the lobe
        // reflects, and the diffuse layer gets the rest.
        vec2 ab             = brdf.envBrdf;
        vec3 specularAlbedo = F0 * ab.x + ab.y;
        // Multi-scatter compensation from this lobe's own albedo, A + B.
        vec3 compensation   = vec3(1.0) + F0 * (1.0 / max(ab.x + ab.y, kEps) - 1.0);
        vec3 specular       = IndirectSpecular(reflect(-V, N), surf.roughness, vWorldPos) * specularAlbedo *
                              compensation;
        vec3 diffuse        = indirect * albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
        if (screenOccluded)
        {
            // A narrow reflection pointing out of a crease is occluded less than
            // the diffuse term, so it takes its own share of the occlusion.
            float alpha = surf.roughness * surf.roughness;
            float specularOcclusion =
                min(surf.occlusion, SpecularOcclusion(brdf.NdotV, screenOcclusion, alpha));
            color = diffuse * occlusion + specular * specularOcclusion + Lo + surf.emissive;
        }
        else
        {
            color = (diffuse + specular) * surf.occlusion + Lo + surf.emissive;
        }
    }

    // ---- Shadow debug views ----

    // Tints the lit result by the cascade each pixel sampled.
    if (uFrame.shadowCounts.w == kShadowDebugCascades && shadowCascadeCount > 0u)
    {
        color *= CascadeTint(shadowCascade);
    }

    // How far in front of this fragment the map's recorded occluder sits:
    //   blue   outside the cascade
    //   red    an occluder in front, brighter the further (shadowed)
    //   green  nothing in front (lit)
    //   black  within a millimetre: ambiguous
    if (uFrame.shadowCounts.w == kShadowDebugMargin && shadowCascadeCount > 0u)
    {
        vec3        Ng    = GeometricNormal();
        vec3        sunL  = -dirLights[uFrame.shadowCounts.y].directionIntensity.xyz;
        ShadowProbe probe = ProbeCascade(shadowCascade, vWorldPos, Ng, dot(Ng, sunL));

        const float kFullScaleMetres = 0.25;
        float margin = (probe.stored - probe.reference) * uFrame.shadowCascade[shadowCascade].w;
        float scaled = clamp(abs(margin) / kFullScaleMetres, 0.0, 1.0);

        color = probe.outside ? vec3(0.0, 0.0, 1.0)
                              : (margin >= 0.0 ? vec3(0.0, scaled, 0.0) : vec3(scaled, 0.0, 0.0));
    }

    // The sun's visibility against the answers inside it:
    //   red    SunVisibility (what shades the pixel)
    //   green  this cascade's filtered lookup
    //   blue   its centre tap alone
    // Black and white are agreement; red alone is the cascade blend adding light,
    // yellow the kernel finding light its centre did not.
    if (uFrame.shadowCounts.w == kShadowDebugTaps && shadowCascadeCount > 0u)
    {
        vec3  Ng    = GeometricNormal();
        vec3  sunL  = -dirLights[uFrame.shadowCounts.y].directionIntensity.xyz;
        float NdotL = dot(Ng, sunL);

        // Facing away from the sun: never looked up.
        if (NdotL <= 0.0)
        {
            outColor = vec4(0.08, 0.08, 0.12, 1.0);
            return;
        }

        ShadowProbe probe    = ProbeCascade(shadowCascade, vWorldPos, Ng, NdotL);
        float       blended  = SunVisibility(Ng, sunL, shadowCascade, NdotL);
        float       filtered = SampleCascade(shadowCascade, vWorldPos, Ng, NdotL);
        float       centre   = ShadowTap(ShadowCoord(shadowCascade,
                                                     vWorldPos + Ng * (uFrame.shadowCascade[shadowCascade].z *
                                                                       sqrt(max(0.0, 1.0 - NdotL * NdotL)))).xy,
                                         shadowCascade, probe.reference);

        color = vec3(blended, filtered, centre);
    }

    // Linear, unbounded radiance; tonemap.frag decides how it looks.
    outColor = vec4(color, 1.0);
}
