#version 450

// Single-scattering through an atmosphere whose depth along a ray is an air-mass
// approximation rather than an integral: a handful of exps per pixel, no loop,
// and no history.
//
// This is a transcription of Render::SkyRadiance in Sky.hpp, which is also what
// the ambient term queries on the CPU. The two must agree, and every constant
// below is declared there — change one and change both.

layout(binding = 256) uniform SkyConstants
{
    mat4 invViewProjection;
    vec4 cameraPosition;  // xyz = world-space eye, w unused
    vec4 sunDirection;    // xyz = unit direction TO the sun, w = cos of the disk's outer edge
    vec4 sunRadiance;     // xyz = colour * intensity, w = cos of the disk's inner edge
    vec4 groundColor;     // xyz = linear colour, w = zenith optical depth
    vec4 nightColor;      // xyz = linear colour, w = haze
    vec4 params;          // x = intensity, y = sun disk intensity, zw unused
} uSky;

layout(location = 0) in vec4 vFarPoint;
layout(location = 0) out vec4 outColor;

// Rayleigh scattering per channel relative to blue: the inverse fourth power of
// wavelength, and the only reason the sky has a colour at all.
const vec3  kRayleighRatio   = vec3(0.1752, 0.4078, 1.0);
// Its mean — the colourless part, which sets how much is scattered rather than
// what colour comes out.
const float kRayleighGrey    = (0.1752 + 0.4078 + 1.0) / 3.0;
const float kMieAsymmetry    = 0.76;
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

vec3 Transmittance(float airMass, float zenithOpticalDepth)
{
    return exp(-kRayleighRatio * (zenithOpticalDepth * airMass));
}

// Both phase functions average one over the sphere rather than integrating to
// one, so they read as "against scattering the same light every way" and sit on
// the same footing as each other.
float RayleighPhase(float cosTheta)
{
    return 0.75 * (1.0 + cosTheta * cosTheta);
}

float MiePhase(float cosTheta)
{
    float gg = kMieAsymmetry * kMieAsymmetry;
    // Clamped because the denominator reaches zero looking straight at the sun
    // as the asymmetry approaches one, and the pow would return infinity.
    float denom = max(1.0 + gg - 2.0 * kMieAsymmetry * cosTheta, 1e-4);
    return (1.0 - gg) / pow(denom, 1.5);
}

void main()
{
    vec3 ray = normalize(vFarPoint.xyz / vFarPoint.w - uSky.cameraPosition.xyz);

    vec3  sunDirection       = uSky.sunDirection.xyz;
    vec3  radiantSun         = uSky.sunRadiance.rgb;
    float zenithOpticalDepth = uSky.groundColor.w;
    float haze               = uSky.nightColor.w;

    float cosGamma    = clamp(dot(ray, sunDirection), -1.0, 1.0);
    float sunAirMass  = SunAirMass(sunDirection.y);
    float viewAirMass = ViewAirMass(ray.y);

    // What is left of the beam where it meets the ground — the sun's own colour,
    // and the whole reason a low sun is orange.
    vec3 beam = radiantSun * Transmittance(sunAirMass, zenithOpticalDepth);

    // Light reaching the eye crossed the atmosphere twice, in along the beam and
    // out along the view ray, and is extinguished over both. Attenuating the sum
    // rather than the beam alone is what keeps a sunset red: over a short path
    // the colour is the scattering coefficient's, deep blue, and over a long one
    // the exponential wins and what survives is red.
    vec3 attenuation = Transmittance(sunAirMass + viewAirMass, zenithOpticalDepth);

    // How much air the view ray has to scatter in — a quantity, not a colour.
    // Saturating toward one is why the horizon is bright and the zenith, with a
    // thirtieth of the air, is not.
    float scattered = 1.0 - exp(-zenithOpticalDepth * kRayleighGrey * viewAirMass);

    // Haze scatters every wavelength alike and throws it hard forward, so it is
    // grey where it is added and coloured only by the attenuation it shares with
    // the molecular term. That sharing is what makes the halo around a setting
    // sun orange rather than white.
    float mie = MiePhase(cosGamma) * (1.0 - exp(-haze * viewAirMass)) *
                exp(-haze * (sunAirMass + viewAirMass));

    vec3 sky = radiantSun * attenuation *
                   (kRayleighRatio * (RayleighPhase(cosGamma) * scattered) + vec3(mie)) +
               uSky.nightColor.rgb;

    // The ground reflects the same beam off a Lambertian albedo, foreshortened by
    // the sun's elevation, and gets the night colour too — so it and the sky over
    // it fall to the same floor instead of the ground going black first.
    vec3 ground = uSky.groundColor.rgb * (beam * (max(sunDirection.y, 0.0) * kInvPi)) +
                  uSky.nightColor.rgb;

    float skyward  = smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    vec3  radiance = mix(ground, sky, skyward);

    // The disk, gated by the same blend so the sun sets behind the ground rather
    // than shining up through it. Softened over a tenth of its radius, because an
    // HDR disk with a hard edge aliases into a flickering dot.
    float sunDiskIntensity = uSky.params.y;
    if (sunDiskIntensity > 0.0)
    {
        float disk = smoothstep(uSky.sunDirection.w, uSky.sunRadiance.w, cosGamma);
        radiance += beam * (sunDiskIntensity * disk * skyward);
    }

    outColor = vec4(max(radiance * uSky.params.x, vec3(0.0)), 1.0);
}
