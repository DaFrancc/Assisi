#version 460 core

in vec3 vFragPos;
in vec2 vTexCoords;
in mat3 vTBN;

out vec4 FragColor;

struct DirLight
{
    vec3  direction;
    vec3  color;
    float intensity;
};

uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMetallic;
uniform sampler2D uRoughness;

uniform vec3     uViewPos;
uniform vec3     uAmbient;
uniform DirLight uDirLight;

const float PI = 3.14159265359;

// GGX / Trowbridge-Reitz normal distribution function
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness * roughness;
    float a2     = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX geometry term (single direction)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's combined geometry term
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// Schlick Fresnel approximation
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    // Sample material textures
    vec3  albedo    = pow(texture(uAlbedo, vTexCoords).rgb, vec3(2.2)); // sRGB -> linear
    float metallic  = texture(uMetallic, vTexCoords).r;
    float roughness = texture(uRoughness, vTexCoords).r;

    // Reconstruct world-space normal from normal map
    vec3 normalTs = texture(uNormal, vTexCoords).rgb * 2.0 - 1.0;
    vec3 N        = normalize(vTBN * normalTs);
    vec3 V        = normalize(uViewPos - vFragPos);

    // Base reflectance: 0.04 for dielectrics, albedo tint for metals
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // --- Directional light contribution ---
    vec3  L        = normalize(-uDirLight.direction);
    vec3  H        = normalize(V + L);
    vec3  radiance = uDirLight.color * uDirLight.intensity;
    float NdotL    = max(dot(N, L), 0.0);

    float NDF = DistributionGGX(N, H, roughness);
    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
    vec3 kD       = (1.0 - F) * (1.0 - metallic);
    vec3 Lo       = (kD * albedo / PI + specular) * radiance * NdotL;

    // Flat ambient (no IBL yet)
    vec3 ambient = uAmbient * albedo;

    vec3 color = ambient + Lo;

    // Reinhard tone map + gamma correction (no HDR buffer yet)
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}