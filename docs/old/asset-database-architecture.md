# Asset database & `.aast` sidecars — architecture

Captured 2026-07-14. **Design settled — all decisions (D1–D7) are resolved; this
is the reference architecture, ready to implement stage by stage.** This defines how Assisi
identifies, relates, and resolves assets: stable GUID references, per-asset
`.aast` sidecar files, an editor-time import pass, and a GUID→location database.
It builds directly on the mesh/material system (`mesh-material-architecture.md`,
A-foundation implemented through A5) and the streaming roadmap
(`asset-streaming-design-notes.md`).

**Stages S1–S4 are built; S5 (cooker + `PakProvider`) is deferred until first
ship (§6).** The point of this doc is the same as the mesh doc:
choose identity, sidecar, and database shapes now that survive renames, sub-asset
explosion, team/VCS use, and a shipped-build bake — without rewriting every
reference site later.

## Why this exists

Today every reference is a **path** (`MeshRenderer.meshPath`, `.amat` texture
channels, level files all store `Core::AssetPath` strings resolved by
`AssetCache::Resolve*`). Paths are readable but brittle: rename or move a file and
every reference to it silently breaks. They also carry no identity, no import
settings, and no record of how assets compose — the mesh→material relationship is
re-derived from the glTF at load every time (`AssetCache::MeshDefaultMaterial`).

The fix is the model Unreal (`.uasset`) and Unity (`.meta`) converged on:
references point at a **stable GUID**, a **sidecar file** per asset stores that
GUID plus import settings and how the asset relates to others, and a **database**
maps GUID → current location so references survive any rename or move.

## Fixed decisions (settled with the user)

1. **`.aast` sidecars.** One `<file>.aast` next to every asset (e.g.
   `checker.amat.aast`, `helmet.gltf.aast`, `wood.png.aast`) — the Unity `.meta`
   model. The sidecar holds the asset's GUID, its import settings, and (for
   composite assets) a manifest of the sub-assets and sources it relates to.

2. **Editor-only generation.** The import pass that creates missing `.aast`s runs
   **only in the editor** (automatically on load, plus a manual "Reimport"
   button). A shipped game **never writes** into the asset tree — consistent with
   the AssetSystem split (the shipped asset root is frequently read-only:
   Program Files, app bundles, read-only mounts; writes go to the user root). A
   shipped build consumes a baked database instead (§6).

3. **Reconcile, never clobber; lazy validation.** The import pass assumes all
   existing engine files are valid: if a `<file>.aast` exists, leave it untouched;
   if it is missing, generate it. It does **not** parse or validate sidecars
   during the scan — well-formedness is checked only when an asset is actually
   loaded. (Cheap scan: a directory walk + existence test, no per-file parse.)

4. **Pure GUID references — no hybrid.** Every reference is a GUID; there is no
   "path-or-GUID" union at reference sites. Engine built-ins that are not files
   (`prim://cube`, `prim://white`, `prim://white-linear`, `prim://flat-normal`)
   get **reserved constant GUIDs** from a fixed low namespace
   (`00000000-0000-0000-0000-00000000000N`, Unity-style) so they are
   collision-proof against minted UUIDs and greppable. The resolver maps a
   reserved GUID → the primitive factory in the same lookup `ResolveMesh` already
   does for `prim://` today. Rationale: hybrid's only real benefit was JSON
   readability of built-ins, which is worthless once every other reference is a
   UUID (rely on the editor showing names); the "built-ins are special" concern
   lives in the resolver, not the reference type; and the reserved-GUID table is
   ~4 one-line entries.

5. **glTF is authoritative on import, then `.amat` takes over.** Importing a glTF
   still reads its materials as the source of truth, but the import pass
   **explodes them into real `.amat` files** (each with its own `.aast`/GUID), and
   the glTF's `.aast` manifest records `slot → material GUID`. After import the
   engine references those `.amat` GUIDs, not the glTF's embedded materials.

## Design principle: one GUID = one file

Unity addresses sub-assets inside a file with a secondary `fileID` (a material
packed inside an `.fbx` has one GUID for the file + a localID for the material).
Assisi sidesteps that: the import pass **materializes sub-assets as their own
files** (glTF materials → `.amat` files). So every GUID maps to exactly one file,
every reference is a single GUID, and there is no localID/sub-addressing layer.
The composite's `.aast` manifest is what records "these child GUIDs came from me."

This is the simplification that makes pure-GUID clean here: the only "asset" the
database ever resolves is a file, and the only thing a reference ever holds is one
GUID.

## Current state (2026-07-16)

Stages **S1–S4 are built**; **S5** (cooker + `PakProvider`) is deferred until
first ship (§6). What's landed, by stage (commits in `asset-upgrade`):

- **S1** — GUID identity core: `AssetId`, `.aast` sidecars, and the
  `AssetDatabase` (GUID→path), with the reconcile import pass wired into the
  editor and sidecar generation (`5ad92f1`, `fc56e17`).
- **S2** — references switched from `Core::AssetPath` strings to `AssetId`
  GUIDs, resolved through the database / provider (`6096554`).
- **S3** — glTF material explosion: import writes `.amat` children + the
  slot→GUID manifest; mesh→material resolution reads the manifest, retiring the
  live `MeshDefaultMaterial` derivation (D4) (`6e7132d`).
- **S4** — source-change detection (`sourceHash`) + conservative auto-reconcile,
  then the field-wise reconciler with prompt-driven conflict resolution and
  liveness scoping (D5) (`4f51a58`, `9b8d30d`).

Underneath, unchanged: `AssetSystem` reads raw bytes by virtual path
(`ReadText`/`ReadBinary`/`Resolve`, asset-root read-only + user-root
read-write); `AssetCache` dedupes resolution with `prim://` primitives
short-circuited; `reflectgen` knows `AssetPath`/`AssetPathVector` and `.amat` is
an `AASSET` via `AssetTypeRegistry`.

**Remaining: S5** — archive format, compression/transcode, baked GUID→location
index, and the shipped `PakProvider`. The S1 provider interface means no loading
code changes when it lands.

## 1. Identity — the GUID and the `AssetId` type

- **GUID = random UUIDv4** (128 bits), minted the first time the editor sees a
  file with no `.aast`. Random (not a counter) so two machines or two branches
  never collide when their work merges — this matters the moment the project is
  under version control with more than one history.
- **`AssetId`** — a new 16-byte value type (`std::array<uint8_t,16>` under the
  hood) that replaces `Core::AssetPath` in *stored references* (components, level
  files, `.amat` channels, `.aast` manifests). Trivially copyable, heap-free,
  cheap to hash — a drop-in for AssetPath's role as an inline reference token.
- **Reserved built-in range.** `AssetId`s `…0000` through `…00FF` are reserved
  for engine built-ins (the `prim://` primitives get fixed values here). Minted
  UUIDv4s never fall in this range (version/variant nibbles differ), so the
  guarantee is structural, not probabilistic.
- **reflectgen support.** A new `FieldType::AssetId` (and `AssetIdVector` for the
  material-override list) serializing as a UUID string — mirrors how `AssetPath`/
  `AssetPathVector` were added. Known codegen surface; same default-deny guard.

## 2. The `.aast` sidecar

A reflection-driven file like `.amat` (an `AASSET`, envelope
`{ "version", "type", <fields> }`), so adding fields flows through codegen. One
schema, two roles depending on the asset:

- **Leaf asset** (texture, audio): `{ guid, importSettings }`. For a texture the
  import settings are the color space and (future) compression/format — the
  per-asset settings that currently have nowhere to live.
- **Composite asset** (glTF, and any asset that references others): additionally a
  **manifest** — the child/source GUIDs and how they attach. For `helmet.gltf`:
  `{ guid, subAssets: [ { slot, materialGuid } … ], sourceHash }`.

Every file under the asset root gets a `.aast` (see D6), including `.amat` and
`.alvl` levels (GUID + any settings). The **only** exclusion is a `.aast` itself —
a sidecar never gets its own sidecar, or the reconcile scan would never terminate
(`x.png.aast.aast…`). That exclusion is a termination requirement, not a policy
choice.

The manifest is the "relationship file" the user asked for: it records that this
mesh's slot 0 is that material's GUID, so the mesh→material default binding is a
**stored fact**, not something re-derived from the glTF at load.

## 3. Asset providers — one interface, two backends

The GUID→bytes step is **not one reader**. In the editor, assets are loose files
with `.aast` sidecars resolved through a live, mutable database. In a shipped
build, assets are packed into archives and the `.aast` files **do not exist** —
their content was consumed at cook time and baked into an archive index. The
storage, the lookup, and the byte-fetch are all different. What is *the same* is
everything above the bytes: parsing `.amat` JSON into a `MaterialData`, uploading
a mesh, resolving texture channels — identical regardless of where the bytes came
from.

So the seam is a provider interface:

```
AssetProvider (interface, shipped):   Open(AssetId) → ByteSpan / stream
  ├── LooseFileProvider  (editor-only) : live AssetDatabase (GUID→path, built by
  │                                       scanning .aast sidecars) → read loose file
  └── PakProvider        (shipped)     : baked GUID→{archive, offset, size} index
                                         → slice of an archive, decompressed
```

- The **deserializers and `AssetCache` sit above `AssetProvider`** and never know
  which backend served them. `AssetCache` keys its GPU resources by `AssetId`
  (replacing today's path keys); built-in GUIDs short-circuit to the primitive
  factories exactly as `prim://` does now, in either backend.
- The **`.aast` file is an editor source artifact — it does not ship.** It feeds
  `LooseFileProvider`'s database and the cooker; the shipped game reads a pak
  index, never a sidecar. "Shipped `.aast` reader" is a misnomer: there is no such
  thing.
- `AssetSystem` stays the raw byte-IO layer *underneath* the loose provider (it is
  how `LooseFileProvider` actually reads a file); `PakProvider` reads archives by
  its own path. The database speaks GUIDs; `AssetSystem` still speaks paths.

### 3a. The AssetDatabase (editor-only, behind `LooseFileProvider`)

The mutable GUID→path map that only exists while authoring.

- **Build:** on editor load, walk the asset root; for each asset file, ensure a
  `.aast` exists (generate + mint UUID if missing — the reconcile pass), then
  register `guid → path` from the sidecar. Reserved built-in GUIDs are seeded from
  a static table. Full scan now; incremental (mtime/watch) is a later refinement,
  named not built.
- **Rename/move robustness falls out for free:** the file moved, its `.aast`
  (carrying the same GUID) moved with it, the next scan registers the new path
  under the same GUID, every reference still resolves. That is the entire payoff —
  and it is an authoring-time payoff, which is why the database is editor-only.

## 4. The import pass (editor-only)

One pass, run on editor load and on the manual "Reimport" button:

1. Walk the asset root.
2. For each asset file with no `.aast`: mint a UUID, write `<file>.aast`.
3. For an importable composite with no generated children (a glTF whose `.amat`s
   don't exist yet): run the importer, write one `.amat` (+ `.aast`) per material
   into the model's directory as `<model>_<material-or-slot>.amat` (the naming
   rule from the mesh doc §3, uniquified, never silently overwriting), and record
   `slot → material GUID` in the glTF's `.aast` manifest.
4. Build the database from the sidecars.

Reconcile-not-clobber governs every step: anything that already exists is left
alone. A **source-changed re-import** (the glTF was edited after its `.amat`s were
generated) is deliberately *not* automatic under this rule — see Decisions (D5).

## 5. Reference migration

- `MeshRenderer`: `meshPath: AssetPath` → `mesh: AssetId`; `materialOverrides:
  vector<AssetPath>` → `vector<AssetId>`.
- Level files and `.amat` texture channels store GUID strings.
- The committed levels + `checker.amat` are migrated once (they reference
  `prim://cube` = a reserved GUID, and `materials/checker.amat` = checker's minted
  GUID). Readability of level JSON drops to UUIDs — mitigated by the inspector/
  browser always showing resolved names, never raw GUIDs (see D2).

## 6. Cooking & the shipped provider

The shipped side is a **cooker** (an editor/build-time tool) plus a runtime
**`PakProvider`**. They are the concrete second backend of §3.

- **Cook (editor-side tool):** read every `.aast` + its payload, pack the payloads
  into archive(s) — compressed, deduped, and eventually platform-transcoded (BC7
  color / BC5 normal, KTX2) — and write a **baked GUID→{archive, offset, size}
  index** into the archive header. The `.aast` metadata is *consumed* here: import
  settings drive the transcode, the manifest's slot→GUID relationships are baked
  into whatever the runtime needs, and then the sidecars are dropped. Nothing
  `.aast` ships.
- **Runtime `PakProvider`:** loads the baked index once and resolves
  `AssetId → archive slice`. Zero asset-tree writes, no per-file scan, no sidecar
  parsing — the shipped game only consumes what the cook produced. This is why
  generation is editor-only: the editor authors and cooks; the runtime consumes.

**Scope (agreed): define the interface now, defer the cooker.** The
`AssetProvider` interface + `LooseFileProvider` are built early (S1–S2) so all
loading code is written against the interface from the start and the editor path
works. The **pak format + cooker + `PakProvider`** are a real subsystem (archive
format, compression, transcode, baked index) that is **not needed until first
ship** — named here and scheduled as a late stage (S5), not built alongside the
editor path. Analogous to how `asset-streaming-design-notes.md` names the
streaming layers without building them yet.

## 7. Module placement

The split follows the shipped/editor line from §3, not "one home for everything."

- **Core (shipped):** `AssetId`, the `AssetProvider` **interface**, `.aast`
  *reader* (deserialize), and the payload deserializers that sit above the
  provider. These ship in every build. `AssetSystem` unchanged (it underlies
  `LooseFileProvider`).
- **Editor-only (`#if EDITOR` in Core, or a tools lib):** `LooseFileProvider`, the
  live `AssetDatabase`, GUID/UUID *minting*, `.aast` *writer*, the reconcile
  import pass, the Reimport button, and the name-resolution the inspector/browser
  use to show GUIDs as names. Stripped from shipped builds.
- **Cooker + `PakProvider` (S5):** the cook tool is editor/build-side;
  `PakProvider` is the shipped runtime backend of `AssetProvider`. Deferred (§6).
- **reflectgen / Core::Reflect:** `FieldType::AssetId` + `AssetIdVector`.
- **Render:** `AssetCache` re-keys on `AssetId` and reads bytes through
  `AssetProvider`; `MeshDefaultMaterial` is replaced by reading the glTF `.aast`
  manifest (see D4).
- **Runtime:** `MeshRenderer` field type change.

No new dependency edges beyond a possible `Assets` module under Core. Promote the
editor-only pieces into a standalone `Assets` module only if they grow a
dependency that shouldn't live in Core.

## 8. Staged rollout (each stage shippable)

- **S1 — provider interface + sidecars + database, refs unchanged.** ✅ done
  (`5ad92f1`, `fc56e17`). The `AssetProvider` interface, `LooseFileProvider`, the
  `.aast` format, the reconcile import pass (generate missing, mint UUIDs), the
  `AssetDatabase` (GUID→path), reserved built-in GUIDs. Loading routes through
  `LooseFileProvider`; paths still drive resolution and the DB is built but not yet
  the reference key. Verifiable in isolation (scan a tree, assert sidecars + map).
- **S2 — `AssetId` type + reflectgen + reference switch.** ✅ done (`6096554`).
  Introduce `AssetId`, add the codegen field types, switch `MeshRenderer` and the
  level files to GUIDs, resolve through the DB / provider. Migrate the committed
  levels. Scenes render identically.
- **S3 — glTF material explosion + manifest.** ✅ done (`6e7132d`). Import pass
  writes `.amat` children and the slot→GUID manifest; mesh→material default
  resolution reads the manifest instead of `MeshDefaultMaterial` deriving live.
- **S4 — Reimport UX + source-changed policy.** ✅ done (`4f51a58`, `9b8d30d`).
  The manual Reimport button + `sourceHash` stale detection first; then the
  field-wise reconciler with the confidence-gated, liveness-scoped policy (D5) —
  auto-reconcile provably-safe diffs, prompt otherwise, immediate for loaded
  assets. (Still all editor/loose path.)
- **S5 — cooker + `PakProvider` (shipped).** ⬜ not started — deferred until first
  ship (§6). Archive format, compression/transcode, baked GUID→location index, and
  the shipped `PakProvider` backend. The interface from S1 means no loading code
  changes here.

## Decisions (all resolved — D1–D7)

- **D1 — GUID format. RESOLVED: 128-bit random UUIDv4**, minted lazily on first
  sight. Rejected: content hash (identity breaks on edit), path hash (breaks on
  rename or re-collides), monotonic counter (shared-registry merge hotspot).
  64-bit random was considered (collision-safe into the hundreds of millions and
  half the bytes) but the width is cold serialization data that never enters a
  hot path, so the 8-byte cost is insignificant; 128-bit chosen for standard-UUID
  tooling familiarity and zero-thought headroom. Reserved built-in range
  `…0000`–`…00FF` stands (a real v4 UUID's version nibble can't land there).

- **D2 — Path-hint in the serialized reference. RESOLVED: GUID + advisory
  full-path hint, disk-only.** The serialized form carries both the GUID and a
  last-known virtual path (e.g. `textures/crate.png`); the **hint lives only on
  disk**. On load the deserializer reads the GUID and discards the hint, so the
  **in-memory `AssetId` stays a pure 16-byte GUID** — runtime and hot path are
  untouched. On save the serializer regenerates the hint by asking the DB for the
  asset's current path, so the hint **self-heals**: it can only be cosmetically
  stale in a file not re-saved since a rename, never functionally wrong (the GUID
  is always the sole thing that resolves — single source of truth preserved).
  Chosen over pure GUID because level/`.amat` files are hand-read and git-diffed
  here: the hint makes diffs legible ("crate.png → barrel.png", not "a UUID
  changed") and gives a hand-recovery net if the DB is ever lost — a full path can
  rebuild the GUID→file mapping where a bare filename would be ambiguous. Cost: a
  little serialization code and slightly larger files; zero runtime cost. Rejected:
  pure GUID (opaque diffs, unrecoverable-by-hand if DB dies), name-only hint
  (lighter but can't seed DB recovery).

- **D3 — Where does the `AssetId`/`.aast` code live? RESOLVED: split by the
  shipped/editor line, not by module.** The shipped half — `AssetId`, the
  `AssetProvider` interface, the `.aast` reader, the payload deserializers — lives
  in **Core** (visible to component structs; ships in every build). The editor
  half — `LooseFileProvider`, the live `AssetDatabase`, UUID minting, `.aast`
  writer, the reconcile import pass — is **editor-only** (`#if EDITOR` in Core or a
  tools lib), stripped from shipped builds. The shipped `PakProvider` + cooker are
  their own late stage (S5). Rationale (refined with the user): there is not one
  reader — the editor reads loose files via a mutable DB, the shipped game reads a
  baked archive index, and the `.aast` file does not ship at all; the common seam
  is the `AssetProvider` interface, above which all deserialization is shared.
  `AssetId` *must* be Core-visible (component fields), which caps how high the type
  can live regardless. Promote the editor pieces to a standalone `Assets` module
  only if they grow a dependency that shouldn't sit in Core. See §3, §6, §7.

- **D4 — Does `.aast` supersede load-time mesh→material resolution? RESOLVED:
  yes — retire `MeshDefaultMaterial`.** The manifest's stored `slot → material
  GUID` becomes the source of the default binding at S3, and `MeshDefaultMaterial`
  (today's live per-index derivation from the glTF) is removed. This is the
  concrete win of "relationship files": the mesh→material link is an explicit,
  inspectable, hand-editable, GUID-pinned fact instead of hidden derivation, and
  it fixes the slot-order fragility — the binding is recorded once at import, so a
  DCC re-export that reorders materials no longer silently rebinds by index.
  Retirement is gated on S3 (the manifest path must exist before the live path is
  removed). Source-changed drift is handled by D5, not by keeping the live path.

- **D5 — Source-changed re-import policy. RESOLVED: confidence-gated
  reconciliation, scoped by liveness.** The deciding axis is not "manual vs. auto"
  but *"can we prove this change is safe?"* Detection primitive: a `sourceHash` of
  the source stored in the composite's `.aast` manifest; a mismatch marks the asset
  stale.

  Two axes govern what happens on a mismatch:

  - **Confidence (what to do):** a reconciler classifies the diff.
    - **Provably-safe → auto-reconcile silently.** The change touches nothing that
      can conflict with authored state: e.g. only mesh geometry changed while slot
      count, slot→material assignment, and every material channel are unchanged; or
      a purely additive new slot that only needs a fresh default `.amat`. When we
      can be 100% confident the result is correct, no prompt — just reconcile.
    - **Not-provably-safe → prompt the user.** Anything that *could* lose or
      mis-map authored intent: slots reordered/removed, a hand-edited channel whose
      source value changed, ambiguous slot identity. We never silently resolve what
      we can't guarantee; the author decides.
  - **Liveness (when to force it):** an asset **currently loaded in the open scene**
    whose source changed is reconciled **immediately** (auto if safe, prompt if
    not) — it's live and out of date, so it can't wait. An asset **not loaded**
    stays **badged stale** and is reconciled lazily (on next load or manual
    Reimport). Same policy, different urgency.

  Mechanism: the **field-wise reconciler** (map matching channels old→new
  albedo→albedo, fill genuinely-new fields with type-appropriate defaults, drop
  only truly-removed fields, preserve hand-edits) is what both *classifies* a diff
  (a clean merge with no conflicts ⇒ provably-safe) and *executes* the safe ones.
  Manual **Reimport** stays available to force the pass anytime.

  *Staging:* the classifier may start **conservative** — the initial cut treats
  only the trivial cases as auto-safe (identical config, or geometry-only) and
  prompts for everything else — then widens as the reconciler matures. `sourceHash`
  + the manual Reimport button ship first (S4); the confidence classifier and
  liveness-driven immediate reconcile land with the reconciler system. No earlier
  decision blocks widening the "safe" set later.

- **D6 — What counts as an "asset" that gets a sidecar? RESOLVED: every file
  under the asset root.** Presence in the asset directory *is* the declaration of
  intent — if it's in there, the user meant to put it there, so it's an asset and
  gets a GUID; anything outside the asset root is by definition not an asset and is
  never touched. The **only** exclusion is a `.aast` itself (a sidecar gets no
  sidecar — a scan-termination requirement, not a policy choice). This means level
  files (`.alvl`), `.amat`s, and loose glTF `.bin` buffers all get GUIDs too;
  harmless (a `.bin` stays the glTF's private payload resolved via the glTF's
  manifest — its GUID just goes unreferenced). Recognized-type detection still
  matters, but only for *how to import/load* a file, never for *whether* to sidecar
  it. Rejected: allowlist-only (cleaner tree, but second-guesses the user's intent
  and needs the list maintained just to decide identity).

- **D7 — Migration cutover. RESOLVED: big-bang at S2.** When `AssetId` lands
  (S2), the resolver speaks only GUIDs and the 4 committed files (`Test.alvl`,
  `Lights.alvl`, `Materials.alvl`, `checker.amat`) are hand-migrated in the same
  atomic commit — resolver + all 4 files together, or a level won't load. No
  path-accepting resolver ever exists. At this scale (4 files) a transitional
  dual-resolver would cost more than the migration it smooths *and* reintroduce
  exactly the path-or-GUID union pure-GUID (decision 4) exists to avoid; a
  one-shot auto-migrate-on-save is dead weight after 4 files. Reserved built-in
  GUIDs must exist first (they do, from S1). The migrated files stay readable
  thanks to D2's disk-only path-hint (GUID + `prim://cube` hint), so hand-editing
  isn't blind. Rejected: dual-resolver (rejected-hybrid complexity), auto-migrate
  (one-shot code justified only at hundreds-of-files scale).

## Related

- `mesh-material-architecture.md` — the mesh→material→draw pipeline this refers
  to (A3/A4 implemented: `.amat`, `AssetCache::Resolve*`, `MeshDefaultMaterial`,
  material slots).
- `asset-streaming-design-notes.md` — the streaming layer; the AssetDatabase is
  where its residency/refcount table eventually keys off GUIDs.
- Deferred tasks it absorbs: "Generate `.amat` from glTF" becomes the import
  pass's S3 step (no longer a standalone editor action).
