#version 450

// Single-scattering through an atmosphere whose depth along a ray is an air-mass
// approximation rather than an integral: a handful of exps per pixel, no loop,
// and no history.
//
// This is a transcription of Render::SkyRadiance in Sky.hpp, which is also what
// the ambient term queries on the CPU. The two must agree — every constant lives
// there, and the scattering coefficients are authored per level rather than
// baked in here, so this shader knows nothing about which planet it is on.

layout(binding = 256) uniform SkyConstants
{
    mat4 invViewProjection;
    vec4 cameraPosition;  // xyz = world-space eye, w unused
    vec4 sunDirection;    // xyz = unit direction TO the sun, w = disk radius (radians)
    vec4 sunRadiance;     // xyz = colour * intensity, w = disk edge softness
    vec4 rayleigh;        // xyz = scattering coefficients, w = the optical depth scaling them
    vec4 mie;             // xyz = coefficients, w = asymmetry
    vec4 groundColor;     // xyz = linear colour, w = sky intensity
    vec4 nightColor;      // xyz = linear colour, w = disk intensity
    vec4 sunDiskColor;    // xyz = disk tint, w = limb darkening
    vec4 atmosphere;      // x = multiple scattering, yzw unused
} uSky;

layout(location = 0) in vec4 vFarPoint;
layout(location = 0) out vec4 outColor;

const float kHorizonAirMass  = 35.567;
const float kTwilightFalloff = 10.0;
const float kHorizonSoftness = 0.01;
const float kInvPi           = 0.31830989;

// Air mass along a ray, relative to straight up. Kasten-Young down to the
// horizon, held there below it: a downward ray leaves through the ground, and
// what happens under it is the ground's business.
float ViewAirMass(float cosZenith)
{
    float c = clamp(cosZenith, -1.0, 1.0);
    if (c <= 0.0)
    {
        return kHorizonAirMass;
    }
    return 1.0 / (c + 0.15 * pow(93.885 - degrees(acos(c)), -1.253));
}

// The same for the beam, continued below the horizon — the sun's path keeps
// lengthening after it sets, and that continuation is the whole of dusk.
float SunAirMass(float cosZenith)
{
    float c = clamp(cosZenith, -1.0, 1.0);
    return c >= 0.0 ? ViewAirMass(c) : kHorizonAirMass * exp(kTwilightFalloff * -c);
}

// Both phase functions average one over the sphere rather than integrating to
// one, so they read as "against scattering the same light every way" and sit on
// the same footing as each other.
float RayleighPhase(float cosTheta)
{
    return 0.75 * (1.0 + cosTheta * cosTheta);
}

float MiePhase(float cosTheta, float asymmetry)
{
    float gg = asymmetry * asymmetry;
    // Clamped because the denominator reaches zero looking along the beam as the
    // asymmetry approaches one, and the pow would return infinity.
    float denom = max(1.0 + gg - 2.0 * asymmetry * cosTheta, 1e-4);
    return (1.0 - gg) / pow(denom, 1.5);
}

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
    float acrossFace = min(angleToSun / max(radius, 1e-6), 1.0);
    float faceCosine = sqrt(max(1.0 - acrossFace * acrossFace, 0.0));
    return edge * (1.0 - limbDarkening * (1.0 - faceCosine));
}

void main()
{
    vec3 ray = normalize(vFarPoint.xyz / vFarPoint.w - uSky.cameraPosition.xyz);

    vec3  sunDirection = uSky.sunDirection.xyz;
    vec3  radiantSun   = uSky.sunRadiance.rgb;
    // The coefficients are two different things and must not be confused. As a
    // RATIO they tint the in-scattered light; scaled by the optical depth they
    // are an EXTINCTION, and only that form belongs in an exp().
    vec3  rayleigh     = uSky.rayleigh.rgb;
    vec3  extinction   = rayleigh * uSky.rayleigh.w;
    float rayleighGrey = (extinction.r + extinction.g + extinction.b) / 3.0;
    vec3  mieCoeff     = uSky.mie.rgb;

    float cosGamma    = clamp(dot(ray, sunDirection), -1.0, 1.0);
    float sunAirMass  = SunAirMass(sunDirection.y);
    float viewAirMass = ViewAirMass(ray.y);
    float totalAirMass = sunAirMass + viewAirMass;

    // What is left of the beam where it meets the ground — the sun's own colour,
    // and the whole reason a low sun shifts hue.
    vec3 beam = radiantSun * exp(-extinction * sunAirMass);

    // Light reaching the eye crossed the atmosphere twice, in along the beam and
    // out along the view ray, and is extinguished over both. Attenuating the sum
    // rather than the beam alone is what keeps a sunset red on Earth: over a
    // short path the colour is the scattering coefficient's, and over a long one
    // the exponential wins and what survives is what it scatters LEAST.
    vec3 attenuation = exp(-extinction * totalAirMass);

    // How much air the view ray has to scatter in — a quantity, not a colour.
    // Saturating toward one is why the horizon is bright and the zenith, with a
    // thirtieth of the air, is not.
    float scattered = 1.0 - exp(-rayleighGrey * viewAirMass);

    // Haze, with its own extinction and its own colour. It shares the molecular
    // attenuation, which is what makes the halo around a setting sun take the
    // sun's colour rather than staying white.
    vec3 mie = (vec3(1.0) - exp(-mieCoeff * viewAirMass)) * exp(-mieCoeff * totalAirMass) *
               MiePhase(cosGamma, uSky.mie.w);

    // The second bounce onward, attenuated by the sun's path but not the view's.
    // Single scattering treats light knocked out of the line of sight as lost,
    // which over the horizon's thirty-odd air masses kills the blue exactly where
    // the sky is deepest and leaves the horizon green. This is what air actually
    // does with it.
    vec3 multiScattered = beam * (rayleigh * (scattered * uSky.atmosphere.x));

    vec3 sky = radiantSun * attenuation * (rayleigh * (RayleighPhase(cosGamma) * scattered) + mie) +
               multiScattered + uSky.nightColor.rgb;

    // The ground reflects the same beam off a Lambertian albedo, foreshortened by
    // the sun's elevation, and gets the night colour too — so it and the sky over
    // it fall to the same floor instead of the ground going black first.
    vec3 ground = uSky.groundColor.rgb * (beam * (max(sunDirection.y, 0.0) * kInvPi)) + uSky.nightColor.rgb;

    float skyward  = smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    vec3  radiance = mix(ground, sky, skyward);

    // The disk, gated by the same blend so the sun sets behind the ground rather
    // than shining up through it.
    float diskIntensity = uSky.nightColor.w;
    if (diskIntensity > 0.0)
    {
        float profile = SunDiskProfile(acos(cosGamma), uSky.sunDirection.w, uSky.sunRadiance.w, uSky.sunDiskColor.w);
        radiance += beam * uSky.sunDiskColor.rgb * (diskIntensity * profile * skyward);
    }

    outColor = vec4(max(radiance * uSky.groundColor.w, vec3(0.0)), 1.0);
}
