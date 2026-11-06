#ifndef ASSISI_SKY_SCATTER_GLSL
#define ASSISI_SKY_SCATTER_GLSL

// The air's scattered light toward one ray: a transcription of
// Render::SkyBodyContribution and the scattering half of Render::SkyRadiance in
// Sky.hpp, which is the specification. sky.frag draws it; the fog takes its
// colour from it, so the two can never disagree about what the sky looks like.

// Mirrors Render::SkyScatterConstants; the member order is the layout.
struct SkyScatter
{
    vec4 sunDirection;  // xyz = unit direction TO the sun
    vec4 sunRadiance;   // xyz = colour * intensity
    vec4 airScattering; // xyz = how strongly the air scatters each channel, w = how much air there is
    vec4 haze;          // xyz = how strongly haze scatters each channel, w = its forwardness
    vec4 groundColor;   // xyz = linear colour, w = sky exposure
    vec4 nightColor;    // xyz = linear colour
    vec4 atmosphere;    // x = sky bounce
    vec4 moonDirection; // xyz = unit direction TO the moon
    vec4 moonRadiance;  // xyz = colour * intensity
    vec4 moonDiskColor; // w = how much of the air's colour shift the moon takes
};

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
    // Kasten and Young's fit, whose coefficients are the fit itself.
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

// Floor on the Mie phase denominator. It reaches zero looking along the beam as
// the asymmetry approaches one, where the pow would return infinity.
const float kMinMiePhaseDenominator = 1e-4;

float MiePhase(float cosTheta, float asymmetry)
{
    float gg = asymmetry * asymmetry;
    float denom = max(1.0 + gg - 2.0 * asymmetry * cosTheta, kMinMiePhaseDenominator);
    return (1.0 - gg) / pow(denom, 1.5);
}

// A transmittance with `tint` of its colour shift left in, and all of its
// dimming. Mixing toward its own luminance rather than toward white is what makes
// this purely a hue control. See Render::TintedTransmittance for why the moon
// has one and the sun does not.
vec3 TintedTransmittance(vec3 transmittance, float tint)
{
    // Untouched at full tint, exactly: the sun always passes one.
    if (tint >= 1.0)
    {
        return transmittance;
    }
    float grey = dot(transmittance, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(grey), transmittance, max(tint, 0.0));
}

// What one body contributes to one ray. Scattering is linear in the beam, so the
// sun's share plus the moon's IS the two-body sky, with no seam in it.
struct BodyRadiance
{
    vec3 beam;   // what is left of the body's light where it meets the ground
    vec3 sky;    // what the air sends toward the eye from that beam
    vec3 ground; // what the ground half reflects of it
};

// `viewAirMass` and `scattered` belong to the ray alone, so the caller computes
// them once and both bodies share them.
BodyRadiance Contribution(SkyScatter s, vec3 ray, vec3 toBody, vec3 radiant, float viewAirMass, float scattered,
                          float tint)
{
    vec3  airScattering  = s.airScattering.rgb;
    vec3  hazeScattering = s.haze.rgb;
    // Everything that takes light out of a straight line: the molecules AND what
    // is suspended among them.
    vec3  extinction  = airScattering * s.airScattering.w + hazeScattering;
    float bodyAirMass = SunAirMass(toBody.y);
    float cosGamma    = clamp(dot(ray, toBody), -1.0, 1.0);

    vec3 transmittance = TintedTransmittance(exp(-extinction * bodyAirMass), tint);
    vec3 beam = radiant * transmittance;

    // Light reaching the eye crossed the atmosphere twice, in along the beam and
    // out along the view ray, and is extinguished over both.
    vec3 attenuation = TintedTransmittance(exp(-extinction * (bodyAirMass + viewAirMass)), tint);

    // How much of the beam the haze scatters toward the eye, and which way it
    // throws it. Its extinction is in `attenuation` with the air's.
    vec3 mie = (vec3(1.0) - exp(-hazeScattering * viewAirMass)) * MiePhase(cosGamma, s.haze.w);

    // The second bounce onward, attenuated by the body's path but not the view's.
    // Without it the horizon goes green, where single scattering has killed the
    // blue over thirty-odd air masses.
    vec3 bounced = beam * (airScattering * (scattered * s.atmosphere.x));

    BodyRadiance result;
    result.beam   = beam;
    result.sky    = radiant * attenuation * (airScattering * (RayleighPhase(cosGamma) * scattered) + mie) + bounced;
    result.ground = s.groundColor.rgb * (beam * (max(toBody.y, 0.0) * kInvPi));
    return result;
}

// Both bodies toward one ray, before the horizon blend, the disks and exposure.
struct SkyAir
{
    vec3 sky;      // the air's light, night floor included
    vec3 ground;   // the ground half's, night floor included
    vec3 sunBeam;  // what the disks are drawn from
    vec3 moonBeam;
};

SkyAir SkyAirRadiance(SkyScatter s, vec3 ray)
{
    // The colourless part of the air's extinction says how much of a beam the
    // air scatters rather than transmits. Saturating toward one is why the
    // horizon is bright and the zenith, with a thirtieth of the air, is not.
    vec3  airExtinction  = s.airScattering.rgb * s.airScattering.w;
    float greyExtinction = (airExtinction.r + airExtinction.g + airExtinction.b) / 3.0;
    float viewAirMass    = ViewAirMass(ray.y);
    float scattered      = 1.0 - exp(-greyExtinction * viewAirMass);

    // The sun takes the whole of the air's colour shift, always; only the moon
    // has a tint, because its brightness is a compromise.
    BodyRadiance fromSun  = Contribution(s, ray, s.sunDirection.xyz, s.sunRadiance.rgb, viewAirMass, scattered, 1.0);
    BodyRadiance fromMoon = Contribution(s, ray, s.moonDirection.xyz, s.moonRadiance.rgb, viewAirMass, scattered,
                                         s.moonDiskColor.w);

    // The night floor goes to both halves, so a moonless landscape and the sky
    // over it fall to the same floor instead of the ground going black first.
    SkyAir air;
    air.sky      = fromSun.sky + fromMoon.sky + s.nightColor.rgb;
    air.ground   = fromSun.ground + fromMoon.ground + s.nightColor.rgb;
    air.sunBeam  = fromSun.beam;
    air.moonBeam = fromMoon.beam;
    return air;
}

#endif
