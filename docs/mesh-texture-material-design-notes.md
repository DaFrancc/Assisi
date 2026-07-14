# Mesh / texture / material system — design notes

Captured 2026-07-14. **Geometry loading is built; materials/textures are not.**
This records where the mesh→texture→material pipeline should go, so the current
"glTF loads geometry only" state isn't mistaken for the finished design, and so
the deferred material work is picked up with the reasoning intact rather than
re-derived.

## What is built (2026-07-14)

glTF/glb **geometry** loading, shipped but uncommitted on `dev`:

- **Geometry module** — sits below Render, above Core+Math, no GPU dependency.
  Owns `MeshData`, `DefaultMeshes` (moved out of Render), and `MeshImporter`.
- `Geometry::ImportMesh(virtualPath)` → `std::expected<MeshData, MeshImportError>`,
  extension-routed: `.gltf`/`.glb` → fastgltf v0.9.0; anything else →
  `UnsupportedFormat` (the slot a future Assimp catch-all backend plugs into).
- **All reads go through AssetSystem.** `LoadExternalBuffers` is deliberately OFF;
  external `.bin` buffers are resolved by hand via `AssetSystem::ReadBinary`, so the
  asset-root escape protection is never bypassed for glTF's sibling files.
- v1 **merges every primitive into one mesh**, baking node world transforms;
  tangents are regenerated (`ComputeTangents`) when a primitive lacks them.
- `AssetCache::ResolveMesh` loads mesh files (cached on success; a failed load is
  attempted once, warned, and thereafter falls back to `prim://cube`).
- The sandbox asset browser lists `.glb`/`.gltf` as cube tiles.
- Assimp is gated behind `option(ASSISI_ENABLE_ASSIMP OFF)` — one flag revives it.

## The gap

`ImportMesh` returns **geometry only**. A glTF file also carries **materials**
(base-color factor + base-color texture, plus normal/metallic-roughness/emissive
that the engine doesn't wire yet). Nothing extracts them, so a loaded model draws
with whatever `MeshRenderer.albedoPath` happens to hold — a leftover texture, or
the flat-white fallback (`DefaultResources::WhiteTexture`) when empty. That is why
a textured model shows the wrong texture: the material is simply ignored.

## Two orthogonal decisions

### 1. Texture packaging — how the image bytes reach the engine

**Leaning: unpack `.glb` into separate files** rather than decode embedded images
in-engine. A `.glb` bundles mesh + buffers + textures into one binary; `.gltf` is
the same data as JSON referencing external `.bin` and image files. Unpacking:

    gltf-pipeline -i model.glb -o model.gltf --separate

(also: glTF-Transform `unpack`, or Blender export as "glTF Separate (.gltf + .bin +
textures)") yields `model.gltf` + `model.bin` + `model_*.png`.

Why this route fits the engine:

- The importer already loads `.gltf` with external buffers through AssetSystem;
  separate textures are just more external files resolved the same way.
- Unpacked textures become **normal engine assets** — a `.png` in `assets/` that the
  existing texture pipeline (`ResolveTexture`, browser thumbnails, sRGB handling)
  already handles, and that stays inspectable/swappable. Matches the user's stated
  preference for separate files over monolithic `.glb`.
- The remaining engine change is small (see below): no embedded-image decoding.

Alternative (more code, only if we ever want embedded `.glb` to self-texture):
decode embedded images in-engine via a new `Texture::LoadFromMemory(bytes)`, pulling
image bytes out of the glTF binary chunk / data URIs.

Cost of the unpack route: a per-asset pre-process step (one CLI command or a Blender
export; scriptable). Optionally an in-engine "Import .glb" action could run the
unpack later, but that is essentially embedding an exporter — leave it manual for now.

### 2. Single vs. multi-material — how many textures a mesh binds

**Orthogonal to packaging, and the harder half.** Materials are *per-primitive*, but
v1 merges all primitives into one mesh, and one merged mesh binds **one** albedo per
draw. So:

- **Single-material model** → merged mesh + its one base-color texture is correct.
  The small wire-up below fully textures it.
- **Multi-material model** → needs **submeshes**: keep each primitive as its own
  sub-range with its own material, drawn as separate calls. This reshapes
  `MeshBuffer` (multi-submesh), the render loop, and how a `MeshRenderer` holds
  materials. Call this **Level 2**. Packaging does not help here — a multi-material
  model needs submeshes no matter how the files are packed.

**Open question to resolve first:** is the current test model single- or
multi-material? That determines whether the small path below is sufficient or Level 2
is required now.

## The small wire-up (sufficient for single-material + unpacked textures)

Once textures are external files with real virtual paths, the engine change is minor:

1. `ImportMesh` reads `material.pbrMetallicRoughness.baseColorTexture` → its external
   image URI → returns it as a **virtual asset path** (resolved relative to the
   `.gltf`, through AssetSystem's rules), alongside the `MeshData`.
2. `AssetCache::ResolveMesh` surfaces that path; albedo resolution falls back to it:
   `albedoTexture = albedoPath set ? ResolveTexture(albedoPath) : ResolveTexture(mesh's baseColor path)`.

No `Texture::LoadFromMemory`, no embedded decode, no synthetic paths, no component or
serialization change. A user-set `albedoPath` still wins; an empty one uses the
glTF's own texture instead of white. This reuses the existing `ResolveTexture`
pipeline end to end.

## Sequencing

    determine test model's material count
        ├─ single-material → unpack to separate files + small albedo wire-up  (done-ish)
        └─ multi-material  → Level 2: submeshes + per-primitive materials      (bigger)

    normal / metallic-roughness / emissive maps → later; engine doesn't wire those
    channels yet (see DefaultResources / nvrhi-migration-todo.md).

## Related

- `docs/asset-streaming-design-notes.md` — async load / streaming; a real mesh loader
  was named there as the trigger to finally use the fetched Assimp. Now fastgltf fills
  that role; Assimp stays the deferred catch-all.
- USD / tinyusdz remains the planned path for composed **environments** (not single
  models), with the reflection-driven JSON staying the native save format.
