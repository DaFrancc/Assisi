// mesh.frag's inputs, as mesh.vert writes them, and its output.

layout(location = 0) in vec3  vWorldPos;
layout(location = 1) in vec3  vNormal;
layout(location = 2) in vec2  vTexCoord;
layout(location = 3) in float vViewZ;
layout(location = 4) in vec3  vTangent;
layout(location = 5) in float vTangentSign;
layout(location = 6) in flat uint vMaterialIndex; // row into the material table
// What SmoothSurfaceLift measures the curved surface from; mesh.vert says what
// it holds.
layout(location = 7) in float vPlaneDistance;

layout(location = 0) out vec4 outColor;
