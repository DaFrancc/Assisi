#version 450

// The sky as a fullscreen triangle pinned to the far plane. No vertex buffer and
// no index buffer: draw three vertices with nothing bound.

layout(binding = 256) uniform SkyConstants
{
    mat4 invViewProjection;
    vec4 cameraPosition;  // xyz = world-space eye, w unused
    vec4 sunDirection;    // xyz = unit direction TO the sun, w = cos of the disk's outer edge
    vec4 sunRadiance;     // xyz = colour * intensity, w = cos of the disk's inner edge
    vec4 groundColor;     // xyz = linear colour, w = zenith optical depth
    vec4 nightColor;      // xyz = linear colour, w = haze
    vec4 params;          // x = intensity, y = sun disk intensity, zw unused
} uSky;

// A point along the pixel's ray, left unprojected. It is an affine function of
// the NDC position, so interpolating it is exact — the perspective divide is
// nonlinear and has to happen per fragment instead.
layout(location = 0) out vec4 vRayPoint;

// The scene's far-plane depth, Render::kSceneDepthFar: reversed Z stores the far
// plane at zero.
const float kFarDepth = 0.0;

void main()
{
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0),(2,0),(0,2)
    vec2 ndc = pos * 2.0 - 1.0;

    // Every fragment at the far-plane depth — what the depth clear left and what
    // the pipeline's Equal test admits, so the sky shades exactly the pixels no
    // geometry wrote and none of the ones it did.
    gl_Position = vec4(ndc, kFarDepth, 1.0);

    // Inverting the same matrix the mesh pass drew with is what puts the sky
    // behind the geometry rather than beside it: the viewport's Y flip and the
    // projection are both undone by construction, with nothing here naming
    // either. Only the ray's direction is used, so any depth on it will do: the
    // far plane for the scene, wherever that depth lands for a probe face.
    vRayPoint = uSky.invViewProjection * vec4(ndc, kFarDepth, 1.0);
}
