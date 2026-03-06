#version 460 core

in vec3 vFragPos;
in vec3 vNormal;
in vec2 vTexCoords;

out vec4 FragColor;

struct DirLight
{
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

uniform sampler2D uDiffuse;
uniform float     uShininess;
uniform vec3      uViewPos;
uniform DirLight  uDirLight;

void main()
{
    vec3 norm     = normalize(vNormal);
    vec3 viewDir  = normalize(uViewPos - vFragPos);
    vec3 lightDir = normalize(-uDirLight.direction);
    vec3 texColor = vec3(texture(uDiffuse, vTexCoords));

    // Ambient
    vec3 ambient = uDirLight.ambient * texColor;

    // Diffuse
    float diff   = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = uDirLight.diffuse * diff * texColor;

    // Specular
    vec3  reflectDir = reflect(-lightDir, norm);
    float spec       = pow(max(dot(viewDir, reflectDir), 0.0), uShininess);
    vec3  specular   = uDirLight.specular * spec;

    FragColor = vec4(ambient + diffuse + specular, 1.0);
}