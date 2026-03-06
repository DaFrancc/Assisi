#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoords;
layout(location = 3) in vec4 aTangent; // xyz = tangent, w = bitangent sign

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vFragPos;
out vec2 vTexCoords;
out mat3 vTBN;

void main()
{
    vFragPos   = vec3(uModel * vec4(aPosition, 1.0));
    vTexCoords = aTexCoords;

    mat3 normalMatrix = mat3(transpose(inverse(uModel)));
    vec3 N = normalize(normalMatrix * aNormal);
    vec3 T = normalize(normalMatrix * aTangent.xyz);
    T      = normalize(T - dot(T, N) * N); // re-orthogonalize against N
    vec3 B = cross(N, T) * aTangent.w;
    vTBN   = mat3(T, B, N);

    gl_Position = uProjection * uView * vec4(vFragPos, 1.0);
}