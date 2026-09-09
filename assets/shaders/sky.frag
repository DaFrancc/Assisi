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
    vec4 airScattering;   // xyz = how strongly the air scatters each channel, w = how much air there is
    vec4 haze;            // xyz = how strongly haze scatters each channel, w = its forwardness
    vec4 groundColor;     // xyz = linear colour, w = sky exposure
    vec4 nightColor;      // xyz = linear colour, w = disk intensity
    vec4 sunDiskColor;    // xyz = disk tint, w = limb darkening
    vec4 atmosphere;      // x = sky bounce, yzw unused
    vec4 moonDirection;   // xyz = unit direction TO the moon, w = its disk radius (radians)
    vec4 moonRadiance;    // xyz = colour * intensity, w = its disk intensity
    vec4 moonDiskColor;   // xyz = tint over the albedo texture, w = how much of the air's colour shift it takes
    vec4 moonUp;          // xyz = which way the disk image's top points, w unused
} uSky;

// The moon's albedo: an orthographic photograph of the one hemisphere a tidally
// locked moon ever shows. Loaded sRGB, so the view decodes it to linear here.
layout(binding = 0)   uniform texture2D uMoon;
layout(binding = 128) uniform sampler   uMoonSampler;

layout(location = 0) in vec4 vFarPoint;
layout(location = 0) out vec4 outColor;

const float kHorizonAirMass  = 35.567;
const float kTwilightFalloff = 10.0;
const float kHorizonSoftness = 0.01;
const float kInvPi           = 0.31830989;
// In units of dot(surface normal, direction to the sun). The moon is not shaded
// by a cosine — regolith is retro-reflective and its lit face is near uniform —
// so this is only how abruptly the terminator arrives.
const float kMoonTerminatorSoftness = 0.05;
const float kMinDiskRadius   = 1e-6;
const float kMinTangentLength = 1e-6;

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
    float acrossFace = min(angleToSun / max(radius, kMinDiskRadius), 1.0);
    float faceCosine = sqrt(max(1.0 - acrossFace * acrossFace, 0.0));
    return edge * (1.0 - limbDarkening * (1.0 - faceCosine));
}

// What one body contributes to one ray. Scattering is linear in the beam, so the
// sun's share plus the moon's is not an approximation of a two-body sky, it IS
// the two-body sky — and it is the only form with no seam in it. Handing the
// scattering to whichever body dominates would replace the sunset's afterglow
// with the moon's beam in a single frame.
//
// `viewAirMass` and `scattered` belong to the ray alone, so main computes them
// once and both calls share them.
struct BodyRadiance
{
    vec3 beam;          // what is left of the body's light where it meets the ground
    vec3 transmittance; // the fraction that survived, on its own
    vec3 sky;           // what the air sends toward the eye from that beam
    vec3 ground;        // what the ground half reflects of it
};

// A transmittance with `tint` of its colour shift left in, and all of its
// dimming. Mixing toward its own luminance rather than toward white is what makes
// this purely a hue control: luminance is linear, so every tint has the same
// luminance and the knob can never brighten or darken anything.
//
// The moon uses it and nothing else does. A low moon reddens exactly as hard as a
// low sun in physics, but real moonlight is a millionth of sunlight — below where
// colour vision works — so that reddening is seen almost entirely in the grey.
// This engine raises the moon's intensity four orders of magnitude to make night
// playable, and this is the other half of that compromise.
// It reaches every extinction term and no scattering coefficient, and that split
// is the whole of it. Extinction is what turns a low body orange; the scattering
// coefficients are why the sky is BLUE. So a moonlit sky stays blue at every
// tint, while the sunset-coloured aureole a low moon throws around itself fades
// with it.
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

BodyRadiance Contribution(vec3 ray, vec3 toBody, vec3 radiant, vec3 extinction, vec3 airScattering,
                          vec3 hazeScattering, float viewAirMass, float scattered, float tint)
{
    float bodyAirMass = SunAirMass(toBody.y);
    float cosGamma    = clamp(dot(ray, toBody), -1.0, 1.0);

    vec3 transmittance = TintedTransmittance(exp(-extinction * bodyAirMass), tint);
    vec3 beam = radiant * transmittance;

    // Light reaching the eye crossed the atmosphere twice, in along the beam and
    // out along the view ray, and is extinguished over both.
    vec3 attenuation = TintedTransmittance(exp(-extinction * (bodyAirMass + viewAirMass)), tint);

    // How much of the beam the haze scatters toward the eye, and which way it
    // throws it. Its extinction is not applied here — it is in `attenuation`
    // with the air's, so both in-scattered terms are dimmed by the same thing
    // they were dimmed by on the way in.
    vec3 mie = (vec3(1.0) - exp(-hazeScattering * viewAirMass)) * MiePhase(cosGamma, uSky.haze.w);

    // The second bounce onward, attenuated by the body's path but not the view's.
    // Without it the horizon goes green, where single scattering has killed the
    // blue over thirty-odd air masses.
    vec3 bounced = beam * (airScattering * (scattered * uSky.atmosphere.x));

    BodyRadiance result;
    result.beam          = beam;
    result.transmittance = transmittance;
    result.sky           = radiant * attenuation * (airScattering * (RayleighPhase(cosGamma) * scattered) + mie) + bounced;
    result.ground        = uSky.groundColor.rgb * (beam * (max(toBody.y, 0.0) * kInvPi));
    return result;
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
    vec3 ray = normalize(vFarPoint.xyz / vFarPoint.w - uSky.cameraPosition.xyz);

    vec3  sunDirection = uSky.sunDirection.xyz;
    vec3  radiantSun   = uSky.sunRadiance.rgb;
    // The scattering strengths are two different things and must not be confused.
    // As a RATIO they tint the in-scattered light; scaled by how much air there
    // is they are an EXTINCTION, and only that form belongs in an exp().
    vec3  airScattering  = uSky.airScattering.rgb;
    vec3  hazeScattering = uSky.haze.rgb;
    // Everything that takes light out of a straight line: the molecules AND what
    // is suspended among them. Haze dims the beam as surely as air does, and
    // leaving it out lights an overcast noon like a clear one.
    vec3  airExtinction  = airScattering * uSky.airScattering.w;
    vec3  extinction     = airExtinction + hazeScattering;
    float greyExtinction = (airExtinction.r + airExtinction.g + airExtinction.b) / 3.0;

    vec3  moonDirection = uSky.moonDirection.xyz;
    vec3  radiantMoon   = uSky.moonRadiance.rgb;

    // The ray's own share of the work, done once for both bodies. Saturating
    // toward one is why the horizon is bright and the zenith, with a thirtieth of
    // the air, is not.
    float viewAirMass = ViewAirMass(ray.y);
    float scattered   = 1.0 - exp(-greyExtinction * viewAirMass);

    // The sun takes the whole of the air's colour shift, always: a sunset is seen
    // in daylight, at full colour, and is not something to soften. Only the moon
    // has a tint, and only because its brightness is a compromise.
    BodyRadiance fromSun  = Contribution(ray, sunDirection, radiantSun, extinction, airScattering,
                                         hazeScattering, viewAirMass, scattered, 1.0);
    BodyRadiance fromMoon = Contribution(ray, moonDirection, radiantMoon, extinction, airScattering,
                                         hazeScattering, viewAirMass, scattered, uSky.moonDiskColor.w);

    // The night floor goes to both halves, so a moonless landscape and the sky
    // over it fall to the same floor instead of the ground going black first.
    vec3 sky    = fromSun.sky + fromMoon.sky + uSky.nightColor.rgb;
    vec3 ground = fromSun.ground + fromMoon.ground + uSky.nightColor.rgb;

    float skyward  = smoothstep(-kHorizonSoftness, kHorizonSoftness, ray.y);
    vec3  radiance = mix(ground, sky, skyward);

    // Both disks, gated by the same blend so a body sets behind the ground rather
    // than shining up through it.
    float sunDiskIntensity = uSky.nightColor.w;
    if (sunDiskIntensity > 0.0)
    {
        float cosGamma = clamp(dot(ray, sunDirection), -1.0, 1.0);
        float profile = SunDiskProfile(acos(cosGamma), uSky.sunDirection.w, uSky.sunRadiance.w, uSky.sunDiskColor.w);
        radiance += fromSun.beam * uSky.sunDiskColor.rgb * (sunDiskIntensity * profile * skyward);
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
        radiance += fromMoon.beam * uSky.moonDiskColor.rgb * face * (moonDiskIntensity * profile * skyward);
    }

    outColor = vec4(max(radiance * uSky.groundColor.w, vec3(0.0)), 1.0);
}
