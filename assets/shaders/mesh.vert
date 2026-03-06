#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vFragPos;
out vec3 vNormal;
out vec2 vTexCoords;

void main()
{
    vFragPos    = vec3(uModel * vec4(aPosition, 1.0));
    vNormal     = mat3(transpose(inverse(uModel))) * aNormal;
    vTexCoords  = aTexCoords;
    gl_Position = uProjection * uView * vec4(vFragPos, 1.0);
}