#version 450

// One level of a texture camera's picture (see Render::PictureLevels). The
// first is stretched from the part of a target the camera drew; each smaller
// one is half the size of the level above, its pixels one bilinear tap at the
// corner four pixels above share, which is their average. Where a side of the
// level above is odd, the taps fall between corners and that side's last row
// weighs a little less: a small blur no one sees on a level a surface shows
// only from far away. The first level is also darkened toward its edge by the
// camera's vignette, which the smaller ones then carry; theirs has strength 0.

layout(location = 0) in  vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// What is read: the drawn target or the level above, alone. Texture_SRV and
// Sampler are offset by VulkanBindingOffsets: shaderResource at +0, sampler
// at +128.
layout(binding = 0)   uniform texture2D uAbove;
layout(binding = 128) uniform sampler   uAboveSampler;

// Render::PictureLevels::Constants.
layout(push_constant) uniform PushConstants
{
    vec2  scale;    // the part read, as a fraction of the texture
    vec2  most;     // the furthest coordinate read, inside that part
    vec2  centre;   // the vignette's clear middle, 0 to 1 from the top-left
    float radius;   // how far it stays clear, in level heights
    float softness; // how wide the fade past the radius is, never zero
    float strength; // how dark beyond the fade: 0 none, 1 black
    float aspect;   // the level's width over its height
} pc;

void main()
{
    vec2 uv = min(vTexCoords * pc.scale, pc.most);
    vec4 colour = textureLod(sampler2D(uAbove, uAboveSampler), uv, 0.0);

    // The same for every pixel of a draw, so a picture without a vignette, and
    // every level but the first, skips it whole.
    if (pc.strength > 0.0)
    {
        // Measured in heights, so the clear part is a circle on a wide picture.
        vec2 fromCentre = (vTexCoords - pc.centre) * vec2(pc.aspect, 1.0);
        float fade = smoothstep(0.0, 1.0, (length(fromCentre) - pc.radius) / pc.softness);
        colour.rgb *= 1.0 - pc.strength * fade;
    }
    outColor = colour;
}
