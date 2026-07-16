#version 450

// Selection-outline mask pass, fragment stage. Writes flat coverage (1.0) wherever
// the selected mesh projects, into a single-channel R8 mask target. Depth testing
// is disabled in the pipeline, so the whole silhouette is covered even where the
// object is occluded — that is what makes the outline show through walls.

layout(location = 0) out vec4 outMask;

void main()
{
    outMask = vec4(1.0);
}
