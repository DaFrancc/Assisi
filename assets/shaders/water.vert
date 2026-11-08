#version 450
#extension GL_GOOGLE_include_directive : require

// Places one vertex of a water patch: on the body's grid, blended toward the
// coarser level near the patch's outer range, clamped inside a lake, and moved
// by every cascade's waves.

#include "water/surface.glsl"

// The leading members of Render::FrameConstants, all this stage reads.
layout(binding = 256) uniform FrameConstants
{
    mat4 viewProjection;
    mat4 view;
} uFrame;

layout(location = 0) out vec3  vWorldPos;
layout(location = 1) out vec3  vNormal;
// The point before the waves moved it, from the camera along world x and z:
// where the slopes are read.
layout(location = 2) out vec2  vGridPos;
layout(location = 3) out float vViewZ;
layout(location = 4) out float vPlaneDistance;
// How far above the still water the waves lifted it, in significant heights.
layout(location = 5) out float vCrest;

const uint kPatchVertices = kWaterPatchQuads + 1u;

// A cascade fades out of the geometry between these mips of it: a vertex that
// coarse cannot show its waves, and reading them anyway makes the mesh crawl.
const float kDisplacementFadeStartLod = 2.0;
const float kDisplacementFadeEndLod   = 4.0;

// Below this a significant height is still water.
const float kMinSignificantHeight = 1.0e-4;

void main()
{
    WaterNode node    = waterNodes[gl_InstanceIndex];
    uvec2     index   = uvec2(uint(gl_VertexIndex) % kPatchVertices, uint(gl_VertexIndex) / kPatchVertices);
    float     spacing = node.size / float(kWaterPatchQuads);
    vec2      local   = node.corner + vec2(index) * spacing;

    // Measured with the camera's height, as the walk that chose the patch was.
    float height   = uWater.surface.x;
    float distance = length(vec3(local.x, height, local.y));
    float range    = kWaterRangeScale * node.size;
    float morph    = clamp((distance - range * kWaterMorphStart) / (range * (1.0 - kWaterMorphStart)), 0.0, 1.0);
    // Odd vertices slide onto their even neighbours, which leaves exactly the
    // coarser level's vertices at the patch's outer range.
    local -= vec2(index & 1u) * spacing * morph;
    local = clamp(local, uWater.bounds.xy, uWater.bounds.zw);

    vec2 axisX = uWater.axes.xy;
    vec2 axisZ = vec2(-axisX.y, axisX.x);
    vec2 grid  = local.x * axisX + local.y * axisZ;

    // Each cascade at the mip whose texels match the vertex spacing, so a
    // coarse vertex takes the waves averaged over its patch, not one point.
    float vertexSpacing = spacing * (1.0 + morph);
    vec3  displacement  = vec3(0.0);
    for (uint cascade = 0u; cascade < kSurfaceCascadeCount; ++cascade)
    {
        float lod    = max(log2(vertexSpacing * float(kSurfaceFftSize) / uWater.tileLength[cascade]), 0.0);
        float weight = 1.0 - smoothstep(kDisplacementFadeStartLod, kDisplacementFadeEndLod, lod);
        if (weight > 0.0)
        {
            vec3 uv = vec3(WaterUv(grid, cascade), float(cascade));
            displacement += weight * textureLod(sampler2DArray(uDisplacement, uWaterSampler), uv, lod).xyz;
        }
    }

    vec3 position = vec3(grid.x + displacement.x, height + displacement.y, grid.y + displacement.z);
    vWorldPos      = position;
    vNormal        = vec3(0.0, 1.0, 0.0);
    vGridPos       = grid;
    vViewZ         = (uFrame.view * vec4(position, 1.0)).z;
    vPlaneDistance = position.y;
    vCrest         = displacement.y / max(uWater.surface.y, kMinSignificantHeight);
    gl_Position    = uFrame.viewProjection * vec4(position, 1.0);
}
