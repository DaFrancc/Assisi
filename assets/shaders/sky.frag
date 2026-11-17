#version 450
#extension GL_GOOGLE_include_directive : require

// Single-scattering through an atmosphere whose depth along a ray is an air-mass
// approximation rather than an integral: a handful of exps per pixel, no loop,
// and no history.
//
// This is a transcription of Render::SkyRadiance in Sky.hpp, which is also what
// the ambient term queries on the CPU. The two must agree — every constant lives
// there, and the scattering coefficients are authored per level rather than
// baked in here, so this shader knows nothing about which planet it is on. The
// scattering itself is in sky/scatter.glsl, which the fog shares.

#include "sky/scatter.glsl"
#include "sky/fog.glsl"
#include "sky/fog_volumes.glsl"
#include "sky/cloud_map.glsl"

layout(binding = 256) uniform SkyConstants
{
    mat4 invViewProjection;
    vec4 cameraPosition;  // xyz = world-space eye, w unused
    vec4 sunDirection;    // xyz = unit direction TO the sun, w = disk radius (radians)
    vec4 sunRadiance;     // xyz = colour * intensity, w = disk edge softness
    vec4 airScattering;   // xyz = how strongly the air scatters each channel, w = how much air there is
    vec4 haze;            // xyz = how strongly haze scatters each channel, w = its forwardness
    vec4 groundColor;     // xyz = linear colour, w = sky exposure
    vec4 nightColor;      // xyz = linear colour, w = disk intensity
    vec4 sunDiskColor;    // xyz = disk tint, w = limb darkening
    vec4 atmosphere;      // x = sky bounce, y = 1 while the sky has clouds, zw unused
    vec4 moonDirection;   // xyz = unit direction TO the moon, w = its disk radius (radians)
    vec4 moonRadiance;    // xyz = colour * intensity, w = its disk intensity
    vec4 moonDiskColor;   // xyz = tint over the albedo texture, w = how much of the air's colour shift it takes
    vec4 moonUp;          // xyz = which way the disk image's top points, w unused
    vec4 fog;             // xyz = the fog's own colour, w = what to draw for it
} uSky;

// Mirrors Render::SkyFogConstants: the placed fog volumes in front of the sky,
// and the level's fog they are seen through.
layout(binding = 257) uniform SkyFogVolumes
{
    FogConstants fog;
    uvec4        volumes; // Render::FogVolumeFrame::Lanes; zero in a capture
} uSkyFog;

// Must match Render::SkyFogMode.
const uint kSkyFogBelowHorizon = 1u;
const uint kSkyFogFlat         = 2u;

// How far away the sky is, for the volumes in front of it: past any of them.
const float kSkyDistance = 3.0e38;

// The moon's albedo: an orthographic photograph of the one hemisphere a tidally
// locked moon ever shows. Loaded sRGB, so the view decodes it to linear here.
layout(binding = 0)   uniform texture2D uMoon;
layout(binding = 128) uniform sampler   uMoonSampler;

// The clouds, as cloud_map.comp drew them this frame: rgb to add to the sky
// behind them, alpha how much of it shows through. Across wraps, up clamps.
layout(binding = 1)   uniform texture2D uCloudMap;
layout(binding = 129) uniform sampler   uCloudMapSampler;

layout(location = 0) in vec4 vRayPoint;
layout(location = 0) out vec4 outColor;

// In units of dot(surface normal, direction to the sun). The moon is not shaded
// by a cosine — regolith is retro-reflective and its lit face is near uniform —
// so this is only how abruptly the terminator arrives.
const float kMoonTerminatorSoftness = 0.05;
const float kMinDiskRadius   = 1e-6;
const float kMinTangentLength = 1e-6;

// The disk's brightness off its centre, in [0, 1]. Two effects, and they are not
// the same one: the edge fade is antialiasing, and the limb darkening is what a
// sphere looks like. A disk with the first and not the second is a soft-edged
// sticker.
float SunDiskProfile(float angleToSun, float radius, float edgeSoftness, float limbDarkening)
{
    float edge = 1.0 - smoothstep(radius * (1.0 - edgeSoftness), radius * (1.0 + edgeSoftness), angleToSun);
    if (edge <= 0.0)
    {
        return 0.0;
    }
    // How far across the visible face this line of sight lands, then the cosine
    // of the angle it makes with the surface there. A ray at the rim leaves
    // through cooler material and carries less of it out.
    float acrossFace = min(angleToSun / max(radius, kMinDiskRadius), 1.0);
    float faceCosine = sqrt(max(1.0 - acrossFace * acrossFace, 0.0));
    return edge * (1.0 - limbDarkening * (1.0 - faceCosine));
}

// The moon's disk: where on its photograph a ray lands, and whether that point is
// in sunlight. The picture is orthographic and this coordinate is an orthographic
// image plane, so a pixel of image is a pixel of moon — no unwrap, no atan, and
// the limb's foreshortening already in the picture.
vec3 MoonDisk(vec3 ray, vec3 toMoon, vec3 toSun, float radius)
{
    vec3 up    = uSky.moonUp.xyz;
    vec3 right = cross(toMoon, up);   // forward x up = right, the camera convention

    float cosGamma = clamp(dot(ray, toMoon), -1.0, 1.0);
    float across   = acos(cosGamma) / max(radius, kMinDiskRadius);
    vec3  offset   = ray - toMoon * cosGamma;
    float len      = length(offset);
    vec3  sideways = len > kMinTangentLength ? offset / len : vec3(0.0);

    // Not clamped for the lookup: the sampler clamps instead, so a ray inside the
    // profile's edge fade gets the image's border rather than a smear of its rim.
    vec2 uv = vec2(0.5 + 0.5 * across * dot(sideways, right),
                   0.5 - 0.5 * across * dot(sideways, up));

    // Clamped here, where the coordinate has to name a point ON the sphere: past
    // the limb there is no surface for a normal to belong to.
    float onFace     = min(across, 1.0);
    float faceCosine = sqrt(max(1.0 - onFace * onFace, 0.0));
    vec3  normal     = sideways * onFace - toMoon * faceCosine;
    float lit        = smoothstep(-kMoonTerminatorSoftness, kMoonTerminatorSoftness, dot(normal, toSun));

    return texture(sampler2D(uMoon, uMoonSampler), uv).rgb * lit;
}

void main()
{
    vec3 eye = uSky.cameraPosition.xyz;
    vec3 ray = normalize(vRayPoint.xyz / vRayPoint.w - eye);

    // A foggy level with no sky: behind everything is the colour the fog fades
    // into, as authored, and nothing for the sky's exposure to scale.
    uint fogMode = uint(uSky.fog.w);
    if (fogMode == kSkyFogFlat)
    {
        vec3 flatFog = max(uSky.fog.rgb, vec3(0.0));
        outColor = vec4(ApplyFogVolumes(flatFog, uSkyFog.volumes, uSkyFog.fog, eye, ray, kSkyDistance,
                                        gl_FragCoord.xy), 1.0);
        return;
    }

    SkyScatter scatter = SkyScatter(uSky.sunDirection, uSky.sunRadiance, uSky.airScattering, uSky.haze,
                                    uSky.groundColor, uSky.nightColor, uSky.atmosphere, uSky.moonDirection,
                                    uSky.moonRadiance, uSky.moonDiskColor);

    // With the level's fog on, what shows below the horizon is past the draw
    // distance, where the fog is total, so it is the fog's colour: the horizon
    // carried on down, not the ground. The disks still set at the horizon.
    bool  fogged  = fogMode == kSkyFogBelowHorizon;
    float skyward = smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    SkyAir air    = SkyAirRadiance(scatter, fogged ? FogDirection(ray) : ray);
    vec3 radiance = fogged ? air.sky : mix(air.ground, air.sky, skyward);

    vec3 sunDirection  = uSky.sunDirection.xyz;
    vec3 moonDirection = uSky.moonDirection.xyz;

    // Both disks, gated by the same blend so a body sets behind the ground rather
    // than shining up through it.
    float sunDiskIntensity = uSky.nightColor.w;
    if (sunDiskIntensity > 0.0)
    {
        float cosGamma = clamp(dot(ray, sunDirection), -1.0, 1.0);
        float profile = SunDiskProfile(acos(cosGamma), uSky.sunDirection.w, uSky.sunRadiance.w, uSky.sunDiskColor.w);
        radiance += air.sunBeam * uSky.sunDiskColor.rgb * (sunDiskIntensity * profile * skyward);
    }

    float moonDiskIntensity = uSky.moonRadiance.w;
    if (moonDiskIntensity > 0.0)
    {
        float radius   = uSky.moonDirection.w;
        float cosGamma = clamp(dot(ray, moonDirection), -1.0, 1.0);
        // Limb darkening zero: the photograph already carries whatever the real
        // limb does. The edge softness IS shared with the sun, because that is
        // antialiasing rather than a look.
        float profile = SunDiskProfile(acos(cosGamma), radius, uSky.sunRadiance.w, 0.0);
        vec3  face    = MoonDisk(ray, moonDirection, sunDirection, radius);
        // The beam is already tinted — Contribution applied it — so the disk, the
        // aureole around it and the light on the ground all soften by the same
        // amount and cannot disagree.
        radiance += air.moonBeam * uSky.moonDiskColor.rgb * face * (moonDiskIntensity * profile * skyward);
    }

    vec3 sky = max(radiance * uSky.groundColor.w, vec3(0.0));

    // The clouds in front of all of it, the disks included, so the sun goes
    // behind a cloud. Gated by the same blend as the disks: there are no clouds
    // below the horizon.
    if (uSky.atmosphere.y != 0.0)
    {
        vec4 clouds = textureLod(sampler2D(uCloudMap, uCloudMapSampler), CloudMapUv(ray), 0.0);
        sky = sky * mix(1.0, clouds.a, skyward) + clouds.rgb * skyward;
    }
    outColor = vec4(ApplyFogVolumes(sky, uSkyFog.volumes, uSkyFog.fog, eye, ray, kSkyDistance, gl_FragCoord.xy),
                    1.0);
}
