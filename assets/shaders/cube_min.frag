#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 0) out vec4 outColor;

void main()
{
    const vec3 lightDir = normalize(vec3(0.4, 0.8, 0.5));
    const float ambient = 0.15;
    const float diffuse = max(dot(normalize(vNormal), lightDir), 0.0);
    const vec3 baseColor = vec3(0.85, 0.35, 0.2);

    outColor = vec4(baseColor * (ambient + diffuse), 1.0);
}
