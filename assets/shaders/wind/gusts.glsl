// The level's gusts, around the camera: one pattern of stronger and weaker wind
// that drifts with the steady wind. Render::Gusts.hpp is the specification and
// its tests hold it; this transcribes it.

// Must match Render::kGustLengthAlong, kGustLengthAcross and kGustPeriod.
const float kGustLengthAlong  = 60.0;
const float kGustLengthAcross = 30.0;
const int   kGustPeriod       = 64;

// Must match Gusts.cpp's kGustDetailWeight, kGustDetailKey and kGustNoiseScale.
const float kGustDetailWeight = 0.5;
const uint  kGustDetailKey    = 0x9E3779B9u;
const float kGustNoiseScale   = 2.0;

// Mirrors Render::GustConstants.
struct GustConstants
{
    // xyz = the camera's cell; w = the gust factor.
    vec4 cell;
    // xy = which way the steady wind blows; z = its speed.
    vec4 wind;
};

// The same PCG hash as Render::WaterHash.
uint GustHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint WrapLattice(int index, int period)
{
    int wrapped = index % period;
    return uint(wrapped < 0 ? wrapped + period : wrapped);
}

// Perlin's improved gradients: toward the middles of a cube's twelve edges.
vec3 GustGradient(uint hash)
{
    const vec3 kGradients[12] = vec3[12](vec3(1.0, 1.0, 0.0), vec3(-1.0, 1.0, 0.0), vec3(1.0, -1.0, 0.0),
                                         vec3(-1.0, -1.0, 0.0), vec3(1.0, 0.0, 1.0), vec3(-1.0, 0.0, 1.0),
                                         vec3(1.0, 0.0, -1.0), vec3(-1.0, 0.0, -1.0), vec3(0.0, 1.0, 1.0),
                                         vec3(0.0, -1.0, 1.0), vec3(0.0, 1.0, -1.0), vec3(0.0, -1.0, -1.0));
    return kGradients[hash % 12u];
}

float GradientNoise(vec3 p, int period, uint key)
{
    vec3  floored = floor(p);
    ivec3 base    = ivec3(floored);
    vec3  inside  = p - floored;
    vec3  eased   = inside * inside * inside * (inside * (inside * 6.0 - 15.0) + 10.0);
    float sum     = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        ivec3 offset  = ivec3(int(corner & 1u), int((corner >> 1u) & 1u), int((corner >> 2u) & 1u));
        ivec3 lattice = base + offset;
        uint  hash    = GustHash(WrapLattice(lattice.x, period) ^
                                 GustHash(WrapLattice(lattice.y, period) ^ GustHash(WrapLattice(lattice.z, period) ^ key)));
        vec3  weights = mix(vec3(1.0) - eased, eased, vec3(offset));
        sum += weights.x * weights.y * weights.z * dot(GustGradient(hash), inside - vec3(offset));
    }
    return sum;
}

// -1 in the deepest lull, 1 in the strongest gust.
float GustNoise(vec3 cell)
{
    float coarse = GradientNoise(cell, kGustPeriod, 0u);
    float fine   = GradientNoise(cell * 2.0, kGustPeriod * 2, kGustDetailKey);
    return clamp(kGustNoiseScale * (coarse + kGustDetailWeight * fine) / (1.0 + kGustDetailWeight), -1.0, 1.0);
}

// The wind's speed @p fromCamera metres from the camera along world x and z.
float GustSpeedNear(GustConstants gusts, vec2 fromCamera)
{
    vec2 toward = gusts.wind.xy;
    vec2 across = vec2(-toward.y, toward.x);
    vec3 cell   = gusts.cell.xyz + vec3(dot(fromCamera, toward) / kGustLengthAlong,
                                        dot(fromCamera, across) / kGustLengthAcross, 0.0);
    return gusts.wind.z * max(1.0 + (gusts.cell.w - 1.0) * GustNoise(cell), 0.0);
}
