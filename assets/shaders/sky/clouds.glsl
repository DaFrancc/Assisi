#ifndef ASSISI_SKY_CLOUDS_GLSL
#define ASSISI_SKY_CLOUDS_GLSL

// The clouds: where they are, how dense, and the light they gather along a ray.
// cloud_map.comp draws the sky map with it and cloud_shadow.comp their shadow,
// so the clouds and their shadows are the same clouds.
//
// A weather map says where there is cloud and of what kind, each kind has its
// own height profile, and shape noise carved by detail noise makes the clouds.
// Variety comes from the first two. Shape after Schneider, "Nubis" (SIGGRAPH
// 2017); light after Hillaire, "Physically Based Sky, Atmosphere and Cloud
// Rendering in Frostbite" (SIGGRAPH 2016).
//
// Positions are in metres relative to the camera, y up; heights are from the
// world's zero. The CPU wraps each noise's offset into its repeat in double, so
// nothing here loses precision however far the camera or the wind has gone.

// One slab of cloud. Mirrors Render::CloudSlabConstants; the member order is
// the layout.
struct CloudSlab
{
    vec4 layer;    // x = base height, y = top height (metres), z = coverage, w = extinction per metre at full density
    vec4 shape;    // xy = the shape noise's offset across, z = up, w = repeats per metre
    vec4 detail;   // xy = the detail noise's offset across, z = up, w = repeats per metre
    vec4 weatherA; // xy = the air-mass noise's offset across, zw = the patch noise's
    vec4 weatherB; // xy = the kind noise's offset across, z = puffiness
};

// Mirrors Render::CloudConstants; the member order is the layout.
struct CloudConstants
{
    CloudSlab low;
    vec4 scales;        // repeats per metre of x = the air-mass noise, y = the patch noise, z = the kind noise; w = the camera's height
    vec4 march;         // x = steps through the layer, z = a map texel's width in radians
    vec4 weatherArea;   // xy = the weather texture's centre across, relative to the camera; z = 1 / its width in metres; w = a texel's width
    vec4 sunDirection;  // xyz = unit direction to the sun, w = 1 to light the clouds by it
    vec4 sunBeam;       // rgb = the sunlight arriving at the clouds, times the sky's exposure
    vec4 moonDirection; // xyz = unit direction to the moon, w = 1 to light the clouds by it
    vec4 moonBeam;      // rgb = the moonlight arriving at the clouds, times the sky's exposure
    vec4 ambientSky;    // rgb = the clear sky's light from above
    vec4 ambientGround; // rgb = the light from below
};

// The noise textures, from cloud_noise.comp. Must match Render::CloudNoiseSlot.
layout(binding = 0)   uniform texture3D uCloudShape;
layout(binding = 1)   uniform texture3D uCloudDetail;
layout(binding = 128) uniform sampler   uCloudNoiseSampler;
// The weather map baked for this frame by cloud_weather.comp: r = coverage,
// g = kind. Must match Render::CloudNoiseSlot.
layout(binding = 2)   uniform texture2D uCloudWeather;

// Must match Render::kCloudShapeNoiseSize and Render::kCloudDetailNoiseSize.
const float kCloudShapeNoiseSize  = 128.0;
const float kCloudDetailNoiseSize = 64.0;

// The layer curves away with the planet, which lays distant clouds down onto
// the horizon instead of stretching them to infinity.
const float kPlanetRadius = 6360000.0;
// Past this the air in front has faded the clouds away.
const float kMaxCloudDistance = 150000.0;
// Near the horizon a ray crosses the layer for a hundred kilometres; spreading
// its steps over all of it would step over whole clouds.
const float kMaxCloudMarchLength = 20000.0;
// A ray gets its steps per thickness of the layer it crosses, up to this many
// times the steps straight up. With the same steps for every ray, a ray along
// the horizon steps a kilometre at a time and the clouds there come out as
// terraced streaks.
const float kMaxStepScale = 3.0;
// A ray this dark has met solid cloud.
const float kOpaqueTransmittance = 0.01;

// The weather map: slices of the shape texture at different depths, so the
// three maps do not line up. The air-mass noise opens and closes stretches of
// sky, the patch noise groups clouds, and the kind noise mixes heaped and flat
// cloud. The variations are at half coverage; they shrink toward clear and
// overcast, so a clear sky stays clear and an overcast one closed.
const float kAirMassSlice = 0.5;
const float kPatchSlice   = 0.2;
const float kKindSlice    = 0.8;
const float kAirMassVariation = 0.9;
const float kPatchVariation   = 0.6;
const float kKindVariation    = 0.6;

// Height profiles, as fractions of the layer: every kind rises over the bottom
// tenth; stratus fades out by four tenths, cumulus rounds off to the top.
const float kBaseRise   = 0.1;
const vec2  kStratusTop = vec2(0.25, 0.35);
const vec2  kCumulusTop = vec2(0.6, 1.0);

// The eroded shape noise spends almost all of itself between these, so the
// coverage cuts it between them and the share left is about the coverage.
// Measured over the generated textures.
const float kShapeFloor   = 0.45;
const float kShapeCeiling = 0.9;
// How far the detail noise eats into an edge, in units of density.
const float kDetailErosion = 0.2;
// Over the bottom tenth the detail flips from wispy to billowing: ragged bases,
// rounded tops.
const float kDetailFlipHeight = 10.0;
// A power below one firms thin cloud up, so an edge reads as an edge rather
// than haze (Schneider, "Nubis Cubed", SIGGRAPH 2023).
const float kDensitySharpening = 0.6;

// The light march: steps, the first as a share of the layer's thickness, and
// growth per step; the detail noise only for the first few.
const int   kLightSteps       = 7;
const float kLightFirstStep   = 0.01;
const float kLightStepGrowth  = 1.8;
const int   kLightDetailSteps = 1;
// Once a view ray is this hidden, what lies further along it shows little, so
// its light marches take three long steps instead, reaching as far (Schneider
// 2015's cheap light samples).
const float kCheapLightBelow      = 0.3;
// Past this many metres a cloud's shading is finer than a map texel, so its
// light marches take the long steps too.
const float kCheapLightBeyond     = 10000.0;
// Through clear air the view march takes this many steps at a time, and goes
// back to it after this many clear steps in a row.
const int   kStrideSteps        = 2;
const int   kClearStepsToStride = 6;
// How many samples in a row inside a cloud share one march toward the light.
const int   kLightMarchEvery    = 2;
const int   kCheapLightSteps      = 3;
const float kCheapLightFirstStep  = 0.1;
const float kCheapLightStepGrowth = 2.2;

// Light bouncing inside the cloud as further, dimmer passes, each through less
// of the density and less forward (Wrenninge et al., "Oz: The Great and
// Volumetric", 2013). A pass's energy must not exceed its share of the density,
// or the bounces fill every shadow and the clouds look like cotton.
const int   kScatterOctaves    = 3;
const float kOctaveEnergy      = 0.5;
const float kOctaveExtinction  = 0.5;
const float kOctaveForwardness = 0.5;

// Light scattered once follows the droplets' phase function: Jendersie and
// d'Eon's fit of Mie scattering ("An Approximate Mie Scattering Function for Fog
// and Cloud Rendering", SIGGRAPH 2023) at 20 micrometre droplets. Its forward
// spike, half a degree wide, makes thin cloud glow where it hides the sun.
const float kDropletForward     = 0.9946; // the Henyey-Greenstein lobe's forwardness
const float kDropletDraine      = 0.5938; // the Draine lobe's forwardness
const float kDropletAlpha       = 27.11;  // the Draine lobe's shape
const float kDropletDraineShare = 0.4982;
// Bounced light has lost the droplets' shape, so it takes a plain lobe. The
// Draine lobe's shape term would starve the sideways light that lights a
// cloud's sides.
const float kBouncedForward = 0.5;
// Deep in thick cloud the light has bounced dozens of times and spread evenly,
// falling off far slower than any pass above: the diffusion of light through a
// thick layer, 1 / (1 + 0.75 depth (1 - g)) for a cloud's g of 0.85. It is what
// lets a sixth of the sunlight out of the base of an overcast deck, which is why
// overcast is grey and not black.
const float kDiffusedFalloff = 0.75 * (1.0 - 0.85);

// Bases are darker than sides, little light reaching them from below, and thin
// cloud near the lit surface is darker than thick: the creases between billows.
const vec2  kDarkBase       = vec2(0.07, 0.14);
const float kDarkBaseFloor  = 0.1;
const float kDarkBaseCurve  = 0.8;
const float kCreaseFloor    = 0.05;
const vec2  kCreaseHeights  = vec2(0.3, 0.85);
const vec2  kCreasePowers   = vec2(0.5, 2.0);

// The sky's light inside a cloud: a share of it at the base, shut out toward
// the core, and only half the sky's colour, or every shadowed side goes blue.
const float kAmbientAtBase      = 0.3;
const float kAmbientCoreFalloff = 0.5;
const float kAmbientSaturation  = 0.5;

const vec3 kCloudOctaveWeights = vec3(0.625, 0.25, 0.125);
const vec3 kCloudLuminanceWeights = vec3(0.2126, 0.7152, 0.0722);

const float kCloudPi = 3.14159265359;
// Floor on the phase function's denominator, which reaches zero looking along
// the beam as the lobe's forwardness approaches one.
const float kMinPhaseDenominator = 1e-6;

float CloudRemap(float value, float fromLow, float fromHigh, float toLow, float toHigh)
{
    return toLow + (value - fromLow) / (fromHigh - fromLow) * (toHigh - toLow);
}

float CloudRemapClamped(float value, float fromLow, float fromHigh, float toLow, float toHigh)
{
    return CloudRemap(clamp((value - fromLow) / (fromHigh - fromLow), 0.0, 1.0), 0.0, 1.0, toLow, toHigh);
}

// Height above the world's zero of a camera-relative point, on a planet that
// curves away beneath the camera. The parabola matches the sphere to well under
// a metre over the clouds' range, and keeps the precision length() of a
// planet-sized vector would lose.
float CloudHeightAt(CloudConstants c, vec3 point)
{
    return c.scales.w + point.y + dot(point.xz, point.xz) * (0.5 / kPlanetRadius);
}

// How far along a unit ray from the camera it reaches `height`. Zero for a
// height at or below the camera, and the camera is always below the clouds.
float CloudRayToHeight(CloudConstants c, vec3 ray, float height)
{
    float rise = height - c.scales.w;
    if (rise <= 0.0)
    {
        return 0.0;
    }
    float curve = (1.0 - ray.y * ray.y) * (0.5 / kPlanetRadius);
    // rise = t * ray.y + t^2 * curve, solved in the form that stays exact as
    // the curve vanishes looking straight up.
    float root = sqrt(ray.y * ray.y + 4.0 * curve * rise);
    float denominator = ray.y + root;
    return denominator > 0.0 ? 2.0 * rise / denominator : kMaxCloudDistance;
}

// Which mip of a noise texture repeating `repeatsPerMetre` holds detail as fine
// as `footprint` metres and no finer.
float CloudNoiseLod(float footprint, float repeatsPerMetre, float size)
{
    return max(log2(max(footprint * repeatsPerMetre * size, 1.0)), 0.0);
}

// A flat slice through the shape texture, as a 2D map.
vec4 CloudMapNoise(vec2 uv, float slice, float lod)
{
    return textureLod(sampler3D(uCloudShape, uCloudNoiseSampler), vec3(uv.x, slice, uv.y), lod);
}

// What the weather map says at a point: how much of the sky is cloud there,
// and how heaped the cloud is, from 0 (flat stratus) to 1 (towering cumulus).
struct CloudWeather
{
    float coverage;
    float kind;
};

// The weather from its three noises: what cloud_weather.comp bakes into the
// weather texture each frame.
CloudWeather CloudWeatherFromNoise(CloudConstants c, CloudSlab s, vec2 across, float footprint)
{
    float airMass = CloudMapNoise(across * c.scales.x + s.weatherA.xy, kAirMassSlice,
                                  CloudNoiseLod(footprint, c.scales.x, kCloudShapeNoiseSize)).r;
    float patches = CloudMapNoise(across * c.scales.y + s.weatherA.zw, kPatchSlice,
                                  CloudNoiseLod(footprint, c.scales.y, kCloudShapeNoiseSize)).r;
    float kinds   = CloudMapNoise(across * c.scales.z + s.weatherB.xy, kKindSlice,
                                  CloudNoiseLod(footprint, c.scales.z, kCloudShapeNoiseSize)).g;

    float level = s.layer.z;
    float room  = 2.0 * min(level, 1.0 - level);
    CloudWeather weather;
    weather.coverage = clamp(level + ((airMass - 0.5) * kAirMassVariation + (patches - 0.5) * kPatchVariation) * room,
                             0.0, 1.0);
    weather.kind = clamp(s.weatherB.z + (kinds - 0.5) * kKindVariation, 0.0, 1.0);
    return weather;
}

// The weather at a camera-relative point across, from the weather texture: one
// read, where the noises would take three.
CloudWeather CloudWeatherAt(CloudConstants c, vec2 across)
{
    vec2 uv = (across - c.weatherArea.xy) * c.weatherArea.z + 0.5;
    vec2 baked = textureLod(sampler2D(uCloudWeather, uCloudNoiseSampler), uv, 0.0).rg;
    CloudWeather weather;
    weather.coverage = baked.r;
    weather.kind = baked.g;
    return weather;
}

// How much cloud the kind allows at `fraction` of the way up the layer.
float CloudHeightProfile(float fraction, float kind)
{
    vec2  top    = mix(kStratusTop, kCumulusTop, kind);
    float rising = clamp(fraction / kBaseRise, 0.0, 1.0);
    float fading = clamp(CloudRemap(fraction, top.x, top.y, 1.0, 0.0), 0.0, 1.0);
    return rising * fading;
}

// One look at the cloud at a point.
struct CloudSample
{
    float density;  // from zero to one; times layer.w it is the extinction per metre
    float envelope; // how deep inside its kind's profile and its coverage the point is, before the noise
};

// `weather` is what the weather map says there, `fraction` how far up the slab
// the point is, and `footprint` how many metres of cloud one sample stands
// for. Without `detailed`, the detail noise is skipped: for shadows, whose
// edges are soft anyway.
CloudSample CloudAt(CloudConstants c, CloudSlab s, CloudWeather weather, vec3 point, float height, float fraction,
                    float footprint, bool detailed)
{
    CloudSample result;
    result.density  = 0.0;
    result.envelope = 0.0;
    if (fraction <= 0.0 || fraction >= 1.0)
    {
        return result;
    }
    float profile = CloudHeightProfile(fraction, weather.kind);
    result.envelope = profile * weather.coverage;
    if (result.envelope <= 0.0)
    {
        return result;
    }

    vec3 shapeUv = vec3(point.x * s.shape.w + s.shape.x, height * s.shape.w + s.shape.z, point.z * s.shape.w + s.shape.y);
    vec4 shape = textureLod(sampler3D(uCloudShape, uCloudNoiseSampler), shapeUv,
                            CloudNoiseLod(footprint, s.shape.w, kCloudShapeNoiseSize));
    // The billows, broken into puffs where the Worley octaves are weak, and cut
    // to the kind's height.
    float erosion = dot(shape.gba, kCloudOctaveWeights);
    float base = clamp(CloudRemap(shape.r, erosion - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0) * profile;
    // Coverage decides how much of that is cloud, and how dense: thin, small
    // clouds where it is low, so a fair sky's clouds are light.
    float cut = mix(kShapeCeiling, kShapeFloor, weather.coverage);
    float covered = clamp(CloudRemap(base, cut, 1.0, 0.0, 1.0), 0.0, 1.0) * weather.coverage;
    if (covered <= 0.0 || !detailed)
    {
        result.density = covered;
        return result;
    }

    vec3 detailUv = vec3(point.x * s.detail.w + s.detail.x, height * s.detail.w + s.detail.z,
                         point.z * s.detail.w + s.detail.y);
    vec3 detail = textureLod(sampler3D(uCloudDetail, uCloudNoiseSampler), detailUv,
                             CloudNoiseLod(footprint, s.detail.w, kCloudDetailNoiseSize)).rgb;
    float wisps = dot(detail, kCloudOctaveWeights);
    float edge  = mix(wisps, 1.0 - wisps, clamp(fraction * kDetailFlipHeight, 0.0, 1.0));
    result.density = pow(clamp(CloudRemap(covered, edge * kDetailErosion, 1.0, 0.0, 1.0), 0.0, 1.0), kDensitySharpening);
    return result;
}

float CloudSlabFraction(CloudSlab s, float height)
{
    return (height - s.layer.x) / (s.layer.y - s.layer.x);
}

// Density in a slab at a camera-relative point under `weather`, working out its
// height first.
float CloudDensityAt(CloudConstants c, CloudSlab s, CloudWeather weather, vec3 point, float footprint, bool detailed)
{
    float height = CloudHeightAt(c, point);
    return CloudAt(c, s, weather, point, height, CloudSlabFraction(s, height), footprint, detailed).density;
}

// The optical depth from a point toward a light, through the slab's cloud. The
// march is short against the weather map's patches, so it keeps the weather at
// its start rather than reading the map again at every step. A `cheap` march
// reaches as far in fewer, longer steps.
float CloudOpticalDepthToward(CloudConstants c, CloudSlab s, CloudWeather weather, vec3 point, vec3 toLight,
                              float footprint, bool cheap)
{
    int   steps      = cheap ? kCheapLightSteps : kLightSteps;
    float growth     = cheap ? kCheapLightStepGrowth : kLightStepGrowth;
    float stepLength = (s.layer.y - s.layer.x) * (cheap ? kCheapLightFirstStep : kLightFirstStep);
    float travelled  = 0.0;
    float depth      = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        vec3 middle = point + toLight * (travelled + 0.5 * stepLength);
        depth += CloudDensityAt(c, s, weather, middle, max(footprint, stepLength), i < kLightDetailSteps) * stepLength;
        travelled += stepLength;
        stepLength *= growth;
    }
    return depth * s.layer.w;
}

float HenyeyGreenstein(float cosTheta, float forwardness)
{
    float gg = forwardness * forwardness;
    float denominator = max(1.0 + gg - 2.0 * forwardness * cosTheta, kMinPhaseDenominator);
    return (1.0 - gg) / (4.0 * kCloudPi * pow(denominator, 1.5));
}

float Draine(float cosTheta, float forwardness, float alpha)
{
    float gg = forwardness * forwardness;
    return HenyeyGreenstein(cosTheta, forwardness) * (1.0 + alpha * cosTheta * cosTheta) /
           (1.0 + alpha * (1.0 + 2.0 * gg) / 3.0);
}

// The droplets' phase function, for light scattered once.
float DropletPhase(float cosTheta)
{
    return mix(HenyeyGreenstein(cosTheta, kDropletForward), Draine(cosTheta, kDropletDraine, kDropletAlpha),
               kDropletDraineShare);
}

// The light a body scatters toward the eye at a point `depth` deep in cloud
// toward it, per unit of the cloud's scattering.
vec3 CloudBodyLight(vec3 beam, float depth, float cosTheta)
{
    float sum        = DropletPhase(cosTheta) * exp(-depth) + 1.0 / (4.0 * kCloudPi * (1.0 + kDiffusedFalloff * depth));
    float energy     = kOctaveEnergy;
    float extinction = kOctaveExtinction;
    float forward    = kBouncedForward;
    for (int octave = 1; octave < kScatterOctaves; ++octave)
    {
        sum += energy * HenyeyGreenstein(cosTheta, forward) * exp(-depth * extinction);
        energy     *= kOctaveEnergy;
        extinction *= kOctaveExtinction;
        forward    *= kOctaveForwardness;
    }
    return beam * sum;
}

// The darkening of a cloud's base and creases, for a point `fraction` up the
// slab with `density`, `depth` deep toward the light.
float CloudDirectShading(float fraction, float density, float depth)
{
    float base = pow(CloudRemapClamped(fraction, kDarkBase.x, kDarkBase.y, kDarkBaseFloor, 1.0), kDarkBaseCurve);
    float power = CloudRemapClamped(fraction, kCreaseHeights.x, kCreaseHeights.y, kCreasePowers.x, kCreasePowers.y);
    float crease = mix(kCreaseFloor + pow(density, power), 1.0, clamp(depth, 0.0, 1.0));
    return base * crease;
}

// What a view ray collects from the clouds, front to back.
struct CloudRay
{
    vec3  light;         // the light the clouds send toward the eye
    float transmittance; // how much of what lies behind the clouds shows through
    float hidden;        // how much the clouds hid, as weights for the two below
    float distance;      // how far away the clouds are, weighted by how much each hid
    float height;        // the height there
};

CloudRay NoClouds()
{
    CloudRay result;
    result.light = vec3(0.0);
    result.transmittance = 1.0;
    result.hidden = 0.0;
    result.distance = 0.0;
    result.height = 0.0;
    return result;
}

// Add a stretch of cloud `t` along the ray at `height`, letting `stepTransmittance`
// of the light behind it through and scattering `light` per unit of what it hides.
// Integrated over the step: a cloud scatters what it removes, so the step adds
// its light times the share of the ray it hides, right at any step length.
void CloudAccumulate(inout CloudRay ray, vec3 light, float stepTransmittance, float t, float height)
{
    float share = ray.transmittance * (1.0 - stepTransmittance);
    ray.light    += light * share;
    ray.hidden   += share;
    ray.distance += t * share;
    ray.height   += height * share;
    ray.transmittance *= stepTransmittance;
}

// March a unit ray from the camera through one slab, adding what it meets.
// `jitter`, in [0, 1), staggers where the steps fall from one ray to the next.
void MarchSlab(CloudConstants c, CloudSlab s, vec3 ray, float jitter, int steps, inout CloudRay result)
{
    if (s.layer.z <= 0.0 || result.transmittance < kOpaqueTransmittance)
    {
        return;
    }
    float start = CloudRayToHeight(c, ray, s.layer.x);
    float end   = min(CloudRayToHeight(c, ray, s.layer.y), start + kMaxCloudMarchLength);
    if (end <= start || start >= kMaxCloudDistance)
    {
        return;
    }

    float crossings = clamp((end - start) / (s.layer.y - s.layer.x), 1.0, kMaxStepScale);
    steps = max(int(float(steps) * crossings), 1);
    float stepLength = (end - start) / float(steps);
    float cosSun     = dot(ray, c.sunDirection.xyz);
    float cosMoon    = dot(ray, c.moonDirection.xyz);
    vec3  skyAbove   = mix(vec3(dot(c.ambientSky.rgb, kCloudLuminanceWeights)), c.ambientSky.rgb, kAmbientSaturation);

    // Through clear air the march strides, looking only at the shape noise;
    // meeting cloud, it steps back once and marches finely, with the detail and
    // the light, until the air has been clear for a while (Schneider 2017).
    // Every sample still lands on the same grid of steps.
    int  step     = 0;
    bool striding = true;
    int  clearRun = 0;
    // The light's march is the dearest part of a sample, and the depth toward
    // the light changes slowly along a view ray inside a cloud, so one march
    // serves kLightMarchEvery samples in a row.
    float sunDepth   = 0.0;
    float moonDepth  = 0.0;
    int   sinceLight = kLightMarchEvery;
    while (step < steps)
    {
        float t        = start + (float(step) + jitter) * stepLength;
        vec3  point    = ray * t;
        float height   = CloudHeightAt(c, point);
        float fraction = CloudSlabFraction(s, height);
        // One sample stands for the patch of sky one map texel covers at this
        // distance; the step runs along the ray, which does not blur across.
        float footprint = t * c.march.z;
        CloudWeather weather;
        CloudSample  cloud;
        cloud.density = 0.0;
        if (fraction > 0.0 && fraction < 1.0)
        {
            weather = CloudWeatherAt(c, point.xz);
            cloud   = CloudAt(c, s, weather, point, height, fraction, footprint, !striding);
        }
        if (striding)
        {
            // The detail noise only wears cloud away, so where the shape alone
            // finds none there is none.
            if (cloud.density > 0.0)
            {
                striding = false;
                clearRun = 0;
                step = max(step - 1, 0);
            }
            else
            {
                step += kStrideSteps;
            }
            continue;
        }
        ++step;
        if (cloud.density <= 0.0)
        {
            ++clearRun;
            striding = clearRun >= kClearStepsToStride;
            sinceLight = kLightMarchEvery;
            continue;
        }
        clearRun = 0;
        bool cheap = result.transmittance < kCheapLightBelow || t > kCheapLightBeyond;
        bool marchLight = sinceLight >= kLightMarchEvery;
        sinceLight = marchLight ? 1 : sinceLight + 1;

        // The sky's light, shut out toward the cloud's core and its base.
        float open = pow(1.0 - clamp(cloud.envelope, 0.0, 1.0), kAmbientCoreFalloff) *
                     mix(kAmbientAtBase, 1.0, clamp(fraction, 0.0, 1.0));
        vec3 light = mix(c.ambientGround.rgb, skyAbove, clamp(fraction, 0.0, 1.0)) * open;
        if (c.sunDirection.w > 0.0)
        {
            if (marchLight)
            {
                sunDepth = CloudOpticalDepthToward(c, s, weather, point, c.sunDirection.xyz, footprint, cheap);
            }
            light += CloudBodyLight(c.sunBeam.rgb, sunDepth, cosSun) *
                     CloudDirectShading(fraction, cloud.density, sunDepth);
        }
        if (c.moonDirection.w > 0.0)
        {
            if (marchLight)
            {
                moonDepth = CloudOpticalDepthToward(c, s, weather, point, c.moonDirection.xyz, footprint, cheap);
            }
            light += CloudBodyLight(c.moonBeam.rgb, moonDepth, cosMoon) *
                     CloudDirectShading(fraction, cloud.density, moonDepth);
        }

        CloudAccumulate(result, light, exp(-cloud.density * s.layer.w * stepLength), t, height);
        if (result.transmittance < kOpaqueTransmittance)
        {
            // Solid. The sun's disk is hundreds of times brighter than the sky,
            // so even the last percent would show it as a circle on the cloud.
            result.transmittance = 0.0;
            return;
        }
    }
}

// Everything a unit ray from the camera meets in the clouds.
CloudRay MarchClouds(CloudConstants c, vec3 ray, float jitter)
{
    CloudRay result = NoClouds();
    MarchSlab(c, c.low, ray, jitter, int(c.march.x), result);
    if (result.hidden > 0.0)
    {
        result.distance /= result.hidden;
        result.height   /= result.hidden;
    }
    return result;
}

// The optical depth from `base`, on the layer's base, up through the layer
// toward the light: what cloud_shadow.comp needs.
float CloudShadowDepth(CloudConstants c, vec3 base, vec3 toLight, int steps, float footprint)
{
    CloudSlab s = c.low;
    float stepLength = (s.layer.y - s.layer.x) / toLight.y / float(steps);
    float depth      = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        vec3  point   = base + toLight * ((float(i) + 0.5) * stepLength);
        float spread  = max(stepLength, footprint);
        depth += CloudDensityAt(c, s, CloudWeatherAt(c, point.xz), point, spread, false) * stepLength;
    }
    return depth * s.layer.w;
}

#endif
