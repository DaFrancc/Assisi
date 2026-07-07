#version 460 core

in vec3  vFragPos;
in vec2  vTexCoords;
in mat3  vTBN;
in float vViewZ; // view-space Z (negative for geometry in front of camera)

out vec4 FragColor;

// ---- Light structs (must match ClusterGrid GPU structs exactly) ------------

struct PointLight
{
    vec4 positionRadius; // xyz = world pos, w = radius
    vec4 colorIntensity; // xyz = colour,    w = intensity
};

struct SpotLight
{
    vec4  positionRadius; // xyz = world pos,       w = radius
    vec4  directionInner; // xyz = direction (unit), w = cos(innerAngle)
    vec4  colorIntensity; // xyz = colour,           w = intensity
    float outerCutoff;
    float _p0, _p1, _p2;
};

struct DirLight
{
    vec4 directionIntensity; // xyz = direction toward light, w = intensity
    vec4 colorPad;           // xyz = colour, w = unused
};

struct LightGrid
{
    uint pointOffset;
    uint pointCount;
    uint spotOffset;
    uint spotCount;
};

// ---- SSBOs (binding points match ClusterGrid constants) --------------------

layout(std430, binding = 1) readonly buffer PointLights    { PointLight  pointLights[];    };
layout(std430, binding = 2) readonly buffer SpotLights     { SpotLight   spotLights[];     };
layout(std430, binding = 3) readonly buffer DirLights      { DirLight    dirLights[];      };
layout(std430, binding = 4) readonly buffer LightIndexList { uint        lightIndexList[];  };
layout(std430, binding = 5) readonly buffer LightGrids     { LightGrid   lightGrids[];     };

// ---- Uniforms --------------------------------------------------------------

uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMetallic;
uniform sampler2D uRoughness;

uniform vec3 uViewPos;
uniform vec3 uAmbient;
uniform int  uDirLightCount;

// Cluster grid parameters (set by LightingSystem::SetupMeshShader)
uniform uvec3 uGridDim;
uniform vec2  uScreenSize;
uniform float uNearZ;
uniform float uFarZ;

// ---- PBR helpers -----------------------------------------------------------

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness * roughness;
    float a2     = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- Cook-Torrance BRDF ---------------------------------------------------

vec3 CookTorrance(vec3 N, vec3 V, vec3 L, vec3 radiance,
                  vec3 albedo, float metallic, float roughness, vec3 F0)
{
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL == 0.0)
        return vec3(0.0);

    vec3  H        = normalize(V + L);
    float NDF      = DistributionGGX(N, H, roughness);
    float G        = GeometrySmith(N, V, L, roughness);
    vec3  F        = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3  specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
    vec3  kD       = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

// ---- Windowed inverse-square attenuation (Frostbite) ----------------------
// Returns 0 at dist >= radius, physically plausible inside.

float Attenuation(float dist, float radius)
{
    float ratio  = dist / radius;
    float ratio4 = ratio * ratio * ratio * ratio;
    float numer  = max(1.0 - ratio4, 0.0);
    return (numer * numer) / (dist * dist + 1.0);
}

// ---- Cluster index ---------------------------------------------------------

uint ClusterIndex()
{
    uint ix = clamp(uint(gl_FragCoord.x / uScreenSize.x * float(uGridDim.x)),
                    0u, uGridDim.x - 1u);
    uint iy = clamp(uint(gl_FragCoord.y / uScreenSize.y * float(uGridDim.y)),
                    0u, uGridDim.y - 1u);

    // Logarithmic depth slice (abs because vViewZ is negative for visible geometry)
    float viewDepth = abs(vViewZ);
    uint  iz = clamp(uint(log(viewDepth / uNearZ) / log(uFarZ / uNearZ) * float(uGridDim.z)),
                     0u, uGridDim.z - 1u);

    return ix + iy * uGridDim.x + iz * uGridDim.x * uGridDim.y;
}

// ---- Main ------------------------------------------------------------------

void main()
{
    // Sample material textures
    vec3  albedo    = pow(texture(uAlbedo,    vTexCoords).rgb, vec3(2.2)); // sRGB → linear
    float metallic  = texture(uMetallic,  vTexCoords).r;
    float roughness = texture(uRoughness, vTexCoords).r;

    // World-space normal from normal map
    vec3 normalTs = texture(uNormal, vTexCoords).rgb * 2.0 - 1.0;
    vec3 N = normalize(vTBN * normalTs);
    vec3 V = normalize(uViewPos - vFragPos);

    // Base reflectance: 0.04 for dielectrics, albedo tint for metals
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // ---- Directional lights (no position, no falloff) ----------------------
    for (int i = 0; i < uDirLightCount; i++)
    {
        vec3 L        = normalize(-dirLights[i].directionIntensity.xyz);
        vec3 radiance = dirLights[i].colorPad.xyz * dirLights[i].directionIntensity.w;
        Lo += CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // ---- Clustered point + spot lights ------------------------------------
    uint clusterIdx  = ClusterIndex();
    uint pointOffset = lightGrids[clusterIdx].pointOffset;
    uint pointCount  = lightGrids[clusterIdx].pointCount;
    uint spotOffset  = lightGrids[clusterIdx].spotOffset;
    uint spotCount   = lightGrids[clusterIdx].spotCount;

    for (uint i = 0u; i < pointCount; i++)
    {
        uint  li       = lightIndexList[pointOffset + i];
        vec3  lPos     = pointLights[li].positionRadius.xyz;
        float r        = pointLights[li].positionRadius.w;
        vec3  lCol     = pointLights[li].colorIntensity.xyz;
        float lInt     = pointLights[li].colorIntensity.w;

        vec3  toLight  = lPos - vFragPos;
        float dist     = length(toLight);
        vec3  L        = toLight / dist;
        float att      = Attenuation(dist, r);
        vec3  radiance = lCol * lInt * att;

        Lo += CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // Spot light indices are offset by MAX_LIGHT_INDICES in the shared buffer
    // (must match cluster_cull.comp #define MAX_LIGHT_INDICES 65536)
    const uint kSpotBase = 65536u;
    for (uint i = 0u; i < spotCount; i++)
    {
        uint  li       = lightIndexList[kSpotBase + spotOffset + i];
        vec3  lPos     = spotLights[li].positionRadius.xyz;
        float r        = spotLights[li].positionRadius.w;
        vec3  lDir     = spotLights[li].directionInner.xyz;  // unit, world space
        float inner    = spotLights[li].directionInner.w;    // cos(innerAngle)
        vec3  lCol     = spotLights[li].colorIntensity.xyz;
        float lInt     = spotLights[li].colorIntensity.w;
        float outer    = spotLights[li].outerCutoff;

        vec3  toLight  = lPos - vFragPos;
        float dist     = length(toLight);
        vec3  L        = toLight / dist;

        // Cone attenuation: 1 inside inner cone, smooth falloff to outer, 0 outside
        float theta    = dot(L, normalize(-lDir));
        float cone     = smoothstep(outer, inner, theta);

        float att      = Attenuation(dist, r) * cone;
        vec3  radiance = lCol * lInt * att;

        Lo += CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
    }

    // ---- Flat ambient + tone map ------------------------------------------
    vec3 color = uAmbient * albedo + Lo;

    // Reinhard tone map + gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}