// The clouds' shadow on a surface: how much of the directional light gets
// through the clouds above it, read from the map cloud_shadow.comp draws.

// Mirrors Render::CloudShadowConstants; the member order is the layout.
struct CloudShadowConstants
{
    vec4 area;    // xy = the map's centre across, relative to the camera; z = 1 / its width in metres; w = 1 while clouds cast
    vec4 light;   // xyz = unit direction the shadow is cast along, toward the light; w = the clouds' base height
    vec4 outside; // x = the light let through past the map's edge, y = the camera's height
};

// Must match Render::MeshPass's cloud shadow slot.
layout(binding = 16) uniform texture2D uCloudShadow;

// Over this outer share of the map's half-width the shadow fades to its
// average, so the map's edge never shows as a line across the land.
const float kCloudShadowEdgeFade = 0.2;

// `toSurface` is the surface's offset from the camera. Follows the light up
// from the surface to the clouds' base and reads the map there, through
// `clampSampler`: it is passed in because this file comes before the frame
// constants, which hold the constants, and the sampler is declared after them.
float CloudShadow(CloudShadowConstants c, vec3 toSurface, sampler clampSampler)
{
    if (c.area.w == 0.0)
    {
        return 1.0;
    }
    float height = c.outside.y + toSurface.y;
    float rise   = max(c.light.w - height, 0.0);
    vec2  atBase = toSurface.xz + c.light.xz * (rise / c.light.y);
    vec2  uv     = (atBase - c.area.xy) * c.area.z + 0.5;
    float inside = texture(sampler2D(uCloudShadow, clampSampler), uv).r;
    vec2  fromMiddle = abs(uv - 0.5) * 2.0;
    float fade = smoothstep(1.0 - kCloudShadowEdgeFade, 1.0, max(fromMiddle.x, fromMiddle.y));
    return mix(inside, c.outside.x, fade);
}
