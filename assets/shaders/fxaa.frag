#version 460 core

// FXAA 3.11 — simplified implementation.
// Based on Timothy Lottes' FXAA technique.

in  vec2 vTexCoords;
out vec4 FragColor;

uniform sampler2D uScreenTexture;
uniform vec2      uTexelSize; ///< 1.0 / resolution

// Tuning constants
const float EDGE_THRESHOLD_MIN = 0.0312; // skip very dark edges
const float EDGE_THRESHOLD_MAX = 0.125;  // minimum contrast to trigger AA
const float SUBPIXEL_QUALITY   = 0.75;   // subpixel aliasing reduction strength
const int   SEARCH_STEPS       = 12;     // maximum edge-walk iterations

float Luma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec3  colorCenter = texture(uScreenTexture, vTexCoords).rgb;
    float lumaCenter  = Luma(colorCenter);

    // Cardinal neighbours
    float lumaN = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2( 0,  1)).rgb);
    float lumaS = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2( 0, -1)).rgb);
    float lumaE = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2( 1,  0)).rgb);
    float lumaW = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2(-1,  0)).rgb);

    float lumaMin   = min(lumaCenter, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax   = max(lumaCenter, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float lumaRange = lumaMax - lumaMin;

    // Early-out: not on an edge
    if (lumaRange < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX))
    {
        FragColor = vec4(colorCenter, 1.0);
        return;
    }

    // Diagonal neighbours
    float lumaNW = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2(-1,  1)).rgb);
    float lumaNE = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2( 1,  1)).rgb);
    float lumaSW = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2(-1, -1)).rgb);
    float lumaSE = Luma(textureOffset(uScreenTexture, vTexCoords, ivec2( 1, -1)).rgb);

    // Subpixel blend factor (handles thin-feature aliasing)
    float lumaSum     = 2.0 * (lumaN + lumaS + lumaE + lumaW) + lumaNW + lumaNE + lumaSW + lumaSE;
    float subpixBlend = clamp(abs(lumaSum / 12.0 - lumaCenter) / lumaRange, 0.0, 1.0);
    subpixBlend       = smoothstep(0.0, 1.0, subpixBlend);
    subpixBlend       = subpixBlend * subpixBlend * SUBPIXEL_QUALITY;

    // Edge orientation (horizontal vs vertical)
    float edgeH = abs(-2.0 * lumaW  + lumaNW + lumaSW)
                + abs(-2.0 * lumaCenter + lumaN  + lumaS) * 2.0
                + abs(-2.0 * lumaE  + lumaNE + lumaSE);
    float edgeV = abs(-2.0 * lumaN  + lumaNW + lumaNE)
                + abs(-2.0 * lumaCenter + lumaW  + lumaE) * 2.0
                + abs(-2.0 * lumaS  + lumaSW + lumaSE);
    bool isHoriz = (edgeH >= edgeV);

    // Step direction perpendicular to the edge
    float stepLen = isHoriz ? uTexelSize.y : uTexelSize.x;
    float lumaPos = isHoriz ? lumaN : lumaE;
    float lumaNeg = isHoriz ? lumaS : lumaW;

    float gradPos     = abs(lumaPos - lumaCenter);
    float gradNeg     = abs(lumaNeg - lumaCenter);
    bool  posIsSteeper = (gradPos >= gradNeg);

    float gradScaled   = 0.25 * max(gradPos, gradNeg);
    float lumaLocalAvg = posIsSteeper ? 0.5 * (lumaPos + lumaCenter)
                                      : 0.5 * (lumaNeg + lumaCenter);
    if (!posIsSteeper) stepLen = -stepLen;

    // Start at the pixel centre, shifted half a step perpendicular to the edge
    vec2 uv = vTexCoords;
    if (isHoriz) uv.y += stepLen * 0.5;
    else         uv.x += stepLen * 0.5;

    // Walk along the edge in both directions
    const float QUALITY[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    vec2  step  = isHoriz ? vec2(uTexelSize.x, 0.0) : vec2(0.0, uTexelSize.y);
    vec2  uv1   = uv - step;
    vec2  uv2   = uv + step;
    float end1  = Luma(texture(uScreenTexture, uv1).rgb) - lumaLocalAvg;
    float end2  = Luma(texture(uScreenTexture, uv2).rgb) - lumaLocalAvg;
    bool  done1 = abs(end1) >= gradScaled;
    bool  done2 = abs(end2) >= gradScaled;

    for (int i = 0; i < SEARCH_STEPS && !(done1 && done2); ++i)
    {
        if (!done1) { uv1  -= step * QUALITY[i]; end1 = Luma(texture(uScreenTexture, uv1).rgb) - lumaLocalAvg; done1 = abs(end1) >= gradScaled; }
        if (!done2) { uv2  += step * QUALITY[i]; end2 = Luma(texture(uScreenTexture, uv2).rgb) - lumaLocalAvg; done2 = abs(end2) >= gradScaled; }
    }

    // Pixel blend offset based on position along the edge
    float dist1    = isHoriz ? (vTexCoords.x - uv1.x) : (vTexCoords.y - uv1.y);
    float dist2    = isHoriz ? (uv2.x - vTexCoords.x) : (uv2.y - vTexCoords.y);
    float distFull = dist1 + dist2;
    float pixOffset = -min(dist1, dist2) / distFull + 0.5;

    // Only apply edge offset when luma variation is consistent with the edge direction
    bool isLumaCenterSmaller = lumaCenter < lumaLocalAvg;
    bool correctVariation    = ((dist1 < dist2 ? end1 : end2) < 0.0) != isLumaCenterSmaller;
    float finalOffset        = max(correctVariation ? pixOffset : 0.0, subpixBlend);

    vec2 finalUV = vTexCoords;
    if (isHoriz) finalUV.y += finalOffset * stepLen;
    else         finalUV.x += finalOffset * stepLen;

    FragColor = vec4(texture(uScreenTexture, finalUV).rgb, 1.0);
}
