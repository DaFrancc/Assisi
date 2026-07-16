# Mesh & material system — architecture

Captured 2026-07-14. **This is the authoritative design for the mesh → material →
draw-submission pipeline.** It supersedes the "two orthogonal decisions" in
`old/mesh-texture-material-design-notes.md` (both are resolved here) and defines the
foundation that the GPU-driven roadmap (`gpu-driven-rendering-design-notes.md`,
stages 1–6) and the streaming roadmap (`asset-streaming-design-notes.md`) build
on. Design reviewed by two independent adversarial review passes; their findings
(hot-reload contract, sort-key depth bits, default-deny codegen, per-submesh cull
removal, texture-compression hook, streaming contracts) are folded in.

**Stages A1–A5 and B are built** — see "Current state"; the GPU-driven stages
C–G remain. The point of
this doc is that the data structures and interfaces designed today survive the
entire roadmap — submeshes, material assets, instancing, batching, LODs, GPU
culling, bindless, streaming — with internals swapped, never producers rewritten.

## Fixed decisions (the constraints everything else follows from)

- **Materials are standalone assets** — reflection-driven JSON files (`.amat`),
  referenced by virtual path, shown in the asset browser, editable in the
  inspector, shareable between meshes. They map one-to-one onto the future
  bindless material table.
- **The material definition covers the full glTF PBR channel set** — baseColor,
  normal, metallic-roughness, occlusion, emissive — factors and textures, wired
  into the shader (replacing the hardcoded `kMetallic/kRoughness`).
- The GPU-driven doc's stance holds: no Nanite/meshlets, no LOD autogen,
  instance-count is the scaling axis, well-authored assets assumed.
- Opaque-only pipeline for now. `alphaMode`/`alphaCutoff`/`doubleSided` are
  deliberately out of scope — named here so it isn't mistaken for an oversight.
  The `.amat` schema's per-field deserialization makes adding them non-breaking,
  and the sort key's pipeline bits (below) are where opaque/masked/blend buckets
  land.

## Current state (2026-07-16)

The A-foundation (A1–A5) is complete and the first GPU-driven stage (whole-mesh
AABB cull, **B**) has landed. What's built, by stage (commits in `asset-upgrade`):

- **A1** — MeshData v2: submesh / LOD / material data model + import tests
  (`cebe83b`, `4f97824`).
- **A2** — reflection plumbing: `AssetPathVector` + default-deny for unknown
  types, `AASSET` codegen backend + `AssetTypeRegistry`, `.amat` serialization
  round-trip (`a348977`, `b504dcb`, `c75a4ad`).
- **A3** — `Material` class, `MaterialConstants`, PBR default textures;
  `AssetCache` resolves materials + `prim://` texture defaults (`6dee708`,
  `2d16235`).
- **A4** — full glTF PBR rendering in the mesh pass; `MeshRenderer` gains a
  per-slot material list; editable material slots + `.amat` browser tiles;
  material-channel debug view mode; `Materials.alvl` DamagedHelmet PBR test
  level (`39ebd40`, `eb7d3de`, `10a7088`, `38e289c`, `f2f6296`).
- **A5** — DrawItem submission layer: extract → sort → Submit (`dfb7fa8`).
- **B** — two-level whole-mesh AABB cull refine (`cbc9ec4`).

The asset-identity layer (`asset-database-architecture.md`) has landed through
**S4**: GUID identity core with `.aast` sidecars and database, path→GUID
reference migration, glTF material explosion into `.amat` children + manifest,
and source-change detection with prompt-driven conflict resolution.

**Remaining:** GPU-driven stages **C–G** (geometry arena, bindless,
indirect/instancing, compute cull, HZB occlusion) and asset-DB **S5** (final
reference migration).

Still-standing foundations from before this roadmap: reflection (`ACOMP`/
`AFIELD` → reflectgen → ComponentRegistry), `.alvl` JSON scenes, the inspector,
and the asset browser.

---

## 1. MeshData v2 — submeshes, LODs, bounds (Geometry module)

Geometry stays GPU-free: it is the CPU decode target that importers produce,
tests exercise headlessly, and future tools/streaming workers consume without
touching nvrhi.

```cpp
struct SubMesh
{
    uint32_t IndexOffset  = 0;   // first index in MeshData::Indices
    uint32_t IndexCount   = 0;
    uint32_t MaterialSlot = 0;   // index into MeshData::Materials
    BoundingSphere LocalBounds;  // computed at import
    Aabb           LocalAabb;    // GPU-driven stage 1's data half
};

struct LodRange
{
    uint32_t FirstSubMesh = 0;   // submeshes stored grouped by LOD, LOD0 first
    uint32_t SubMeshCount = 0;
};

struct MeshData
{
    std::vector<Vertex>       Vertices;   // unchanged: 48B interleaved
    std::vector<uint32_t>     Indices;    // unchanged
    std::vector<SubMesh>      SubMeshes;
    std::vector<LodRange>     Lods;       // [0] = LOD0
    std::vector<MaterialData> Materials;  // material slot table (import defaults)
};
```

The vertex/index storage shape is deliberately untouched — one vertex array, one
index array per asset. That is exactly the shape that relocates into a shared
geometry arena (GPU-driven stage 2) by adding base offsets; `SubMesh` offsets are
already relative, so consumers never change.

**Degenerate rule (load-bearing):** empty `SubMeshes` ⇒ one implicit full-range
submesh, slot 0, engine-fallback material. This covers factory meshes —
`prim://cube` goes through `AssetCache::ResolvePrimitive`, never the importer —
and is what makes stage A1 pixel-identical.

**Bounds relocate Render → Geometry.** `Bounds.hpp` already consumes
`Geometry::MeshData`, so Geometry is its natural home; the importer needs it to
compute per-submesh bounds at import. `Aabb` is added beside the sphere.
`Frustum` stays in Render (renderer vocabulary). This is a mechanical namespace
rename touching `Frustum.hpp`, `MeshBuffer`, `Renderer.cpp`, `SceneRenderer`.

**Importer becomes two-phase:**

1. Walk scene nodes collecting `(primitive, worldMatrix, lodLevel)` records.
   LOD level comes from the name-suffix convention `*_LOD<n>` — matched on the
   **node name, falling back to the mesh name** (Blender exports differ); no
   suffix → LOD 0. Import and select are the engine's only LOD jobs; decimation
   lives in DCC tools, per the GPU-driven doc.
2. Bucket records by `(lodLevel, glTF material index)`. Each distinct material
   becomes one slot (shared across LODs); each bucket becomes one SubMesh, node
   transforms baked per primitive exactly as today. Same-material primitives
   still merge — draw count stays minimal; only the materials-differ case stops
   merging. Per-submesh sphere/AABB computed per bucket. `ComputeTangents`
   fallback unchanged (buckets never share vertices, so accumulation can't bleed
   across material seams).

Single-material files degenerate to exactly today's output: 1 slot, 1 submesh,
1 LOD. Nothing regresses.

**Import warnings for silently-dropped data.** The importer must diagnose, not
swallow: `TEXCOORD_1`+ (occlusion/AO commonly lives on UV1 — it would bind with
the wrong UVs otherwise), `COLOR_0`, `JOINTS_0`/`WEIGHTS_0` (skinning),
non-opaque `alphaMode`, `doubleSided`. Each warns naming what was flattened.
Second UV set / vertex color / skinning are **vertex-format evolution** items:
the future single-format geometry arena is the constraint that makes fat-vertex
vs. second-arena a decision to take consciously at that stage — recorded here so
it's a tradeoff, not a surprise.

**MeshBuffer changes (Render):**

- Copies the SubMesh/LOD tables at upload; gains
  `std::vector<const Material*> DefaultMaterials` (filled by AssetCache at
  resolve, parallel to slots). Consumers address geometry as
  `(const MeshBuffer*, submeshIndex)`.
- **Drops `_sourceData`** (the retained CPU copy). Verified: zero consumers —
  physics is box-colliders-only (Jolt `BoxShape` from
  `RigidBodyDescriptor.halfExtents`); the "for physics" comment was speculative.
  Bounds live in the SubMesh table, not raw vertices. Future consumers (mesh
  colliders, editor picking, navmesh baking) re-derive via
  `Geometry::ImportMesh` or an opt-in keep-CPU-data resolve flag — collision
  cooking wants to run once at load and keep the *cooked shape*, not raw
  MeshData. This resolves the CPU/GPU double-storage question now instead of
  deferring it to streaming.
- **Gains a stable monotonic `Id`** (assigned by AssetCache at upload, never
  reused, survives `Clear()`) — symmetric with `Material::Id`. It feeds the sort
  key today, becomes the arena mesh index at the arena stage and the
  instance-buffer mesh id after that. Never hash pointers into keys.

## 2. Materials — MaterialData, .amat, the GPU Material

### MaterialData (Geometry — CPU, paths + factors only)

```cpp
struct MaterialData
{
    // Factors — glTF pbrMetallicRoughness, spec defaults:
    glm::vec4 BaseColorFactor {1.f, 1.f, 1.f, 1.f};
    float     MetallicFactor    = 1.f;
    float     RoughnessFactor   = 1.f;
    float     NormalScale       = 1.f;
    float     OcclusionStrength = 1.f;
    glm::vec3 EmissiveFactor {0.f, 0.f, 0.f};

    // Texture channels — virtual asset paths; empty = factor-only.
    Core::AssetPath BaseColorTexture;          // sRGB
    Core::AssetPath NormalTexture;             // linear
    Core::AssetPath MetallicRoughnessTexture;  // linear; glTF: G=rough, B=metal
    Core::AssetPath OcclusionTexture;          // linear; R channel
    Core::AssetPath EmissiveTexture;           // sRGB

    std::string Name;  // UI label only (glTF material name); not serialized
};
```

sRGB vs linear is a **fixed property of the channel**, never per-file config.

Two default sets, deliberately distinct:

- *glTF spec defaults* (metallic=1, roughness=1) are the field initializers —
  correct when a glTF omits fields.
- *Engine fallback material* (id 0, for empty/missing references) is
  `{white, metallic 0, roughness 0.6, no textures}` — exactly today's hardcoded
  look, so an unmaterialed scene renders identically before and after.

### .amat — reflection-driven JSON

```json
{
  "version": 1,
  "type": "Material",
  "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
  "metallicFactor": 0.0,
  "roughnessFactor": 0.6,
  "normalScale": 1.0,
  "occlusionStrength": 1.0,
  "emissiveFactor": [0.0, 0.0, 0.0],
  "baseColorTexture": "models/crate/crate_basecolor.png",
  "normalTexture": "models/crate/crate_normal.png",
  "metallicRoughnessTexture": "models/crate/crate_mr.png",
  "occlusionTexture": "",
  "emissiveTexture": ""
}
```

Serialization rides the reflection system (see §4 — `AASSET()` /
`AssetTypeRegistry`). Deserialization is per-field `if-contains`, so added
fields never break old files.

### Material (Render — the GPU object)

```cpp
class Material
{
  public:
    uint32_t Id() const;                 // generation semantics — see below
    nvrhi::ITexture* BaseColor() const;  // never null: defaults substituted
    // ... Normal / MetallicRoughness / Occlusion / Emissive
    nvrhi::IBuffer*  Constants() const;  // MaterialConstants CB
  private:
    Geometry::MaterialData _source;      // kept for inspector edits / re-save
    nvrhi::BufferHandle    _constants;
    // texture pointers are non-owning; AssetCache owns them
};

struct MaterialConstants     // 48B used + 16B reserved pad = 64B
{
    glm::vec4  baseColorFactor;
    glm::vec4  emissiveFactorNormalScale;  // xyz emissive, w normalScale
    glm::vec4  metalRoughOcclusion;        // x metal, y rough, z occlusion
    glm::uvec4 flags;                      // bit0 = hasNormalTexture
    // bindless stage fills the pad: uvec4 textureIndices (table slots).
    // This struct IS the future material-table row, verbatim.
};
```

> The exact field packing is **current encoding, not an architectural
> guarantee** — the invariant is only that one 64-byte-aligned struct is both
> the per-material CB today and the bindless table row later. Repack freely
> until stage D freezes it.

`DefaultResources` grows into the fallback channel set: `WhiteTexture` (sRGB,
exists), `WhiteLinearTexture` (1×1, for MR/occlusion — sampling white multiplies
factors by 1, giving glTF "no texture" semantics for free), `FlatNormalTexture`
(1×1 linear `(128,128,255)`), plus `DefaultMaterialData()`. Every resolved
Material substitutes these for empty channels, so **the shader never branches on
null textures** — it always has five valid SRVs; the `flags` bit only gates the
normal-map TBN path (avoids degenerate-tangent artifacts too).

### Material::Id is a generation, not just an identity

Monotonic `uint32_t`, never reused, **survives `AssetCache::Clear()`**. The
mutation contract — this is the live-editor hot-reload fix:

- **Factor edits** rewrite the existing `MaterialConstants` CB in place. Same
  Id; the cached binding set is untouched and stays correct.
- **Texture-channel edits mint a new Id** and eagerly evict the old binding-set
  entry.

Invariant: **a binding set is immutable for the lifetime of its Id.** Stale
entries are therefore *dead, never wrong* — the failure class of the current
raw-`ITexture*` keying is eliminated by construction, and the material editor is
fully live.

### AssetCache growth

```cpp
/// "" → engine fallback material (id 0). Failed load/parse → warn-once +
/// fallback. Never null. Pointer valid until Clear().
const Material* ResolveMaterial(const Core::AssetPath& path);
```

- Keyed by `AssetPath` — two MeshRenderers naming the same `.amat` share one
  Material, one CB, one binding set.
- **Mesh-default materials live in a separate map keyed by `(meshPath, slot)`** —
  no synthetic `"<path>#<slot>"` string keys (those could silently truncate at
  `TrivialString<127>` capacity and alias slots).
- **The texture cache keys on `(path, ColorSpace)`.** Materials need sRGB
  (baseColor/emissive) *and* linear (normal/MR/occlusion) from the same cache,
  so the per-call ColorSpace is a correctness requirement, not an option — the
  current cache-wide `_textureColorSpace` cannot express a material. Keying on
  the pair also removes first-resolve-wins ambiguity. The ImGui thumbnail cache
  stays on its own path.

### Texture format evolution (deferred, hook named)

Today: stb_image → uncompressed RGBA8 + CPU-generated mips. Fine at current
scale; **not** the end state. Five-channel PBR at 2K is ~100 MB+ of VRAM per
material set uncompressed, normal maps sample worse in RGBA8 than BC5, and
CPU-side mip generation fights the streaming doc's background-upload goal.
Target: pre-cooked block compression (BC7 color, BC5 normal) or KTX2/BasisU
containers. The hook: `Texture`'s load path and the streaming loader's worker
decode take a container/format parameter when this lands. Nothing built now;
the commitment is recorded so "robust and scalable" stays honest.

## 3. glTF material import — in-memory synthesis; .amat generation is an editor action

**`ImportMesh` synthesizes materials in-memory** into `MeshData::Materials`:
each used glTF material's pbrMetallicRoughness (+ normal/occlusion/emissive)
maps to a `MaterialData`; texture URIs resolve to virtual asset paths relative
to the `.gltf` (same `ParentDir` logic already used for `.bin` buffers, same
escape-protected root). Loading stays read-only; a freshly dropped unpacked
model renders fully textured with zero manual steps.

Embedded images (`.glb` binary chunk / data URIs) are **not decoded** — the
unpack stance from the mesh/texture notes holds. An embedded image logs a
warning naming the fix (`gltf-pipeline -i model.glb -o model.gltf --separate`)
and imports factor-only.

**".amat generation" is a sandbox editor action** on a `.gltf` tile — it runs
the same mapping and writes one `.amat` per slot via the same resolve-then-write
pattern level saving uses. Naming rule: glTF material names are optional and
non-unique — uniquify as `<name-or-"material">_<slot>.amat`; never overwrite
silently. The glTF remains source of truth for its own *defaults*; `.amat`
files exist for overrides and hand authoring. No import-registry/meta-file
machinery — nothing has to remember the link.

**Accepted risk:** overrides are keyed by slot index, so a DCC re-export that
reorders materials silently rebinds every override. Standard engine tradeoff;
accepted deliberately rather than buying an import registry.

## 4. MeshRenderer v2 + reflection plumbing

```cpp
ACOMP()
struct MeshRenderer
{
    AFIELD() Assisi::Core::AssetPath meshPath;

    /// Per-slot material overrides. Element i overrides mesh slot i.
    /// Empty element (or vector shorter than slot count) = mesh's imported
    /// default for that slot.
    AFIELD() std::vector<Assisi::Core::AssetPath> materialOverrides;

    AFIELD(transient) const Assisi::Render::MeshBuffer* mesh = nullptr;
    AFIELD(transient) std::vector<const Assisi::Render::Material*> materials;
};
```

Resolution (level load and inspector edit):
`materials[i] = overrides[i] nonempty ? ResolveMaterial(overrides[i]) : mesh->DefaultMaterials[i]`,
never null. **`albedoPath` is deleted, not migrated** — exactly two committed
levels exist; hand-edit them once. A migration shim would outlive its use cases.
MeshRenderer stops being trivially copyable; the sparse set only requires
movability (a documented property being consciously spent).

**reflectgen changes, sized honestly:**

- `FieldType::AssetPathVector` — `std::vector<Core::AssetPath>` ⇄ JSON string
  array. Genuinely small: one TYPES entry, one inspector case.
- **Default-deny unknown types.** Today an unrecognized non-transient field is
  *silently skipped* — a data-loss hazard. The fix is not "populate the
  `UNSUPPORTED_TYPES` denylist" (an enumerated list can never catch types nobody
  anticipated): any non-transient `AFIELD` whose type is not in `TYPES` becomes
  a **hard generation error**. `UNSUPPORTED_TYPES` remains only for better
  messages on known-bad types.
- **`AASSET()` is a second codegen backend, not a type extension.** The existing
  generator is entirely Scene/Entity-shaped (`addToScene`, `iterateEntities`,
  `getByEntity`); a standalone asset has no entity and no scene. AASSET emits
  registration into a new `Core::Reflect::AssetTypeRegistry` with a slimmer meta
  (`name, typeIndex, fields, serialize, deserialize`) via its own generated
  template branch, with its own golden tests. This buys the `.amat` JSON shape,
  an automatic material editor (the inspector's field renderer generalizes to
  any FieldMeta list), and per-field forward compatibility — but it is real
  work, budgeted as such (stage A2).

**Editor UX:** the inspector's material rows size to the *resolved mesh's* slot
count, labeled by material Name, each with the existing browse button filtered
to `.amat`. The asset browser lists `.amat` tiles; clicking one opens the
reflection-driven material editor panel (live edits per §2's mutation contract).

## 5. Draw submission — the DrawItem layer

Split by knowledge: **extraction lives in Runtime** (knows the ECS); **the item
struct, sort, and submit live in Render** (renderer vocabulary, no ECS
dependency). `Runtime::DrawScene` becomes three phases:

```cpp
struct DrawItem
{
    uint64_t sortKey;
    const MeshBuffer* mesh;    // arena stage: only Submit() reads buffers
    uint32_t submeshIndex;
    const Material*   material;
    glm::mat4 model;           // copied — and verbatim the future per-instance record
};
```

**Opaque sort key: `[pipeline:8 | materialId:20 | meshId:20 | depth:16]`.**
Material-major for state coherence, mesh within material to keep identical
`(mesh, material)` runs adjacent (instancing collapses runs), and quantized
front-to-back view depth as the tie-break *within* runs — the GPU-driven doc's
early-Z ordering without breaking batchability. Ids are masked/asserted from
the 32-bit monotonic counters. The future **transparent pass uses a separate
depth-major key** (`[pipeline:8 | depth:32 back-to-front | materialId:24]`) —
the two-key split is the transparency seam.

> The bit allocations are **current encoding, not an architectural
> guarantee** — the invariants are: pipeline-major, then material, then mesh
> (run adjacency), depth as intra-run tie-break; and a separate depth-major
> transparent key. Widths get revisited when real scenes give real
> material/mesh counts.

Per frame:

1. **Extract** (Runtime): `Query<Transform, MeshRenderer>` → whole-mesh sphere
   frustum cull → LOD select (always LOD0 now; projected-screen-size selection
   is a drop-in here later) → emit one DrawItem per submesh of the surviving
   LOD.
2. **Sort** by key.
3. **Submit** (Render): `MeshPass::Submit(commandList, ..., span<const DrawItem>)`
   walks the sorted list — rebind binding set only when material changes,
   VB/IB only when mesh changes, push constants + `drawIndexed`
   (submesh IndexOffset/IndexCount) per item.

**Per-submesh CPU culling is deliberately absent.** Two independent review
passes converged on cutting it: (a) same-material primitives merge across the
whole model, so a submesh's bounds ≈ the mesh's bounds — the test rejects
almost nothing while costing a transform + 6 plane tests per submesh per entity
per frame; (b) the GPU-driven doc explicitly commits to whole-object cull
granularity. The per-submesh bounds **data** stays (near-free at import; LOD
selection wants it, and any future spatially-spread-materials case finds it
waiting).

`DrawStats` grows to `{drawnItems, culledMeshes, materialBinds, meshBinds}` in
the F12 overlay — state-coherence wins become directly observable.

**Why this is the future-proof seam:** the indirect/instancing stage rewrites
`Submit`'s interior only (sorted runs collapse into `drawIndexedIndirect`
commands); the bindless stage swaps `material*` for `Id` in instance data (the
id is already in the key); the compute-cull stage replaces extract+sort with an
instance-buffer upload + dispatch while Submit degenerates to a few
`drawIndexedIndirectCount` calls. Producers never change; internals swap.

## 6. Binding model + shader evolution

**Binding sets keyed by `Material::Id`** (generation semantics per §2). Replaces
the raw-`ITexture*` key and dissolves its aliasing hazard by construction.
`InvalidateBindingSets()` on level unload stays for memory hygiene, not
correctness. Cache size is bounded by material count per level.

Layout (slots chosen around the occupied light buffers — note the material SRVs
are deliberately **non-contiguous** around t1–t5; don't "tidy" the lights):

```
push constants  128B  MVP + model (unchanged — budget stays fully spent)
b0   FrameConstants           (existing)          → GLSL binding 256
b1   MaterialConstants        (new, per-material) → GLSL binding 257
t0   baseColor                (existing)          → binding 0
t1–5 clustered light buffers  (existing, untouched)
t6   normal   t7 metallicRoughness   t8 occlusion   t9 emissive → bindings 6–9
s0   shared sampler                                → binding 128
```

> Slot assignments are **current encoding, not an architectural guarantee**
> (the whole layout collapses into descriptor tables at the bindless stage).
> The invariants: material access is confined to one binding set selected per
> material-run, and the light buffers keep their existing slots.

Per-draw material references never enter push constants — they travel in the
binding set now (selected per material-run during Submit, amortized by sorting)
and become a `materialIndex` in per-instance data at the bindless stage. The
128-byte question never comes due; when the model matrix itself moves into
instance data, push constants *shrink*.

**Shader:** the vertex shader passes the tangent through (attribute already in
the input layout). The fragment shader isolates **all** material access in one
`SampleMaterial()` function — samples the five channels × factors, TBN
normal-mapping gated by the flags bit, fallbacks falling out of the default
textures — so the Cook-Torrance/clustered-lighting code is untouched, and the
bindless transition later rewrites only SampleMaterial's fetch lines
(bound CB/SRVs → `materials[idx]` + descriptor-table lookups, `nonuniformEXT`
where needed). Emissive adds after the light loops, before Reinhard — it dims
under Reinhard; accepted until a real tonemap pass exists.

**NVRHI capability status:** `createDescriptorTable`/`writeDescriptorTable` and
bindless layouts verified present in the vendored header, as are
`drawIndexedIndirect`/`drawIndexedIndirectCount`. Vulkan descriptor-indexing
device features / variable-count semantics remain a spike before the bindless
stage.

## 7. Module placement

| Thing | Module |
| --- | --- |
| MaterialData, MeshData v2, SubMesh/LodRange, Bounds+Aabb, importer changes, AASSET registration | **Geometry** |
| AssetTypeRegistry, FieldType::AssetPathVector, default-deny | **Core::Reflect** + **tools/reflectgen** |
| Material, MaterialConstants, DefaultResources growth, ResolveMaterial, MeshBuffer v2 (+Id, −_sourceData), DrawItem, MeshPass::Submit | **Render** |
| MeshRenderer v2, extract/sort (BuildDrawList) | **Runtime** |
| Material editor panel, .amat tiles, "Generate materials" action | **apps/sandbox** |

No new modules, no new dependency edges. Geometry still depends only on
Core+Math; Render still sits above Geometry.

## 8. Contracts for the streaming era

Stated now so streaming doesn't discover them the hard way:

- **AssetCache is main-thread-only until the job system lands.** `Resolve*`
  mutate unguarded maps, and the monotonic Id counters are shared mutable
  state — both become must-fix items at streaming layer 1 (async load).
- **Per-asset eviction requires refcounts on the Material → Texture edges.**
  Materials hold non-owning pointers valid-until-Clear; individual eviction
  (streaming layer 3) is where the residency/refcount table takes ownership of
  that contract. Dead binding-set entries pin GPU memory until Clear;
  per-entry eviction is the streaming-era refinement.
- **CPU-side mesh data** (dropped in §1) re-enters here as an opt-in resolve
  flag if mesh colliders / picking / navmesh ever need it.
- **Text assets don't survive streaming scale.** Reflection-driven `.amat`
  JSON is the *authoring* format; parsing thousands of JSON files on worker
  threads during chunked loads competes with decode work for CPU. Before
  streaming layer 3 (chunk residency) goes live, a **binary cooking step** for
  shipped/streamed builds converts .amat (and likely .alvl) into a
  load-and-go binary form. Authoring stays JSON; cooking is a build/export
  step, not an engine-runtime concern.

## 9. Staged rollout

Every stage ships a complete, pixel-correct renderer. A-stages are this
system's foundation; B–G are the GPU-driven doc's stages 1–6 by their original
numbers.

| Stage | Deliverable | Status | Verify |
| --- | --- | --- | --- |
| **A1** | MeshData v2 + importer rework (buckets, LOD names, per-submesh bounds, import warnings) + Bounds move. MeshBuffer copies tables; draws still full-range → identical output | ✅ done (`cebe83b`, `4f97824`) | Geometry unit tests vs a committed hand-authored 2-material 2-LOD cube; degenerate primitive rule |
| **A2** | Reflection plumbing: AssetTypeRegistry backend + AASSET, AssetPathVector, default-deny hard-error, .amat round-trip | ✅ done (`a348977`, `b504dcb`, `c75a4ad`) | reflectgen golden tests (both backends); round-trip unit test |
| **A3** | Material objects: Material/CB, ResolveMaterial, (path, ColorSpace) texture keying, MeshBuffer::Id, Id-keyed binding sets. Shader still albedo-only | ✅ done (`6dee708`, `2d16235`) | existing scenes pixel-identical; binding-set count == material count in overlay |
| **A4** | Full PBR shader + MeshRenderer v2 + editor UX (live edits per mutation contract) + hand-migrate the two .alvl levels + debug view mode (roughness/metal/normal visualization via FrameConstants flag) | ✅ done (`39ebd40`, `eb7d3de`, `10a7088`, `38e289c`, `f2f6296`) | DamagedHelmet (unpacked) renders all channels; factors-only material matches flat defaults |
| **A5** | DrawItem layer: extract → sort → Submit; expanded DrawStats | ✅ done (`dfb7fa8`) | sort on/off A/B: identical image, different bind counts |
| **B** | Per-mesh AABB in CPU cull (data exists since A1) | ✅ done (`cbc9ec4`) | GPU-driven doc stage 1 |
| **C** | Shared geometry arena (MeshBuffer → ranges; vertex-format constraint from §1 decided here) | ⬜ not started | stage 2 |
| **D** | Per-instance GPU buffer + bindless (kills the binding-set cache; MaterialConstants pad → textureIndices) | ⬜ not started | stage 3; descriptor-indexing spike first |
| **E** | CPU-built indirect draws + instancing (Submit interior only) | ⬜ not started | stage 4 |
| **F** | Compute cull + screen-size LOD select (replaces extract/sort) | ⬜ not started | stage 5 |
| **G** | Two-phase HZB occlusion | ⬜ not started | stage 6 |

Streaming (async load → loading screen → chunk residency) slots in any time
after A, on §8's contracts. Upload batching still rides the streaming loader,
never built standalone.

**Test assets:** a committed hand-authored two-material two-LOD cube
(`TestCube_LOD0/_LOD1`, exported "glTF Separate") for unit tests; Khronos
**DamagedHelmet** (every PBR channel in one material) and a multi-material
Khronos model (slots + overrides in the editor), both unpacked via
`gltf-pipeline --separate`, for when implementation reaches A4.

## 10. Risks / open questions

- fastgltf v0.9 exact material API spellings (texture→image→URI indirection,
  `normalTexture->scale`) verified at implementation time; extensions beyond
  core pbrMetallicRoughness import as defaults + warning.
- Emissive dims under Reinhard until a real tonemap pass.
- Slot-index override fragility across DCC re-export (accepted, §3).
- Multi-UV / vertex color / skinning are vertex-format evolution items gated on
  the arena decision (§1).
- Texture compression deferred with hook (§2).
- Alpha modes / double-sided deferred with the pipeline-bits seam named (§2).
- Vulkan descriptor-indexing spike before stage D — and **make the device
  selector rigid early**: the engine already requires Vulkan 1.3, but
  descriptor indexing is *optional feature bits* even there (they live in
  `VkPhysicalDeviceVulkan12Features`, having gone core in 1.2:
  `descriptorIndexing`, `runtimeDescriptorArray`,
  `shaderSampledImageArrayNonUniformIndexing`,
  `descriptorBindingPartiallyBound`,
  `descriptorBindingVariableDescriptorCount`, plus `drawIndirectCount`).
  Require them at device selection well before stage D so unsupported
  hardware fails loudly at startup instead of mid-migration. Bindless
  variable-count semantics are notoriously vendor-quirky.
- Binary asset cooking before streaming layer 3 (§8) — .amat/.alvl JSON is
  authoring-only at streaming scale.

## Related

- `docs/gpu-driven-rendering-design-notes.md` — stages 1–6 = B–G here.
- `docs/asset-streaming-design-notes.md` — builds on §8's contracts.
- `docs/old/mesh-texture-material-design-notes.md` — superseded by this doc.
- `docs/old/mesh-material-architecture-plan.md` — the working plan this doc was
  written from (review findings and their disposition).
