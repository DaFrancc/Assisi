# Mesh & Material System Architecture — Design Document

## Context

The engine currently has no material concept: `MeshRenderer` = `{meshPath, albedoPath}`, the glTF importer discards materials, the forward shader hardcodes `metallic=0.0 / roughness=0.6`, and draws are one `drawIndexed` per entity with binding sets keyed by raw `ITexture*`. Three existing docs (`mesh-texture-material-design-notes.md`, `gpu-driven-rendering-design-notes.md`, `asset-streaming-design-notes.md`) record intent but no unified architecture.

The user wants a **robust, scalable mesh + material architecture** whose data structures and interfaces survive the full roadmap (submeshes, material assets, instancing, batching, LODs, GPU culling, bindless, streaming) without throwaway work. **User decisions:** materials are standalone assets (.amat, reflection-driven JSON); the material definition and (eventual) shader wiring cover the full PBR channel set; **this session delivers the design document only — no code.**

This plan was reviewed by two independent adversarial review agents (Fable 5 + Opus); their consensus findings are incorporated below.

## Deliverable

**Create `docs/mesh-material-architecture.md`** — the authoritative architecture design, written in the same voice/format as the existing design-notes docs (dated header, "what exists / what's designed" split, sequencing section). Also add a short pointer in `docs/mesh-texture-material-design-notes.md` noting it is superseded by the new doc (its "two orthogonal decisions" are now resolved).

## The architecture to document

### 1. MeshData v2 (Geometry module — stays GPU-free)
- `SubMesh {IndexOffset, IndexCount, MaterialSlot, BoundingSphere LocalBounds, Aabb LocalAabb}` — one per (LOD, material slot).
- `LodRange {FirstSubMesh, SubMeshCount}`; submeshes stored grouped by LOD, LOD0 first. LOD import by name-suffix convention `*_LOD<n>` — match on **node name, falling back to mesh name** (Blender exports differ); no suffix → LOD0.
- `MeshData` gains `SubMeshes`, `Lods`, `Materials` (vector<MaterialData> slot table). **Degenerate rule (explicit):** empty `SubMeshes` ⇒ one implicit full-range submesh, slot 0, engine-fallback material — this covers factory/primitive meshes (`prim://cube` bypasses the importer via `AssetCache::ResolvePrimitive`) and guarantees A1's pixel-identical claim.
- Vertex/index arrays unchanged — the shape that later relocates into a shared geometry arena (GPU-driven stage 2) by adding base offsets only.
- **Bounds relocate Render→Geometry** (`Bounds.hpp` consumes `Geometry::MeshData`, so Geometry is its natural home). Add `Aabb` beside the sphere — GPU-driven stage 1's data half. `Frustum` stays in Render. Note in doc: this is a mechanical namespace rename touching Frustum.hpp, MeshBuffer, Renderer.cpp, SceneRenderer.
- **Importer**: two-phase — collect `(primitive, worldMatrix, lodLevel)` records, then bucket by `(lodLevel, glTF material index)`; each bucket = one SubMesh (same-material primitives still merge; only materials-differ stops merging). Single-material files degenerate to today's output exactly.
- **Import warnings for silently-dropped data**: `TEXCOORD_1`+ (occlusion/AO commonly uses UV1 — imports with wrong UVs otherwise, must be diagnosed), `COLOR_0`, `JOINTS_0/WEIGHTS_0` (skinning), non-opaque `alphaMode`, `doubleSided`. Each warns naming what was flattened. Second UV set / vertex color / skinning are named as known **vertex-format evolution** items; the doc records that the future single-format geometry arena is the constraint that makes fat-vertex vs. second-arena a decision to take consciously at stage C.
- **`MeshBuffer` drops `_sourceData`** (the retained CPU copy). Verified: zero consumers — physics is box-colliders-only (Jolt `BoxShape` from `RigidBodyDescriptor.halfExtents`, `PhysicsWorld.cpp:249-258`); the "for physics" comment was speculative. Bounds live in the SubMesh table. Future consumers (mesh colliders, editor picking, navmesh) re-derive via `Geometry::ImportMesh` or an opt-in keep-CPU-data resolve flag. Resolves the CPU/GPU double-storage question now.
- **`MeshBuffer` gains a stable monotonic `Id`** (assigned by AssetCache at upload, never reused, survives `Clear()`) — symmetric with `Material::Id`, feeds the sort key, and becomes the arena mesh index at stage C / instance-buffer mesh id at stage D.

### 2. MaterialData + .amat (Geometry) / Material (Render)
- `MaterialData`: glTF pbrMetallicRoughness factors (BaseColorFactor vec4, Metallic/Roughness/NormalScale/OcclusionStrength floats, EmissiveFactor vec3) + five `AssetPath` texture channels (baseColor sRGB, normal linear, metallicRoughness linear [glTF packing G=rough, B=metal], occlusion linear, emissive sRGB) + UI-only `Name` (not serialized to .amat). Color space is a fixed property of the channel, never per-file config.
- `.amat` = reflection-driven JSON (`{"version":1, "type":"Material", ...fields}`), per-field `if-contains` deserialization → forward-compatible. `alphaMode`/`alphaCutoff`/`doubleSided` deliberately out of scope (opaque-only pipeline) but named in the doc as the fields that land later, with the sort key's pipeline bits as their bucketing seam.
- Engine fallback material (id 0) = today's hardcoded look (white albedo, metallic 0, roughness 0.6). glTF spec defaults (metallic=1, roughness=1) remain MaterialData's field initializers.
- `DefaultResources` grows: WhiteTexture (exists), WhiteLinearTexture (MR/occlusion), FlatNormalTexture (128,128,255 linear). Every resolved Material substitutes defaults for empty channels → **shader never branches on null textures**; a `flags` bit gates only the normal-map TBN path.
- Render-side `Material`: non-owning texture ptrs + `MaterialConstants` CB. **Layout stated precisely: 48B used (baseColorFactor 16 + emissive/normalScale 16 + metal/rough/occlusion/flags 16) + 16B reserved pad = 64B; stage D fills the pad with `uvec4 textureIndices`** — the "verbatim future bindless row" claim is thereby checkable.
- **`Material::Id` is a generation, not just an identity**: monotonic, never reused, survives `Clear()`. **Mutation contract (the hot-reload fix):** factor edits rewrite the existing CB in place (same Id — binding set unaffected); **texture-channel edits mint a new Id** and eagerly evict the old binding-set entry. Invariant stated in doc: *a binding set is immutable for the lifetime of its Id.* This keeps the live material editor (A4) fully hot, and stale entries stay dead-not-wrong.
- `AssetCache::ResolveMaterial(path)`: "" → fallback; failed load → warn-once + fallback; never null. Keyed by AssetPath (shared). **Mesh-default materials live in a separate map keyed by `(meshPath, slot)`** — no synthetic string keys (a `"<path>#<slot>"` string could silently truncate at TrivialString<127> capacity and alias slots).
- **Texture cache keyed on `(path, ColorSpace)`** — materials require sRGB and Linear from the same cache (baseColor vs normal/MR), so the per-call ColorSpace is a correctness requirement, not an enhancement; keying on the pair removes the first-resolve-wins ambiguity. Doc notes the ImGui thumbnail cache deliberately stays on its own path.
- **Texture format evolution (deferral-with-hook, required for streaming honesty):** today = stb_image → uncompressed RGBA8 + CPU mips; the doc names the target (KTX2/BasisU or pre-cooked BC7 color / BC5 normal), notes 5-channel PBR at 2K ≈ ~100MB+ uncompressed per material set, and states the hook: `Texture` load path and streaming layer 1 (worker decode) take a container/format parameter when compressed textures land. Nothing built now; commitment recorded.

### 3. glTF material import stance
- `ImportMesh` **synthesizes materials in-memory** (into `MeshData::Materials`); texture URIs resolve to virtual paths relative to the .gltf (same `ParentDir` logic as .bin buffers). Embedded .glb/data-URI images NOT decoded — warn naming `gltf-pipeline --separate`, import factor-only.
- **".amat generation" is a sandbox editor action** on a .gltf tile (writes `<modelDir>/<name>.amat` via the same resolve-then-write pattern level-saving uses — `SandboxLevels.cpp:103-112` → `SceneSerializer::SaveToFile`). **Naming rule:** glTF material names are optional/non-unique — uniquify as `<name-or-"material">_<slot>.amat`, never overwrite silently. glTF stays source of truth for its defaults; .amat exists for overrides and hand authoring.
- **Accepted risk (named in doc):** slot-index-keyed overrides rebind silently if the DCC re-export reorders materials — standard engine tradeoff, deliberately no import-registry machinery.

### 4. MeshRenderer v2 + reflection plumbing
- `{meshPath, AFIELD() std::vector<AssetPath> materialOverrides, transient mesh*, transient vector<const Material*> materials}`. Empty/short vector → mesh default per slot. **`albedoPath` deleted** — hand-edit the two committed .alvl files once; no migration shim. Doc acknowledges MeshRenderer stops being trivially copyable (sparse set requires movability only — a documented property being consciously spent).
- **reflectgen changes, sized honestly:**
  - `FieldType::AssetPathVector` (vector<AssetPath> ⇄ JSON string array) — genuinely small: one TYPES entry + one inspector case.
  - **Default-deny unknown types**: any non-transient AFIELD whose type is not in `TYPES` is a **hard generation error** (not "populate `UNSUPPORTED_TYPES`" — that's an enumerated denylist and can never catch unanticipated types; today unknowns silently skip, a data-loss hazard). `UNSUPPORTED_TYPES` remains only for better error messages on known-bad types.
  - **`AASSET()` is a second codegen backend, not a type extension**: ComponentMeta/generate_cpp are Scene/Entity-shaped (addToScene/iterateEntities/getByEntity); a standalone asset needs a parallel registry (`Core::Reflect::AssetTypeRegistry`: name, fields, serialize/deserialize — no scene hooks), its own generated-template branch, and its own golden tests. A2 is sized as real work.
- Inspector: per-slot rows sized to the resolved mesh's slot count, labeled by material Name, browse button filtered to .amat. Asset browser lists .amat tiles; clicking opens the reflection-driven material editor panel (live edits per the mutation contract in §2).

### 5. Draw submission layer (the scalability seam)
- `Render::DrawItem {uint64 sortKey, const MeshBuffer* mesh, uint32 submeshIndex, const Material* material, mat4 model}`.
- **Sort key (opaque): `[pipeline:8 | materialId:20 | meshId:20 | depth:16]`** — depth = quantized front-to-back view-space distance as tie-break *within* material+mesh runs: preserves state coherence and instancing run-adjacency while delivering the GPU-driven doc's front-to-back early-Z within each run. Doc states the future transparent-pass key is depth-major back-to-front (`[pipeline:8 | depth:32 | materialId:24]`) — the two-key split is the transparency seam. 20-bit ids: masked/asserted from the 32-bit monotonic counters.
- Runtime produces: extract (Query → **whole-mesh sphere frustum cull only** → LOD select (LOD0 now; screen-size drop-in later) → emit one DrawItem per submesh of the surviving LOD) → sort by key. Render consumes: `MeshPass::Submit(span<const DrawItem>)` — rebind binding set only on material change, VB/IB only on mesh change.
- **Per-submesh CPU culling: cut** (both reviewers, independently): same-material primitives merge across the model so submesh bounds ≈ mesh bounds (rejects ~nothing, costs a transform+test per submesh per frame), and the GPU-driven doc explicitly commits to whole-object cull granularity. Per-submesh bounds **data** stays (cheap at import; LOD selection and any future refinement want it).
- `DrawStats` grows: `{drawnItems, culledMeshes, materialBinds, meshBinds}` in F12 overlay.
- Future-proofing unchanged: stage E rewrites Submit's interior (sorted runs → indirect commands); stage D swaps material* → Id in instance data; stage F replaces extract/sort with instance-buffer upload + compute dispatch. Producers never change.

### 6. Binding model + shader evolution
- Binding sets keyed by `Material::Id` (generation semantics per §2 — stale = dead, never wrong; edits mint new Ids). `InvalidateBindingSets()` on Clear stays for memory hygiene.
- Layout: push constants unchanged (128B MVP+model); b1 = MaterialConstants CB (GLSL binding 257); **t0 baseColor stays, t1–t5 remain the clustered light buffers, t6–t9 = normal/MR/occlusion/emissive** — doc states plainly the material SRVs are non-contiguous around the light buffers so nobody "tidies" them apart (collapses at bindless anyway); s0 shared sampler.
- Shader: vert passes tangent through; frag isolates all material access in one `SampleMaterial()` function (5 channels × factors, TBN gated by flags bit, fallbacks fall out of default textures). Cook-Torrance/cluster code untouched. Emissive added after light loops, before Reinhard (dims — noted, acceptable until real tonemap). Bindless transition later rewrites only SampleMaterial's fetch lines.
- NVRHI pre-verified: `createDescriptorTable`/`writeDescriptorTable` (nvrhi.h:3747-3750), bindless layouts (:2069), `drawIndexedIndirect`/`Count` (:3369, 3378). Vulkan descriptor-indexing feature flags remain a pre-stage-D spike.

### 7. Module placement
Geometry: MaterialData, MeshData v2, SubMesh/LodRange, Bounds+Aabb, importer changes, AASSET registration. Core::Reflect + reflectgen: AssetTypeRegistry, AssetPathVector, default-deny. Render: Material, MaterialConstants, DefaultResources growth, ResolveMaterial, MeshBuffer v2 (+Id, −_sourceData), DrawItem, MeshPass::Submit. Runtime: MeshRenderer v2, extract/sort. Sandbox: material editor, .amat tiles, Generate-materials action. **No new modules, no new dependency edges.**

### 8. Explicit contracts for the streaming era (new section in doc)
- **AssetCache is main-thread-only until the job system lands** — Resolve* mutate unguarded maps; the monotonic Id counters are shared mutable state; both get named as must-become-thread-safe items at streaming layer 1.
- **Per-asset eviction requires refcounts on the Material→Texture edges** — Material holds non-owning pointers valid-until-Clear; individual eviction (streaming layer 3) is where the residency/refcount table takes ownership of that contract. Dead binding-set entries pin GPU memory until Clear; per-entry eviction is the streaming-era refinement.
- CPU-copy retention (mesh colliders etc.) re-enters here as an opt-in resolve flag if ever needed.

### 9. Staged rollout (each stage shippable, pixel-correct)
- **A1** Submeshes + LOD import + import warnings (Geometry only; MeshBuffer copies tables, draws still full-range → identical output). Test: hand-authored 2-material 2-LOD cube fixture + degenerate primitive-mesh rule.
- **A2** Reflection plumbing (AssetTypeRegistry backend + AASSET, AssetPathVector, default-deny hard-error, .amat round-trip; reflectgen golden tests for both backends). Sized as the second codegen backend it is.
- **A3** Material objects (Material/CB/ResolveMaterial, (path,ColorSpace) texture keying, MeshBuffer::Id, id-keyed binding sets; shader still albedo-only → scenes identical).
- **A4** Full PBR shader + MeshRenderer v2 + editor UX (live edits per mutation contract) + hand-migrate 2 levels. Test: DamagedHelmet (unpacked). Debug view mode (roughness/metallic/normal visualization) via FrameConstants flag.
- **A5** Draw-item layer (whole-mesh cull → LOD0 → emit → sort → Submit) + expanded DrawStats. A/B sort toggle: identical image, different bind counts.
- Then: **B** AABB cull refine → **C** geometry arena (vertex-format constraint decision recorded in §1) → **D** instance buffer + bindless → **E** CPU indirect + instancing → **F** compute cull + screen-size LOD select → **G** HZB. Streaming slots in after A per §8 contracts.
- Test assets: committed hand-authored test cube; DamagedHelmet + a multi-material Khronos model (unpacked via `gltf-pipeline --separate`).

### 10. Risks / open questions (doc's closing section)
fastgltf v0.9 material API spellings verified at implementation; emissive dims under Reinhard; slot-order fragility across DCC re-export (accepted, §3); multi-UV/vertex-color/skinning are vertex-format evolution items gated on the arena decision (§1); texture compression deferred-with-hook (§2); alpha/double-sided deferred with pipeline-bits seam named (§2); Vulkan descriptor-indexing spike before stage D.

## Files

- **Create:** `docs/mesh-material-architecture.md` (the whole deliverable)
- **Edit:** `docs/mesh-texture-material-design-notes.md` (2-3 line superseded-by pointer at top)

## Verification

- Doc review pass: every file:line reference cited in the doc checked against the actual source (Read the referenced lines).
- Cross-doc consistency: sequencing section must agree with `gpu-driven-rendering-design-notes.md` stages 1–6 and `asset-streaming-design-notes.md` sequencing (naming their stages explicitly).
- No code changes → build/tests unaffected; nothing to run.
