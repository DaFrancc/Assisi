# Lighting — design notes & implementation plan

> **SUPERSEDED 2026-08-15 by [`../lighting-and-shadows-plan.md`](../lighting-and-shadows-plan.md).**
> That document replaces this one rather than amending it. Kept for history only — do not
> follow anything below.
>
> What the successor changed, in brief:
> - **Frame budgets removed as a gating concept.** Nothing is excluded on cost; every
>   feature is a knob with a measured, published cost. The per-stage ms budgets below are void.
> - **Mobility trichotomy cut** from shadows — cache invalidation is inferred from the
>   tracked-Transform change ticks the engine already computes. Mobility returns only for
>   baked GI, where "will this ever move" genuinely cannot be inferred.
> - **L0 test suite cut from seven scenes to three** plus `Lights.alvl`; the pinned-clock
>   ±0.05 ms protocol replaced with within-session A/B deltas guarded by NVML clock readings,
>   because clocks cannot be pinned without root.
> - **Ray tracing and the whole temporal family (TAA, temporal upscaling, frame generation)
>   are out by standing decision**, with no reopening trigger — a firmer position than this
>   document's.
> - **SSAO and PCSS restored**; **quality tiers** promoted to core scope; **sky and ambient
>   moved ahead of the local-light atlas**.
> - **GI direction chosen**: hybrid precomputed transfer, dynamically relit — and repositioned
>   from indefinite deferral to the next major project after shadows.

Captured 2026-07-21. v2 2026-07-22 (research sweep, 12 primary-source
verification checks). v3 2026-07-22 (two adversarial reviews at 6.5/10 and
7/10 + blind adjudication of 6 disputes). **v4 2026-07-22** after a third
independent review of v3 (7.5/10) — its findings (composite mechanism,
movable lights, the door problem, min-spec, test scenes, AO, sky) are
designed-in below; review record at the end. **Nothing here is built yet.**
Goal in one line: **a lighting system that looks modern and supports
everything — many shadowed point lights, GI — where cost scales with what is
placed in the level, correctness is the default, and a feature that isn't
placed costs ~nothing.**

## Targets & budgets

- **Min-spec:** GTX 1060 6GB / RX 580-class, 1080p60 (16.6 ms frame).
  **Perf target:** RTX 3060 / RX 6600-class, 1080p144 (6.9 ms frame).
  (Tiers are proposals until real test hardware is chosen.)
- **Min-spec is a gate, not decoration** (v4): budget gates run on
  perf-target hardware at every stage merge, and on min-spec at every stage
  *completion* with a 3× headroom rule (a stage's min-spec cost may not
  exceed 3× its perf-target budget). If min-spec hardware is unavailable,
  derate perf-target numbers by 3× on paper and mark the gate provisional.
- Per-stage GPU budgets, reference scene, perf-target @1080p:
  L1 CSM ≤ 0.8 ms · L2 post ≤ 0.5 ms · L3 dynamic shadow work ≤ 0.8 ms
  (sub-line: K=2 *moving* shadowed lights within it) · L4 sampling
  ≤ 0.25 ms · L4.5 SSAO (opt-in) ≤ 0.5 ms · L5 sampling ≤ 0.15 ms · L6
  refresh (opt-in) ≤ 0.5 ms at default N. Everything active ≈ 3.5 ms ≈ half
  a 144 fps frame — tight; unused stages cost nothing, per the founding
  rule.
- Memory defaults: local-light shadow atlas **D16** (v4 — standard for
  local shadows; halves memory) 4096² = 32 MB, 8192² = 128 MB option; sun
  CSM stays D32 on its own texture; probe volumes ≤ 16 MB/region;
  reflection probes ≤ 32 MB/scene. Exceeding a budget degrades resolution,
  never correctness.

## Stage L0 — test scene suite (v4; the substrate every gate runs on)

Deliverable before L1 merges: **blank** (empty world), **reference**
(defined content: ~50 k triangles visible, 8 shadowed point + 4 spot + 1
sun, 4 movables incl. 1 skinned, stated draw-call scale), **stress** (4×
reference), **thin-wall**, **enclosed-room**, **door-room** (a lit exterior,
a dark interior, one Stationary door), and **corridor** (interior with sky
visible through an opening). Every gate below names its scene. Measurement
protocol: GPU timestamp queries, median over ≥ 500 frames, **GPU clocks
pinned** (locked-clocks mode; all vendors have one); where pinning is
impossible, compare A/B deltas within one session, never absolutes across
sessions. Tolerance ± 0.05 ms under pinned clocks.

## Decisions

- **Governing principle: pay for what you place.** Zero passes/bandwidth
  for features not in use. Blank scene stays near-zero forever (the
  anti-UE5-baseline rule).
- **Mobility is a trichotomy (v4): `Static | Stationary | Movable`.**
  Static: never moves; cached everywhere, baked everywhere. **Stationary:
  objects with discrete states that change occasionally — doors, elevators,
  movable walls. Included in probe bakes at current state; a state change
  dirties affected shadow tiles and the probes within the object's
  influence radius, which L6 re-bakes as a burst.** Movable: continuous
  movers (characters, physics props) — dynamic shadow casters, excluded
  from bakes. Editor-set, default Static for level geometry, forced Movable
  for skinned meshes. This classification is the foundation of every cache
  and bake below; it is built first. (The Stationary class is the door-
  problem fix — see L4/L6.)
- **Local-light shadows: one unified shadow atlas with caching, all light
  types, per-light `castsShadows` default ON** (correctness by default;
  turning shadows off is the explicit per-light opt-out). Production
  pattern: DOOM 2016/Eternal, Godot, Flax (atlas + caching + per-light
  update rate). **Composite mechanism specified (v4): copy-then-rasterize**
  — one cached static-depth copy per tile; each frame a light has Movables
  in range, the cached depth is copied into the live tile (512² D16 =
  0.5 MB bandwidth — bounded, honest) and Movable casters rasterize on
  top. Tiles with no Movables in range are sampled directly: zero per-frame
  *render* cost (sampling/cluster cost is per-frame and counted in budgets;
  the old "zero cost" wording was sloppy).
- **Movable lights have a policy (v4):** a light whose transform changed
  skips static caching that frame — full tile re-render, throttled by a
  per-light update-rate knob (Flax-style: 1, ½, ⅓ frames) and importance
  scaling; caching resumes after the light rests a few frames. Spot = 1
  face (cheap — the flashlight case); moving point = 6 faces (the
  expensive case; the K=2 budget sub-line exists for exactly this).
  Granularity note: a moving light dirties all its faces; a Movable object
  dirties only the faces whose frustum contains it.
- **Point shadows are first-class** — 6 cube faces per light, each face
  rendered with slightly widened FOV so border texels bake into the tile
  (standard seam fix; per-face culling uses the widened frustum; PCF taps
  clamp to the tile's interior rect). Dual-paraboloid rejected (artifacts).
- **The atlas allocator is staged:** v1 = power-of-two size classes,
  rect-based addressing (atlas UV rect + matrix per light record), no
  resizing, overflow demotes least-important lights a size class. v2 =
  importance-driven resizing (screen area × distance) with hysteresis +
  eviction (the DOOM Eternal destination). Rect interface makes v1→v2
  invisible to shading. "Renders once, ever" holds in v1; v2 resizes
  re-render cached depth, made rare by hysteresis. Capacity quantified
  (v4): "many shadowed point lights" = **16–24 well-resolved** (256–512²
  faces) at 4096²/D16, more with the 8192² option; a DoD covers demoted-
  tier visual acceptability.
- **Directional (sun) shadows: CSM, separate from the atlas.** Stable
  snapped cascades, 3×3 PCF. Variance/moment formats rejected: bleeding is
  intrinsic to the Chebyshev bound (MJP 2015: 32-bit EVSM "still
  exhibits bleeding"), AAA ships plain PCF (DOOM Eternal), and
  prefiltering only pays off at kernel widths we don't use. (2013 cost
  deltas are historical context, not the argument.)
- **Diffuse GI: DDGI-style probe grid, baked in-engine; refresh is an
  opt-in budget.** Octahedral irradiance + distance moments with Chebyshev
  visibility weighting — the leak fix *is* the design (naive grids provably
  leak: JCGT 2019, Monter; SH-encoded depth rings). Scope notes: visibility
  weights zero out bad probes but can't conjure good ones (all-8-invalid →
  darkening; RTXGI-style relocation is the known fix, deferred with trigger
  "dead zones appear in real scenes"; probe layout reserves a per-probe
  world-offset field from day one). **Continuous movers do not occlude
  bounce light — stated limitation** (v4), shared by every baked pipeline;
  the standard patch (per-character capsule occlusion on indirect, à la
  Uncharted/TLOU) is a possible future line-item, not planned scope.
- **Specular GI: reflection probes with leak controls in v1:** influence
  volumes + interior/exterior priority (the correctness features — they
  stop wall/sky bleed) enforced at froxel cluster-list build; box-projection
  parallax and blending are quality fast-follows. Specular occlusion v1 =
  material AO on indirect specular.
- **Sky/environment lighting exists (v4):** a procedural sky dome (two-
  color gradient v1, upgradeable) rendered into every probe bake and into
  one generated global reflection probe (the outdoor specular fallback).
  HDR by construction (internal, no asset import). **Rule: `kAmbient` is
  disabled during bakes** — otherwise it double-counts into every bounce
  iteration.
- **The baker is the renderer.** Small cubemaps via our own forward pass;
  multi-bounce via iteration; no path tracer, no BVH, no RT requirement.
- **Explicit ray tracing stays out permanently.** Sole sanctioned use: the
  optional L7 backend, behind a numeric trigger.

## Cross-cutting prerequisites

- **Mobility trichotomy** (above) — first thing built; L3 and L4 have no
  foundation without it.
- **HDR internal assets:** probe irradiance (R11G11B10F), distance moments
  (RG16F), prefiltered reflection cubemaps (RGBA16F) — engine-generated,
  serialized via `.aast`/`AssetCache`. HDR *imports* stay out of scope;
  emissive = LDR texture × float intensity.
- **Bake versioning:** every bake stores a hash of static+stationary
  geometry/state, static light set, probe positions, volume params, and its
  companion bake. **L4 diffuse and L5 specular bakes dirty together**
  (mismatched indirect diffuse/specular on one surface otherwise). Editor
  shows a "GI stale" marker on mismatch. Baked mode assumes a static sun;
  time-of-day = L6 bursts at transitions or per-game descope.
- **Debug viz is a deliverable:** cascade colorizer + map viewer (L1),
  atlas inspector with tile ownership/cache state (L3), probe visualizer
  spheres + stale indicator (L4), influence-volume wireframes (L5).
- **Light units:** pick one scalar convention now; calibrate L2 exposure
  against it.
- **GPU timestamp queries** for the gates (small; the parked frame-profiler
  project is not required).

## Current state (surveyed 2026-07-21)

Clustered forward PBR renderer, Vulkan via NVRHI, GLSL 450 → SPIR-V (the
DOOM-family base). Direct lighting: Cook-Torrance GGX, dir/point/spot via
`modules/Render/src/ClusterGrid.cpp` + `assets/shaders/cluster_*.comp`; CPU
structs in `modules/Runtime/.../LightingSystem.hpp`. Indirect:
`const float kAmbient = 0.03;` in `assets/shaders/cube_min.frag` — all GI
stages funnel into replacing it. Reinhard inline; no HDR target; FXAA
exists. Compute dispatch wrapper in use. Texture pipeline LDR/RGBA8.
No shadows, no IBL, no mobility flag, no ray infra.

**Gap ledger:** (a) HDR render targets — L2; (b) HDR internal asset
serialization — L4/L5; (c) shadow passes + atlas allocator — L1/L3;
(d) mobility trichotomy — prerequisite; (e) timestamp queries —
prerequisite; (f) test scenes — L0; (g) ray infra — only L7.

## Stage L1 — directional cascaded shadow maps

Depth-only D32; v1 single cascade → 3–4 stable snapped cascades (fixed
FOV/near/far assumption noted; SDSM rejected — readback dependency). 3×3
PCF. Bias design explicit: slope-scaled + normal-offset, auto-scaled by
cascade texel footprint, per-light overrides. Knobs: enabled, resolution,
distance, cascade count, PCF radius. Budget ≤ 0.8 ms (reference scene).
**Done when:** no crawl during strafe *and rotation*; no acne/peter-panning
at default bias (reference scene); cascade boundaries invisible in
albedo-flat scenes; off = pass absent from capture.

## Stage L2 — HDR pipeline: filmic tonemap, bloom, exposure

RGBA16F scene target; post chain: scene → bloom (Karis first downsample,
firefly clamp) → filmic tonemap (ACES/AgX) + manual EV + gamma → FXAA →
swapchain. Reinhard deleted. Histogram auto-exposure = follow-up; exposure
defaults settle **before** L4 bake validation. Specular aliasing: geometric
specular AA (Toksvig-style) in-stage; TAA stays out of scope. Budget
≤ 0.5 ms. **Done when:** emissive blooms; filmic rolloff; no firefly
flicker from a small bright emissive during camera orbit; FXAA intact.

## Stage L3 — local-light shadow atlas (spot + point)

4096² **D16** atlas (8192² option). v1 allocator: power-of-two size
classes, rect addressing, overflow = class demotion of least-important
lights (capacity math: 64 × 512² tiles ≈ 10 point lights full-res; the
16–24 target uses mixed classes). v2: importance sizing + hysteresis +
eviction. Caching per the composite decision (copy-then-rasterize; zero
per-frame render cost for clean tiles). Movable-light policy per Decisions.
Alpha-tested casters via depth-only alpha-test shader variant. Sampling via
cluster light records (rect + matrix; point faces by major axis; widened-
FOV borders). Bias auto-scaled per tile resolution.
Budget ≤ 0.8 ms dynamic work on the reference scene, including the K=2
moving-lights sub-line. **Done when:** point light occluded in all 6
directions including across face seams; a static shadowed light with no
Movables in range shows zero per-frame render cost (capture); 20 shadowed
lights degrade resolution not correctness, **and demoted 256² faces are
visually acceptable at typical ranges (new gate)**; a Stationary door
state-change updates its tile correctly; no placed light leaks unless
shadows were explicitly disabled; atlas inspector live.

## Stage L4 — probe-grid diffuse GI (DDGI-style, baked)

Uniform grids, 1–2 m spacing, region-scoped volumes (one-cell overlap +
crossfade between volumes v1). Per probe: 8² octahedral irradiance
(R11G11B10F) + 16² distance/distance² (RG16F) + reserved world-offset.
Sampling: trilinear × soft-backface × Chebyshev × low-irradiance
perceptual term + normal-biased offset (verified against the paper; G3D /
RTXGI reference GLSL). Bake: 6 × 64² faces via own forward pass (L1+L3 on,
Static+Stationary included, Movables excluded, sky dome on, `kAmbient`
off, previous iteration bound), depth→radial conversion + octahedral
projection + moment pre-blur (the named risk surface; probe visualizer
exists for this), 2–3 iterations. In-editor "Bake Lighting". Persistence
via bake-hash versioning (incl. Stationary states). Budget ≤ 0.25 ms
sampling; ≤ 16 MB/region. **Done when:** colored-wall bounce; enclosed
room leak-free; **door-room scene: closed door blocks bounce after its
burst re-bake settles (new gate)**; thin-wall scene checked for darkening
(that's the relocation trigger, not a weight bug); bake deletion falls
back to `kAmbient`; stale marker fires on static/stationary edits.

## Stage L4.5 — SSAO (opt-in contact occlusion; new in v4)

Probes at 1–2 m spacing cannot produce contact darkening; without it,
baked-probe GI reads flat ("the baked look"). Basic SSAO needs only the
depth buffer — no TAA, no motion vectors: hemisphere sampling + spatial
blur, **applied to indirect lighting only** (AO on direct light is wrong
and looks it). Opt-in toggle, off = zero cost, ≤ 0.5 ms budget on. **Done
when:** contact darkening visible under props and in corners on the
reference scene; direct light unaffected (capture-verified); off = passes
absent.

## Stage L5 — reflection probes (specular IBL)

Hand-placed probes, influence volumes + interior/exterior priority in v1
(enforced at cluster-list build — that *is* the leak control), GGX-
prefiltered mips at bake, split-sum LUT, material-AO specular occlusion.
Plus the generated global sky probe as outdoor fallback. Box projection +
blending = fast-follows. Budget ≤ 0.15 ms. **Done when:** corridor scene:
interior receives zero sky specular (capture-verified selection); metallic
sphere reflects the correct room at 3 roughness values; boundary
transitions switch without wrong-room content; stale marker fires with
L4's.

## Stage L6 — runtime probe refresh (opt-in dynamism + event bursts)

Two modes, one mechanism (re-bake probes through the L4 path, fully
GPU-side):
- **Budget mode (continuous):** N probes/frame, round-robin/prioritized.
  Default N=1–2 at 144 fps (≤ 0.5 ms); N=0 = pure baked, the global
  default. Serves local dynamism (a moving light drags its bounce with
  visible, accepted latency).
- **Burst mode (event-driven; the Stationary hook):** a state change
  (door, elevator) enqueues the probes in its influence radius at an
  elevated temporary budget until drained — tens of probes, settled in
  well under a second, hidden behind the instantly-correct direct shadow
  change. This is the door-problem fix.
Cost honesty (adjudicated v3): one probe = 6 × 64² forward renders ≈
24.6 k fragments *plus* 6 tiny views of CPU cull/submit — ~100× the
sample count of a 256-ray update; fragment cost trivial, per-view CPU
overhead is the real cost, so the refresh path gets aggressive per-face
culling from day one. Convergence stated honestly: 500-probe volume at
N=4 ≈ 2 s per full pass — scene-wide changes (moving sun) are bursts at
transitions or out of scope per game. **Done when:** moving light drags
bounce; door-room gate (L4) passes via burst; N=0 restores baked cost
exactly; default-N cost within budget.

## Stage L7 — accelerated probe-ray backend (optional; numeric trigger)

Replace cubemap re-bakes with rays (software SDF à la Flax — noting that
path is a subsystem of its own: per-mesh SDF generation + global SDF with
~hundreds of MB to manage, so the trigger must justify a *project*, not a
patch — or hardware ray query where present). Identical probe output;
never required. **Trigger:** measured L6 cost exceeds its budget at the N
a real game needs.

## Transparency & volumetrics

- **Prerequisite (added 2026-07-22): the material system has no `alphaMode`/
  `alphaCutoff`.** Both bullets below assume it does. L3's alpha-test depth
  variant cannot be written without those fields, and the transparent forward
  pass needs the blend bucket in the sort key — neither exists today
  (`mesh-material-architecture.md` §2 had them parked as out of scope). The
  cheap half — the fields, the material table entry, and the masked pipeline
  bit — is small and non-breaking, and is what unblocks L3; the blended pass
  can follow. Land that before L3, or L3 ships foliage casting solid
  rectangular shadows.
- Transparent receivers: the transparent forward pass samples the same
  cluster lists, atlas, and probes as opaque (clustered forward's
  advantage); lands with L3/L4 as shader-variant work. OIT out of scope.
- Cutout casters via L3's alpha-test depth variant; colored translucent
  shadows out of scope permanently.
- Particles render into the HDR target pre-tonemap; may sample probes
  later.
- Volumetrics/fog: explicit future work post-baseline (froxel grid is the
  substrate, atlas provides occlusion; nothing here blocks it).

## Rejected techniques (evidence on file)

- **SDFGI-class:** unfixable enclosed-space leaks (Godot #50770, open
  since 2021); camera-motion frame drops; successor HDDAGI on its third
  unmerged PR (Intel device-lost, DX12, RADV issues). Temperamental.
- **Voxel cone tracing:** always-on voxelize+mip+trace regardless of
  content ("only very fast GPUs" — Wicked's author); per-scene artifact
  tuning.
- **Radiance cascades:** PoE2's is screen-space-fed (wrong for free
  cameras); world-space 3D research-stage (O(N⁴), experimental 128³);
  fixed baseline cost — inverse of our model. Re-evaluate in years.
- **Screen-space GI:** needs TAA/motion vectors; off-screen limitation.
  (Screen-space *AO* is different — depth-only, and now stage L4.5.)
- **Virtual shadow maps & VSM/EVSM formats:** grounds in Decisions;
  Olsson et al. cited only for cluster-reuse feasibility (their numbers
  used virtualized cube maps — atlas-scale claims rest on DOOM/Godot/Flax
  shipping it).
- **Lightmaps / second UV:** out unless a game demands per-texel static
  detail (then external Blender bake).
- **Explicit ray tracing as a visual feature:** never.

## Invariant & measurement (merge gates)

- **Blank-scene gate:** feature unused → frame time unchanged (L0
  protocol: pinned clocks, median ≥ 500 frames, ± 0.05 ms, blank scene).
- **Pay-for-placement gate:** static shadowed light with no Movables in
  range, and a baked probe volume: ~zero steady-state per-frame *render*
  cost in a capture.
- **Budget gates:** per-stage ≤-budgets on the named scene, perf-target
  each merge; min-spec 3× headroom at stage completion.
- **Correctness gates:** L3 wall/seam tests, L4 enclosed-room + door-room,
  L5 corridor-specular — pass with defaults.

## Review record

v2→v3: two adversarial reviews (6.5, 7/10); accepted findings folded in;
6 disputes blind-adjudicated (rulings in git history of this file).
v3→v4: third independent review (7.5/10). Accepted and designed-in:
composite mechanism (copy-then-rasterize), movable-light policy + K-budget,
door problem (Stationary mobility class + L6 burst mode; continuous-mover
limitation stated, capsule-occlusion noted as future option), min-spec made
a real gate (3× headroom), L0 test-scene suite, pinned-clock measurement
protocol, SSAO as L4.5, sky dome + kAmbient-off-during-bakes rule, D16
atlas + demoted-tier quality gate + "many lights" quantified (16–24),
L7 fallback effort honestly sized, "zero cost" wording corrected to
"zero render cost". Pushed back (recorded, not adopted): calendar
estimates (AI-assisted throughput makes dependency order the useful
planning unit).
