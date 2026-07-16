#version 450

// Editor entity-icon billboard, vertex stage. Draws one camera-facing quad per
// entity that has a Transform but no mesh, at a fixed WORLD size (so perspective
// and distance shrink it on screen like any world object). No vertex buffer: the
// six corners come from gl_VertexIndex, offset from the entity's world centre
// along the camera's right/up axes (passed in already scaled to the half-size).

layout(push_constant) uniform PushConstants
{
    mat4 viewProjection;
    vec4 center;    // xyz = entity world position
    vec4 rightHalf; // xyz = camera right * half world size
    vec4 upHalf;    // xyz = camera up    * half world size
} pc;

layout(location = 0) out vec2 vTexCoords;

// Two triangles forming a quad; corner is in [-1,1] on each axis.
const vec2 kCorners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
    vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
);

void main()
{
    const vec2 corner   = kCorners[gl_VertexIndex];
    const vec3 worldPos = pc.center.xyz + corner.x * pc.rightHalf.xyz + corner.y * pc.upHalf.xyz;
    gl_Position         = pc.viewProjection * vec4(worldPos, 1.0);

    // Map corner [-1,1] to UV [0,1]; flip V so the image is upright (V=0 at the
    // top, matching how Texture::LoadFromAssets uploads image rows).
    vTexCoords = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
}
