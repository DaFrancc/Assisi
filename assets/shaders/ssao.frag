#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Screen-space occlusion, step two: what fraction of the hemisphere over each
// pixel's surface the depth buffer leaves open, before the blur.
//
// Depth only. The normal is rebuilt from the distances around the pixel rather
// than read from a G-buffer the forward renderer does not have, and the
// kernel is turned about it by an angle that depends on the pixel's place on
// the screen and nothing else. Nothing here reads a previous frame, so nothing
// here can flicker that the depth does not. Render::Ssao.hpp holds the CPU half
// of every function below, and the two must agree.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out float outOcclusion;

// Distance in metres per pixel (ssao_depth.frag).
layout(binding = 0) uniform texture2D uDistance;

// Mirrors Render::SsaoConstants.
layout(binding = 256) uniform SsaoConstants
{
    vec4  projection; // x = xScale, y = yScale, z = depthScale, w = depthBias
    vec4  viewport;   // xy = target size in pixels, z = far plane distance, w = pixels per metre at one metre
    vec4  params;     // x = radius (m), y = strength, z = widest radius in pixels, w = bias as a fraction of radius
    vec4  blur;       // x = relative depth tolerance (ssao_blur.frag)
    uvec4 counts;     // x = samples to take
    vec4  kernel[32]; // xyz = offset about +Z as a fraction of the radius
} uSsao;

// Must match Render::kSsaoTileSize and Render::kSsaoTileRotation.
const uint kTileSize = 4u;
const uint kTileRotationCount = kTileSize * kTileSize;
const uint kTileRotation[kTileRotationCount] = uint[kTileRotationCount](
    0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);

const float kTwoPi = 6.28318530718;

// How close to the far plane a distance may come and still be geometry. The
// depth clear lands exactly on it, and so does the sky; neither has a surface
// to occlude.
const float kFarPlaneFraction = 0.999;

ivec2 TargetSize()
{
    return ivec2(uSsao.viewport.xy);
}

float DistanceAt(ivec2 pixel)
{
    return texelFetch(uDistance, clamp(pixel, ivec2(0), TargetSize() - 1), 0).r;
}

// Render::SsaoViewPosition.
vec3 ViewPosition(vec2 pixel, float distance)
{
    vec2 ndc = vec2((pixel.x + 0.5) / uSsao.viewport.x * 2.0 - 1.0, 1.0 - (pixel.y + 0.5) / uSsao.viewport.y * 2.0);
    return vec3(ndc.x / uSsao.projection.x * distance, ndc.y / uSsao.projection.y * distance, -distance);
}

// Render::SsaoPixelOf.
vec2 PixelOf(vec3 position)
{
    float distance = -position.z;
    vec2 ndc = vec2(position.x * uSsao.projection.x / distance, position.y * uSsao.projection.y / distance);
    return vec2((ndc.x + 1.0) * 0.5 * uSsao.viewport.x, (1.0 - ndc.y) * 0.5 * uSsao.viewport.y) - vec2(0.5);
}

// Render::SsaoEffectiveRadius.
float EffectiveRadius(float distance)
{
    float pixelsPerMetre = uSsao.viewport.w / distance;
    return min(uSsao.params.x, uSsao.params.z / pixelsPerMetre);
}

// Render::SsaoRangeWeight.
float RangeWeight(float radius, float depthDelta)
{
    float ratio = radius / max(abs(depthDelta), radius * uSsao.params.w);
    return smoothstep(0.0, 1.0, ratio);
}

// The surface's view-space normal, from the distances of its four neighbours.
// On each axis the side whose distance differs less is taken: at a silhouette
// the other side is a different surface, and a difference taken across it
// points the normal at the background and lights the edge up.
vec3 ReconstructNormal(ivec2 pixel, vec3 center)
{
    ivec2 size = TargetSize();
    vec3 left  = ViewPosition(vec2(pixel - ivec2(1, 0)), DistanceAt(pixel - ivec2(1, 0)));
    vec3 right = ViewPosition(vec2(pixel + ivec2(1, 0)), DistanceAt(pixel + ivec2(1, 0)));
    vec3 above = ViewPosition(vec2(pixel - ivec2(0, 1)), DistanceAt(pixel - ivec2(0, 1)));
    vec3 below = ViewPosition(vec2(pixel + ivec2(0, 1)), DistanceAt(pixel + ivec2(0, 1)));

    // At the edge of the target the neighbour outside is the pixel itself, and
    // a difference of zero has no direction.
    bool useRight = pixel.x == 0 || (pixel.x < size.x - 1 && abs(right.z - center.z) < abs(center.z - left.z));
    bool useBelow = pixel.y == 0 || (pixel.y < size.y - 1 && abs(below.z - center.z) < abs(center.z - above.z));
    vec3 alongX = useRight ? right - center : center - left;
    vec3 alongY = useBelow ? below - center : center - above;
    // Rows count downward, so alongY points down the screen; this order is the
    // one that points the normal back toward the camera.
    return normalize(cross(alongY, alongX));
}

// An orthonormal basis about a unit @n with no branch on its direction (Duff et
// al.), so no normal is a special case.
void Basis(vec3 n, out vec3 tangent, out vec3 bitangent)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    tangent   = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    bitangent = vec3(b, s + n.y * n.y * a, -n.y);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float distance = DistanceAt(pixel);
    if (distance >= uSsao.viewport.z * kFarPlaneFraction)
    {
        outOcclusion = 1.0;
        return;
    }

    vec3 center = ViewPosition(vec2(pixel), distance);
    vec3 normal = ReconstructNormal(pixel, center);

    vec3 tangent;
    vec3 bitangent;
    Basis(normal, tangent, bitangent);
    // Render::SsaoTileAngle.
    uint rotation = kTileRotation[(uint(pixel.y) % kTileSize) * kTileSize + (uint(pixel.x) % kTileSize)];
    float angle = float(rotation) * kTwoPi / float(kTileRotationCount);
    float c = cos(angle);
    float s = sin(angle);
    vec3 turnedTangent   = c * tangent + s * bitangent;
    vec3 turnedBitangent = c * bitangent - s * tangent;

    float radius = EffectiveRadius(distance);
    float bias = radius * uSsao.params.w;
    ivec2 size = TargetSize();

    float occluded = 0.0;
    uint count = uSsao.counts.x;
    for (uint i = 0u; i < count; ++i)
    {
        vec3 k = uSsao.kernel[i].xyz;
        vec3 samplePoint = center + (turnedTangent * k.x + turnedBitangent * k.y + normal * k.z) * radius;
        float sampleDistance = -samplePoint.z;
        if (sampleDistance <= 0.0)
        {
            continue;
        }
        ivec2 samplePixel = ivec2(floor(PixelOf(samplePoint) + 0.5));
        // Off the target is unknown, and unknown is open: a surface at the
        // screen's edge does not darken for what the camera cannot see.
        if (any(lessThan(samplePixel, ivec2(0))) || any(greaterThanEqual(samplePixel, size)))
        {
            continue;
        }
        float sceneDistance = DistanceAt(samplePixel);
        if (sceneDistance <= sampleDistance - bias)
        {
            occluded += RangeWeight(radius, distance - sceneDistance);
        }
    }

    float open = 1.0 - occluded / float(max(count, 1u));
    outOcclusion = pow(max(open, 0.0), uSsao.params.y);
}
