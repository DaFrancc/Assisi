// mesh.frag's inputs: varyings, per-frame constants, clustered lights and the
// shadow view table.

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vTexCoord;
layout(location = 3) in float vViewZ;
layout(location = 4) in vec3  vTangent;
layout(location = 5) in float vTangentSign;
layout(location = 6) in flat uint vMaterialIndex; // row into the material table

layout(location = 0) out vec4 outColor;

// Mirrors Render::FrameConstants and mesh.vert's copy; the member order is the
// layout.
layout(binding = 256) uniform FrameConstants
{
    mat4  viewProjection;
    mat4  view;
    uvec4 gridDim;            // xyz used
    vec4  screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    uvec4 lightCounts;        // x = directional light count, y = material debug view
    vec4  cameraPosition;     // world-space camera position
    // Froxel lookup: xy = gridDim.xy / screenSize, z = gridDim.z / log(farZ/nearZ),
    // w = -z * log(nearZ).
    vec4  clusterScale;
    vec4  indirectSky;        // rgb = radiance onto a surface facing straight up
    vec4  indirectGround;     // rgb = radiance onto a surface facing straight down
    // x = 1 while a prefiltered environment answers specular, y = its last mip,
    // z = 1 while screen-space occlusion ran this frame.
    vec4  indirectSpecular;
    // x = cascade count (0 = no sun shadows), y = shadowed directional light,
    // z = ShadowFilter, w = ShadowDebugView.
    uvec4 shadowCounts;
    // x = local ShadowFilter, y = 1 while any local light holds a tile, z = 1
    // while local lookups take the contact-hardening path.
    uvec4 localShadowCounts;
    // x = UV step between PCF taps, y = cascade blend-band fraction, z = penumbra
    // cap over the filter radius, w = sun penumbra per unit of blocker distance.
    vec4  shadowParams;
    // The sun's contact hardening: x = penumbra UV per unit depth (0 = off),
    // y = reach cap in UV, z = one texel of depth, w = world-space penumbra cap.
    vec4  shadowPcss;
    // Per cascade: x = view distance it ends at, y = depth bias, z = normal
    // offset in world units, w = depth range in world units.
    vec4  shadowCascade[8];
    mat4  shadowViewProjection[8];
} uFrame;

// Must match Render::MaterialDebugView.
const uint kDebugNone      = 0u;
const uint kDebugBaseColor = 1u;
const uint kDebugMetallic  = 2u;
const uint kDebugRoughness = 3u;
const uint kDebugNormal    = 4u;
const uint kDebugOcclusion = 5u;
const uint kDebugEmissive  = 6u;
const uint kDebugScreenOcclusion = 7u;

// ---- Clustered lights (mirror Render::ClusterGrid's GPU structs) ------------

// shadowView is the light's first row in the shadow view table, or
// kNoShadowView when it holds no atlas tile. A point light's six faces are that
// row and the five after it.
struct PointLight
{
    vec4  positionRadius;
    vec4  colorIntensity;
    uvec4 shadowView;
};

struct SpotLight
{
    vec4  positionRadius;
    vec4  directionInner;
    vec4  colorIntensity;
    float outerCutoff;
    uint  shadowView;
    float _p0, _p1;
};

// Must match Render::kNoShadowView.
const uint kNoShadowView = 0xFFFFFFFFu;

struct DirLight { vec4 directionIntensity; vec4 colorPad; };

struct LightGrid
{
    uint pointOffset;
    uint pointCount;
    uint spotOffset;
    uint spotCount;
};

layout(std430, binding = 1) readonly buffer PointLights    { PointLight pointLights[];   };
layout(std430, binding = 2) readonly buffer SpotLights     { SpotLight  spotLights[];    };
layout(std430, binding = 3) readonly buffer DirLights      { DirLight   dirLights[];     };
layout(std430, binding = 4) readonly buffer LightIndexList { uint       lightIndexList[]; };
layout(std430, binding = 5) readonly buffer LightGrids     { LightGrid  lightGrids[];    };

// Must match Render::ClusterGrid::kMaxLightIndices and cluster_cull.comp's MAX_LIGHT_INDICES.
const uint kSpotIndexBase = 262144u;

// ---- Shadow view table (mirrors Render::ShadowViewGpu) --------------------
// One row per local-light shadow view; the sun's cascades are in uFrame.
struct ShadowViewRow
{
    mat4 viewProjection;
    vec4 uvScaleOffset; // xy = scale, zw = offset: the tile's rectangle of the atlas
    vec4 params;        // x = depth bias, y = normal offset, z = tap step, w = array slice
    vec4 clampUv;       // xy = min, zw = max UV a lookup may reach
    // x = penumbra UV per unit depth, y = texel of depth x distance, z = reach
    // cap, w unused
    vec4 pcss;
};

layout(std430, binding = 8) readonly buffer ShadowViews { ShadowViewRow shadowViews[]; };

const float PI = 3.14159265359;

// The froxel this fragment falls in.
uint ClusterIndex()
{
    uvec3 gridDim = uFrame.gridDim.xyz;
    uint  ix      = clamp(uint(gl_FragCoord.x * uFrame.clusterScale.x), 0u, gridDim.x - 1u);
    uint  iy      = clamp(uint(gl_FragCoord.y * uFrame.clusterScale.y), 0u, gridDim.y - 1u);

    // Logarithmic depth slice. Clamped before the cast: a fragment nearer than
    // nearZ gives a negative slice, and a negative float to uint is undefined.
    float slice = max(log(abs(vViewZ)) * uFrame.clusterScale.z + uFrame.clusterScale.w, 0.0);
    uint  iz    = clamp(uint(slice), 0u, gridDim.z - 1u);

    return ix + iy * gridDim.x + iz * gridDim.x * gridDim.y;
}
