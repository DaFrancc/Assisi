# Lighting & Shadows — plan of record (2026-08-15, rev 3)

**Adopted 2026-08-15.** The issue-level worklist lives in the Linear project "Rendering"
(team Engine), whose description and milestone descriptions carry the same decisions in
executable form. This document is the *reasoning* behind them: the forks considered, the
options rejected and why, the measurements, and the sources. Linear says what to build;
this says why, and why not the alternatives.

Written against branch `render` at 4876806. This **replaces**, not amends, the v4 notes in
`docs/old/lighting-design-notes.md` — that document was mined as input and is cited below
only to say what was kept, what was cut, and why. **Rev 2** re-derived the performance
envelope from real hardware targets. **Rev 3** removes frame budgets as a gating concept
entirely, at the programmer's direction, and replaces them with a measured-cost ledger
plus a no-cliff degradation principle (§1.2). The architecture survives unchanged; the
gating model does not.

Issues implementing this plan: ENG-136 (measurement), ENG-80 (shadow flags), ENG-33 (sun
CSM), ENG-86 + ENG-142 (sky, ambient, indirect seam), ENG-139 + ENG-140 (local atlas,
caching), ENG-141 (editor diagnostics), ENG-143 (specular AA), ENG-144/145/146 (sky probe,
PCSS, SSAO). Prerequisites: ENG-135 → ENG-82 (HDR, tonemap).

The brief, in the programmer's words: *"the best lighting and shadow system we can make —
one that looks great AND that is performant. If there are a few paths, I want to hear
them."* And, on budgets: *"I don't have a frame budget in mind, I just want to squeeze
out everything... don't exclude features only because they don't fit the frame budget, we
want everything."* Sections 4.x are the paths. Everything else is the one-sane-answer
material.

---

## 1. Targets, philosophy, and the cost ledger

### 1.1 Hardware facts (unchanged from rev 2)

- **Dev/measurement machine: RTX 3070, 8 GB, 2560×1440.** Current baseline on it:
  **700–800 fps ≈ 1.3 ms/frame at 1440p** on `Lights.alvl`.
- **Coverage goal: ≥90% of "reasonable gaming setups"** — down into the ≤6 GB tier
  (GTX 1060/1650, RX 580 class). Steam July 2026: modal GPU RTX 3060 (3.71%), #2 RTX 4060
  Laptop (3.44%); 1080p 51.1% of primary displays vs 21.5% at 1440p; VRAM 16 GB 25.9% /
  8 GB 25.3% / 12 GB 12.9% / ≤6 GB ≈16%. Consequence: **quality tiers are core scope from
  day one** (§6.0), and the modal gate resolution is 1080p even though measurement
  happens at 1440p on the dev machine (both get measured — §5).
- **HARD CONSTRAINT: no upscaling, no frame generation.** DLSS/FSR/XeSS/frame-gen appear
  nowhere in this plan — not as defaults, not as fallbacks, not as polish. The renderer
  is natively fast or it has failed. (AA remains MSAA/FXAA, as today.)
- **Stretch direction: "the average system able to pull off 4K144."** The arithmetic
  (checked): 4K144 is 9.6× the pixel throughput of 1080p60; scaling the 1.3 ms baseline
  by pixel ratio gives ≈2.9 ms at 4K on the 3070 (an overstatement — much of 1.3 ms is
  fixed overhead that doesn't scale with pixels), leaving ≈4 ms of a 6.94 ms frame there
  and ≈2.7 ms on a 3060. Tight but not fantasy for modest content. The practical
  consequence is not a budget — it is that **per-pixel costs are the scaling enemy**, so
  every per-pixel feature (filter kernels, ambient terms) must have a cheap setting on
  its knob, whatever its default ends up being.

### 1.2 The gating model: measured costs, not budgets

The programmer's position, adopted throughout: old games run superbly on average modern
hardware because their software was lean, not because their budgets were generous; the
goal is to squeeze out everything the hardware offers, and an ECS engine keeps more of
the frame for graphics than the OOP engines those games shipped on. Therefore:

- **Nothing is excluded from the plan on cost grounds.** Every feature that improves the
  look gets built. (Rev 2 violated this once — it used a derived 0.6 ms shadow budget to
  kill PCSS at planning time. That is a cost estimate deleting a feature before it was
  ever measured. Corrected in §4.1 and §6.)
- **Every feature ships as a runtime knob with a measured, published cost**: ms added,
  A/B against the previous stage's baseline, on the dev machine, at both 1080p and
  1440p, recorded in the issue that landed it. The ledger of these numbers *is* the
  performance discipline — without a budget you cannot say "acceptable", only "this
  costs X and here is what X buys", so X must always be known and public.
- **Performance is achieved by what is ON BY DEFAULT, not by what exists in the
  codebase.** Cost estimates decide defaults; they never decide what gets built.
- **Default tier assignments are decided after measurement**, at each stage's gate.
  Every default stated in this document (§6.0 table included) is **provisional,
  confirmed or moved at that stage's gate** — the table is a map of the knob space, not
  a set of promises.
- **Three absolute principles survive at any philosophy:**
  1. **Blank-scene / pay-for-what-you-place:** a feature that costs anything when unused
     is a bug. Runs on every stage merge.
  2. **No silent regression:** a stage that slows the previous stage's baseline paths
     (features off) has failed its gate regardless of what its own feature costs.
  3. **No performance cliffs.** The programmer's stated goal is that a developer using
     "reasonable map and game design" gets a fast, beautiful game *without ever having to
     tear the engine apart to keep it fast*. The thing that forces tearing-apart is the
     cliff: place one more light and frame time doubles. So the requirement on every
     system here — the local-light system above all — is **proportional, predictable,
     monotonic degradation**: cost rises smoothly with what is placed, with no knee.
     This is a stronger and more specific claim than "fast", and it is testable — the
     Lights.alvl sweep in §5 is its test.
- A stage's performance gate is therefore: *cost measured and published at 1080p+1440p,
  justified against what it buys, no regression of the prior baseline, blank-scene gate
  green.* Who judges "justified" is the programmer, at the gate, looking at the ledger
  and the picture — which is exactly the approval gate structured_workflow.txt already
  has.

### 1.3 What the 1.3 ms baseline does and does not prove — and the one collision

`Lights.alvl` is 270 PointLight + 29 SpotLight + 1 DirectionalLight against only **15
MeshRenderers** (verified by count). It proves the clustered light-culling path
(ClusterGrid + cluster_cull.comp) is cheap at high light counts — true and it stays
true. It says **nothing about shadowed cost**, which scales with caster geometry ×
shadow-map renders — 6 faces per point light, so 270 shadowed points = **1,620 face
renders per frame**.

The programmer has been explicit that hundreds of shadowed lights is **not a
requirement**: *"this level is just for stress testing"* — the actual requirement is
that a developer on the sensible route gets a fast, beautiful game by default (§1.2
principle 3). But the arithmetic still has to be named plainly rather than papered
over: if absurd input arrives anyway, 1,620 cull+submit passes per frame is infeasible
at any philosophy, on any hardware this decade — a complexity-class problem, not a
budget one. So the local-light design (§4.2) carries two mechanisms with distinct jobs:
**static-depth caching** (core efficiency: a resting light's tiles cost ~zero render
time — this is what makes generous shadowed-light counts cheap on sensible content) and
**top-N importance selection** as a **graceful-degradation backstop, not a target** —
its default N sits comfortably *above* typical sensible-scene light counts so it almost
never binds during normal authoring, and when absurd input makes it bind, the frame
degrades smoothly (fewer shadowed lights, smaller tiles) instead of cliffing or dying.
Its gate is not "does it fit" but "when it binds, is the degradation smooth, stable,
and correct?" And it is never silent: the editor tells the author when a light has left
the fast path (S7b).

### 1.4 Exceeding the old games — what that concretely means

The 2010s titles that still run beautifully (and are the reference point for "the
software is the problem") bought their look with **precomputation that froze their
levels**: baked lightmaps, baked probes, hard caps on dynamic shadow-casting lights,
static geometry that could never move because the lighting was painted onto it. Their
lesson is real — leanness wins — but their trick is unavailable to a general-purpose
engine that wants authors to move things.

"Exceeding them" therefore means, concretely: **fully dynamic shadows and lighting — any
light, any mesh, movable at any time — at cost comparable to or better than their baked
pipelines**, achieved through what they didn't have: clustered forward culling (already
proven cheap here), cached shadow tiles with inferred invalidation (§4.3 — the cache
gives back most of what baking bought, without freezing anything), GPU-driven submission,
and vastly stronger hardware. Stages S1–S7 deliver exactly that for direct lighting.

The honest implication for **indirect** lighting: the old games' ambient/bounce was
baked; this plan's interim (hemisphere ambient + optional global sky IBL) is *simpler*
than their bakes, not better. Under the stated ambition, ENG-137 (probe GI) — dynamic or
fast-rebaking bounce light — is the axis on which this engine would actually *surpass*
the 2010s look rather than match it. This plan does not re-scope GI into itself, but the
recommendation changes: **"deferred indefinitely" should become "deferred until S7
lands, then planned as the next major project"** — see §8's ENG-137 row.

### 1.5 Memory (hardware limits, not preferences — these stay)

Per-tier shadow texture totals: **Low ≈14 MB** (3×1024² D16 + 2048² D16), **Medium
≈80 MB** (3×2048² D32 = 48 + 4096² D16 = 32), **High ≈96 MB** (4×2048² D32 + 4096² D16),
**Ultra ≈384 MiB** (4×4096² D32 = 256 MiB + 8192² D16 = 128 MiB). Low fits 4 GB cards
with room to spare; Ultra is for the 12 GB+ quarter of the market.

## 2. What "looks great" decomposes into (priority order)

Ranked by how much each contributes to the perceived quality of the *whole image*, from
what shipping engines actually spend effort on (id Tech, Godot, UE, Frostbite writeups):

1. **Stability under camera motion.** Shimmering/crawling edges are the single loudest
   "cheap engine" tell. Fix: sphere-bounded cascade fit + texel snapping (§4.1). Costs
   math, not milliseconds. Load-bearing.
2. **Leak/acne freedom at default settings.** A shadow system that needs per-scene bias
   fiddling never looks great because nobody fiddles every scene. Fix: slope-scaled +
   normal-offset bias auto-scaled by texel footprint. Also ~free. Load-bearing.
3. **Ambient in the shadows.** With today's flat `kAmbient = 0.03` (mesh.frag:525,
   `kDefaultAmbientIntensity` in MeshPass.hpp:53), sun shadows render as near-black pits
   and the feature looks *worse* than no shadows. A sky-tinted hemisphere ambient is the
   cheapest fix that makes shadows read as "lit by the world". Load-bearing, and
   currently missing from the shadows milestone entirely.
4. **Tonemapped HDR response.** Without filmic rolloff the sun can't be bright enough to
   make shadows matter. Already planned (ENG-135/82); hard prerequisite, §3.
5. **Resolution where the eye is.** Cascade distribution for the sun; per-light tile
   sizing for locals. A tuning knob per tier, defaults set at gates.
6. **Filter quality.** PCF with hardware-bilinear comparison taps; kernel size is a
   knob (1-tap → 3×3 → 5×5/Vogel), default by measurement.
7. **Contact hardening (PCSS).** Real, visible win on mid-scale sun shadows — penumbra
   that sharpens at contact is one of the things people point at when they say a game
   "looks current-gen". Built as a planned feature with its default tier decided by
   measurement (§4.1 sub-fork). Rev 2 wrongly killed this from an estimate.
8. **Contact occlusion (SSAO).** Restored from the old plan's L4.5 — rev 1/2 dropped it
   without stating a reason, which on inspection was a cost reflex, exactly what §1.2
   forbids. Depth-only hemisphere AO (no TAA, no motion vectors needed), applied to
   indirect light only. It is what stops flat-ambient scenes reading flat. Planned (S8c).
9. **Soft area-light shadows / colored translucent shadows.**
   Area-light softness is perceptually covered by PCSS; colored translucent shadows were
   excluded by design decision in the old plan (stands — a design call, not a cost
   call). Ray-traced penumbra is **out by standing decision**, §4.1-C. Not in this
   plan's stages.
10. **Indirect bounce (GI).** The biggest look item beyond this plan's scope — and under
    §1.4, the next project after it, not an indefinite deferral.

## 3. Ordering against the rest of the renderer — the plain answer

**Yes, HDR + tonemap must land before any shadow visual gate means anything.** Today
`mesh.frag` ends with inline Reinhard + `pow(1/2.2)` into an LDR swapchain-format target
(`PostProcess.cpp` creates the scene color in the swapchain's format, line ~112).
Shadowing a sun whose intensity is clamped by inline Reinhard produces exactly the flat,
gray-on-gray result the programmer would rightly reject. The current Linear plan of
record already orders ENG-135 (RGBA16F target) → ENG-82 (filmic tonemap) ahead of L0/L1
— **keep that order; it is correct and non-negotiable.** Bloom (ENG-83) stays in the
plan; like everything else it ships with a measured cost and a default decided at its
gate.

Sky (ENG-86) should also come **before** the CSM visual gate rather than after it, for
the §2 item 3 reason: the sky is where the hemisphere ambient's color comes from, and an
outdoor test scene without a sky can't be judged by eye. It is cheap (a dome/gradient
drawn depth-equal after opaque) and pairs with a one-uniform change to the ambient term.

## 4. The architectural forks

### 4.1 Sun shadows: stabilized CSM vs. virtual shadow maps vs. ray-traced

**Option A — stabilized cascaded shadow maps (recommended).**
- *What:* cascade count and resolution are **runtime knobs (1–4+ cascades, 1024–4096,
  D16/D32)**; the provisional default is **4 × 2048² D32** — chosen on merit (4 is the
  industry-standard point where log-blend splits keep near-field texel density without
  starving the far field; rev 2's drop to 3 was budget-driven and is reverted), and
  **confirmed or moved at S4's gate by measurement** at both resolutions. Split scheme:
  log/uniform blend, λ≈0.75 (the standard since the DX SDK article; MJP and every
  published implementation land here). Each cascade fit to a **bounding sphere** of its
  frustum slice (rotation-invariant extent) and **snapped to texel increments** in light
  space (no sub-texel crawl). Max-distance knob (provisional 80 m). Seams hidden by a
  dithered blend band over the last ~10% of each cascade. Filtering: comparison-sampler
  PCF, kernel knob (1-tap HW-bilinear / 3×3 / 5×5 / Vogel disk), default by measurement.
  Bias: slope-scaled raster bias in the depth-only pipeline + **normal-offset** at
  sample time, both auto-scaled by the cascade's world-texels-per-pixel; per-light
  overrides as escape hatches.
- *Cost to build here:* one depth-only pipeline (vertex-only MeshPass variant), one D32
  texture array, N extra CPU cull+submit passes reusing `Frustum::FromViewProjection`
  (Frustum.hpp:32) and the existing DrawItem extraction, new FrameConstants fields + 2
  new SRV slots in mesh.frag's binding set. Largest forced refactor: reusable non-camera
  depth submission (§7.1). 1–2 issues.
- *Runtime cost:* measured at the gate and published. For scale (estimate, not gate):
  depth passes of a ~50k-tri scene are rasterization-trivial; 9 comparison taps ×
  ~3.7M px ≈ 33M taps is comfortable on a 3070. The knobs exist because the same math at
  4K quadruples.
- *What it forecloses:* nothing. PCSS, SDSM tightening, even VSM later, all layer on top
  of or replace a working CSM without touching consumers.

**Option B — UE5-style virtual shadow maps: rejected on structural grounds; the
rejection survives rev 3.** Being explicit about which reasons were budget-dependent
(now void) and which are structural (stand):
- *Void (budget-flavored):* rev 2's "wrong direction for a 700-fps-baseline engine"
  framing. Struck.
- *Structural, standing:* (1) **build cost** — page tables, feedback-driven allocation,
  per-page caching/invalidation, HZB page culling is a multi-month subsystem
  ([Epic's docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine));
  (2) **it presumes a Nanite-class geometry pipeline** — since UE 5.6 VSM requires
  Nanite enabled at the project level, and Epic took until 5.7 to make non-Nanite meshes
  "competitive with the old CSM path"
  ([forum](https://forums.unrealengine.com/t/cannot-use-virtual-shadow-maps-in-non-nanite-projects/2663232),
  [StraySpark](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7))
  — Assisi's geometry pipeline is one arena and a compute cull; (3) **its page-management
  overhead is always-on regardless of scene content**, which fails the blank-scene /
  pay-for-what-you-place gate — and that gate survives rev 3 as an absolute (§1.2).
  VSM solves per-texel shadow density for open worlds this engine does not have.
  **High confidence.**
- *What would reopen it:* an actual open-world game on Assisi with kilometer view
  distances. Recorded as that trigger, nothing less.

**Option C — ray-traced shadows: rejected outright, by standing decision.**
Not deferred, not mapped, not a trigger — **out**. The programmer's direction, verbatim:
*"I very much don't want to implement ray tracing. It's not that good and it's expensive
as hell. I will not consider it and it's not needed for this engine just yet. We can make
beautiful games without RT, TAA, or anything like that."* This decision extends past RT
to the whole **temporal-technique family: TAA, temporal upscaling, and frame generation
are all out**, consistent with the standing no-upscaling/no-frame-gen constraint (§1.1).

The technical picture agrees with the decision and is recorded only so nobody reopens it
by accident: soft ray-traced shadows require a denoiser, denoisers require temporal
accumulation, and that requires motion vectors and TAA — none of which exist here, and
all of which are now permanently out of scope. Undenoised ray-query shadows are
perfectly sharp everywhere and look *worse* than PCF. RT would also need BVH
build/refit/compaction lifecycle management that the engine does not own (NVRHI exposes
the API; nothing uses it). So the cheap version looks bad and the good version requires
a subsystem family that has been ruled out. **No preconditions are recorded because none
are wanted.**

**Consequence worth stating once:** rejecting TAA costs this engine unusually little,
because it is a *forward* renderer. MSAA works natively in forward rendering and is
already implemented (PostProcess.hpp, device-clamped sample counts) — it is deferred
renderers that are pushed toward TAA because MSAA is prohibitive for them. So the
anti-aliasing story stays MSAA/FXAA with no gap where TAA would have been. Nothing else
in this plan depends on temporal accumulation: PCSS (S8b) is purely spatial, SSAO (S8c)
is specified depth-only/spatial, and the shadow-atlas caching in S7 is *reuse of a
rendered depth buffer*, not temporal image filtering — it has no ghosting or
accumulation semantics and is unrelated to this family despite both involving
"across frames."

**Sub-fork: sun filtering — PCF is the substrate; PCSS is a planned feature, restored.**
Fixed-kernel PCF is what id Tech ships and remains the default *substrate* (moment/
variance formats stay rejected for intrinsic light bleed —
[MJP](https://therealmjp.github.io/posts/shadow-maps/) documents 32-bit EVSM still
bleeding; that is a correctness argument, not a cost one, and stands). **PCSS
([NVIDIA](https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf),
[CHS variant](https://gamedev.net/tutorials/programming/graphics/contact-hardening-soft-shadows-made-fast-r4906))
is built as its own stage (S8b)** — rev 2 killed it from an estimate, which §1.2 now
forbids. What it genuinely costs: a blocker search (~16 taps) + variable-kernel filter
(16–32 taps) ≈ **2.5–4× the per-pixel cost of 3×3 PCF**, per light type it's enabled
for. What it buys: contact hardening — penumbra that sharpens where shadow meets caster
— which is among the most visible single "modern" cues a sun shadow can have. It ships
as a knob (off / sun-only / sun+locals); **its default tier is decided by the measured
number at its gate**, not here. Plausible outcome: default-on at High/Ultra, off at
Low/Medium — but that is a prediction to be checked, not a decision.

### 4.2 Local lights: the atlas, the importance cap, and caching as core

Everyone credible converges on one atlas for spot+point:
[DOOM (2016)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/)
— one 8k atlas, cached static depth + dynamic composite;
[DOOM Eternal](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy) kept
it; [Godot](https://docs.godotengine.org/en/3.1/tutorials/3d/lights_and_shadows.html) —
quadrant-subdivided atlas with per-frame size reassignment.

**The cache and the cap have distinct jobs** (§1.3):
- **Static-depth caching** (core efficiency, not optional): cached static depth per
  tile; copy-then-rasterize composite (a 512² D16 tile copy is 0.5 MB — bounded);
  invalidation inferred from transform ticks (§4.3). This is what makes sensible
  content fast by default — a placed, resting, shadowed light approaches zero
  steady-state render cost, so generous shadowed-light counts are cheap where it
  matters.
- **Top-N importance selection** (graceful-degradation backstop — *not* a target to
  design content against): each frame, score every shadowed light in range (screen
  coverage × distance × intensity); the top N hold tiles; the rest render unshadowed;
  hysteresis prevents boundary flicker. **Default N is set comfortably above typical
  sensible-scene light counts so it does not bind in normal authoring**; the §6.0
  numbers (8/16/32/64) are provisional starting points and the per-tier ceilings come
  from measurement at S6/S7's gates. The cap's own gate is the §1.2 no-cliff principle:
  when absurd input makes it bind, degradation must be smooth (proportional falloff in
  shadowed count and tile resolution), stable (no per-frame flicker as lights swap
  across the boundary), and correct — never a knee, never a crash. And binding is never
  silent: S7b surfaces it in the editor.

**The cap is fully exposed and configurable — never hidden, never hardcoded** (programmer's
direction: *"we need to be able to increase or decrease the cap, decide whether the feature
is enabled, or even decide per light type. Do not hide this feature."*). Concretely, S6
ships all of the following as first-class settings, not internal constants:

| Knob | Meaning |
|---|---|
| `shadowCapEnabled` | Master toggle for importance selection (see the honesty note below). |
| `shadowCapPoint` | Max simultaneously shadowed **point** lights. |
| `shadowCapSpot` | Max simultaneously shadowed **spot** lights. |
| `shadowPriority` (per light) | Author override: bias a light's importance score, or pin it as always-shadowed so a key light can never lose its shadow to the cap. |

**Per-type caps are better design than one global cap, not merely a preference.** A point
light costs **six** shadow renders (one per cube face) against a spot light's one, so a
single "8 lights" number means wildly different work depending on the mix — 8 spots is 8
renders, 8 points is 48. Splitting the cap by type makes the setting mean something
predictable, and lets an author who uses mostly spots raise their ceiling without paying
for the point-light worst case. This change is adopted into §6.0's tier table (the single
`cap N` row becomes per-type) and into S6's scope.

**Honesty note on the master toggle.** Turning the cap *off* removes the importance
*ordering*, not the physical limit — the atlas is a fixed-size texture, so with the cap
disabled lights are served tiles until the allocator runs out, and the remainder go
unshadowed in arrival order rather than importance order. That is strictly worse behaviour
under pressure, which is exactly why it is not the default; but it is the author's call to
make, it is documented as such in the setting's tooltip, and S7b's diagnostics keep
reporting what happened either way. The plan does not promise unlimited shadowed lights,
because no allocator can deliver them — it promises that the limit is visible, adjustable,
and never silently applied.

**The remaining fork is landing order, not content:**
- **Option A — one big issue:** everything at once. Too big for this repo's
  one-issue-per-conversation workflow; invalidation bugs (the stale-tile /
  missed-dirty class) would land with no working baseline to A/B against.
- **Option B — two issues, both core, back-to-back (recommended):**
  - **S6, atlas + cap:** tiered D16 atlas; power-of-two size-class allocator with
    rect+matrix records (the interface caching slots under unchanged); spot = 1 tile,
    point = 6 tiles with widened per-face FOV and PCF clamped to the tile interior;
    per-light `castsShadows`; importance cap with hysteresis; every held tile re-renders
    each frame (correct, bounded by N, not yet cheap); overflow demotes size classes.
  - **S7, caching:** cached static depth, composite, tick-inferred invalidation,
    per-light update-rate throttle for movers, atlas inspector in F11. **Lands
    immediately after S6; ENG-139 closes at S7, not S6.** S6 alone is an A/B checkpoint.

**Recommendation: B, S7 non-optional. High confidence.** Defaults: shadows ON per light
(correctness by default — a leaking light is a bug the author opts into); 512² default
face class (256² Low), demotion floor 128²; all confirmed at gates.

### 4.3 Mobility trichotomy: authored flag vs. inferred staticness

The old plan asserts `Static | Stationary | Movable` as the first thing built; ENG-80
blocks both shadow issues on it. Testing that claim against what shadows actually need:

- CSM does not need it: cascades re-render every frame regardless (scrolling cached
  cascades is VSM territory, rejected above).
- The atlas needs exactly one bit per caster: *"has this object moved since this tile
  was rendered?"* — and the engine **already computes that**. Transform is a tracked
  component with a per-entity change tick that `PropagateTransforms` dirty-skips against
  (SceneRenderer.cpp:189-192, `propagationTick` — verified). A tile's cached depth is
  valid iff no caster intersecting the light's volume has a last-moved tick newer than
  the tile's render tick. The inference is *more* correct than an authored flag: it
  cannot be mislabelled, and its failure mode (an object that moves after resting
  invalidates the tile a frame later) is invisible, versus the authored flag's failure
  mode (mislabelled Static object moves, shadow silently never updates) which the old
  plan itself admits is "visibly wrong lighting".
- The *Stationary* class existed for baked probe GI's door problem. Probe GI is not in
  this plan (though its position improves — §1.4). A three-way authored enum on every
  light and mesh renderer, serialized into every level, defaulting wrong for someone, is
  ceremony until a bake exists.

**Recommendation: cut ENG-80 as a shadow prerequisite.** Replace with a much smaller
issue: `castsShadows` on the three light components (LightComponents.hpp) and
`castShadows` on MeshRenderer. Mobility returns as an authored concept if and when baked
GI lands. Medium-high confidence; the caveat — tick-inference must be cheap per light
per frame — is bounded by the cap (a changed-entity set intersected against ≤N
tile-holding lights) and measured at S7's gate.

### 4.4 Interim ambient: flat constant vs. sky hemisphere + global IBL

Not in the current shadows milestone at all, and it should be (§2 item 3):

- **A — keep flat ambient until probe GI:** shadows land, interiors and shadowed
  exteriors go uniformly dark gray. Will not "look great".
- **B — sky-tinted hemisphere ambient when the sky lands (recommended, S5):** ambient
  becomes `lerp(groundColor, skyColor, N.y*0.5+0.5) * intensity` — two uniforms derived
  from the ENG-86 sky, replacing the flat `uFrame.ambient` product in mesh.frag:524-525.
  A few ALU per fragment; shadowed areas immediately read as sky-lit.
- **C — one global sky specular probe (split-sum IBL), now a planned stage (S8a), not
  "optional polish":** rev 1/2 hedged this as optional, which under §1.2 was another
  cost reflex — it improves the look of every metal and every rough surface in every
  scene, its sampling cost is small and measured at its gate, and OpenPBR just landed a
  material model with nothing to reflect. This **reopens the old plan's "IBL only after
  L4" decision** — deliberately: that ordering assumed L4 was imminent; it is not, and
  a global probe is upgrade-compatible with cluster-indexed reflection probes later
  (ENG-35 proper). Generated sky cubemap + GGX-prefiltered mips + split-sum LUT.

**Recommendation: B paired with ENG-86; C as the first post-shadow stage.** Medium-high
confidence on both.

## 5. Measurement: the instrument the whole plan is judged by

With budgets gone, **the A/B + clock-guard protocol is the only discipline left** — the
ledger (§1.2) is only as honest as the instrument. ENG-136 (S2) is accordingly **not
housekeeping; it is the stage every later stage is judged by**, and it should be treated
with that seriousness at its own gate. The old L0 (7 scenes, pinned clocks, ±0.05 ms,
×3 min-spec derate) is re-scoped:

- **Cut 4 of the 7 scenes.** thin-wall, enclosed-room, door-room, corridor exist solely
  to gate probe-GI and reflection stages. They return with ENG-137 (whose stock has
  risen — §1.4 — but which is still not this plan).
- **Keep blank / reference / stress**, re-specced to buildable reality: the repo's only
  committed mesh is `prim://cube`; `Materials.alvl` references a glTF not in the repo;
  skinned meshes don't import. So: register `CreateUnitSphereMesh` /
  `CreateUnitCylinderMesh` (Geometry/DefaultMeshes.hpp) as `prim://sphere` /
  `prim://cylinder`, and build the reference scene from primitives — ~50k visible
  triangles, 8 point + 4 spot + 1 sun, 4 transform-animated movers (not skinned —
  unsupported). **The reference scene has a dual role: it is also the published
  definition of "reasonable map design".** If the engine promises that sensible content
  is fast by default (§1.2 principle 3), it owes authors a statement of what sensible
  means — and the reference scene *is* that statement: this light count, this caster
  density, this triangle budget, these material counts, running at these published
  numbers on this hardware. A contract, not merely a fixture; it costs nothing extra
  and turns a test asset into documentation.
  **`Lights.alvl` joins the suite as the graceful-degradation test, not a target to
  meet**: 270 points at castsShadows-ON defaults is deliberately absurd input, and its
  gate is that the system falls off smoothly — no cliff, no crash, no correctness
  break, no cap-boundary flicker; just proportional falloff in shadowed-light count and
  tile resolution, with zero authoring changes. That gate protects the developer
  experience better than any budget number would.
- **Measurement ladder** (all native, no upscaling anywhere):
  1. **Dev-native:** A/B deltas at **both 1440p and 1080p** on the 3070 — every ledger
     entry carries both numbers.
  2. **Steam-modal mapping:** a 3070 at 1440p is ≈1.23× harder than the modal 3060 at
     1080p (1.78× pixels ÷ ≈1.45× GPU, public aggregate benchmarks) — so dev-native
     numbers carry ~25% margin over the modal rig with no paper games.
  3. **Floor mapping:** a GTX 1060 at 1080p is ≈1.6× harder than the desk at 1440p
     (pixel ratio 0.56 ÷ perf ratio ≈0.35). Low-tier ledger entries are desk-at-1080p
     numbers ×1.6, **provisional until real floor hardware exists**.
- **Protocol** (unchanged, now elevated): A/B within one session, interleaved runs,
  never absolutes across sessions; `Render::GpuTelemetry` (NVML core clock +
  temperature) recorded alongside — windows where the clock drifted >5% or thermals
  climbed are discarded; median over ≥500 frames, median + IQR published, ±0.1 ms
  instrument tolerance unpinned. **The blank-scene gate runs on every stage merge** —
  at a 1.3 ms baseline, a 0.05 ms leak is 4% of the frame.
- **Per-pass GPU timing policy**, given that nvrhi's `beginTimerQuery`/`endTimerQuery`
  force `endRenderPass()` (verified in nvrhi's vulkan-queries.cpp): per-pass timers live
  behind a **profiling toggle** in the F11 panel, default off. Mitigations: (a) shadow
  passes are separate render passes by construction, so bracketing them adds no break
  that wasn't already there — shadow timers can be effectively always-on; (b)
  whole-frame `GetLastGpuFrameTimeMs` stays the primary ledger number via A/B.

ENG-136 is currently **In Progress** — this re-scope should be agreed and the issue
edited *before* it completes.

## 6. Recommended plan — dependency-ordered stages

### 6.0 Quality tiers: the knob space (all defaults provisional until measured)

The knobs below are **built as runtime settings in S4/S6** (cascade count / resolution /
format, shadow distance, PCF kernel, PCSS, atlas size, importance cap N, face class) and
each tier's defaults are **confirmed or moved at the owning stage's gate from the
measured ledger**. The table is the starting map of the space, not a promise. Tier
defaults to Medium; auto-pick by VRAM at first run is an open question (§9).

| | **Low** (≤6 GB: GTX 1060/1650, RX 580 · 1080p) | **Medium — default** (8 GB modal: 3060/4060L) | **High** (3070+ · 1440p) | **Ultra** (12 GB+ · 1440p/4K) |
|---|---|---|---|---|
| Cascades | 3 × 1024² D16 | 4 × 2048² D32 | 4 × 2048² D32 | 4 × 4096² D32 |
| Sun distance | 40 m | 80 m | 80 m | 100 m |
| Sun filter | 1-tap HW-bilinear PCF | 3×3 PCF | 5×5 / Vogel; PCSS candidate | Vogel; PCSS candidate |
| Atlas | 2048² D16 | 4096² D16 | 4096² D16 | 8192² D16 |
| Shadowed **spot** cap (1 render each) | 8 | 16 | 32 | 64 |
| Shadowed **point** cap (6 renders each) | 2 | 4 | 8 | 16 |
| Default face class | 256² | 512² | 512² | 512² |
| Shadow memory | ≈14 MB | ≈80 MB | ≈96 MB | ≈384 MiB |

All cap numbers are provisional starting points, user-adjustable at runtime, and raised or
lowered per tier at S6's gate by measurement. Point caps are deliberately ~¼ the spot caps
because each point light is six shadow renders — see §4.2. A tier is a *preset over the
knobs*, never a lock on them: any setting in this table can be overridden by the author,
and the tier simply stops being the one selected.

Every stage: scope → definition of done (DoD) → visual gate (VG) → performance gate
(PG). **All PGs are the §1.2 form:** cost measured and published in the issue at
1080p+1440p (A/B, NVML-guarded), justified against what it buys at the approval gate, no
regression of the previous stage's feature-off baseline, blank-scene gate green. Sizes
in points per structured_workflow.txt (one issue ≈ one conversation).

**S1. HDR target + tonemap (= ENG-135 → ENG-82, unchanged).**
Scope as written in the issues (the old "≤0.5 ms L2 budget" language in ENG-135's DoD
becomes the ledger form). Size: 2 + 2. *Blocks every visual gate below.* ENG-83 bloom
follows; its default (on/off, tier) is decided from its measured number.

**S2. L0 right-sized — the instrument (= ENG-136, re-scoped per §5, elevated).**
Scope: blank/reference/stress scenes from primitives + `Lights.alvl` adopted as the
graceful-degradation test; `prim://sphere`/`prim://cylinder` registration; the A/B +
NVML-guard protocol implemented as a repeatable capture mode (not a manual ritual);
per-pass timers behind a profiling toggle; baselines recorded at 1440p **and** 1080p.
The reference scene ships with its dual role stated (§5): it is the published contract
for "reasonable map design" — its content stats and measured numbers are documentation,
not just a fixture. DoD: scenes load from a clean clone; a one-command measurement run
produces the ledger numbers; baselines committed to the issue; the reference scene's
contract stats recorded alongside them. Size: 3.

**S3. Shadow flags (replaces ENG-80).**
Scope: `castsShadows` (lights, default ON) + `castShadows` (MeshRenderer, default ON) as
reflected AFIELD fields; plumbed into the light upload path (LightingSystem.cpp) and
DrawItem extraction. DoD: round-trips through save/load, visible in inspector, readable
by the shadow passes. Size: 1.

**S4. Directional CSM (= ENG-33, re-specced).**
Scope: depth-only pipeline (vertex-only MeshPass variant; front/back-face culling choice
measured, not assumed); cascade array with **runtime count/resolution/format knobs (the
§6.0 column is built here, including the settings block, §7.8)**; sphere-fit +
texel-snapped stable cascades, λ≈0.75 splits, distance knob; slope-scaled +
normal-offset bias auto-scaled per cascade; PCF kernel knob; dithered seam blend;
cascade-colorizer debug view in F11; per-cascade CPU cull via
`Frustum::FromViewProjection`. Multi-view GPU cull deferred (small views are CPU-cheap;
S8d if profiling says otherwise). DoD: stable under strafe *and* rotation on the
reference scene; no acne/peter-panning at defaults; seams invisible on flat-albedo
content; `castsShadows` off ⇒ passes absent from capture. VG: capture review on
reference at 3 sun angles, at two tier settings minimum. PG: ledger entries for each
knob step (cascade count 3 vs 4, kernel sizes, resolutions) so the tier defaults are
*chosen from data at this gate*. Size: 5 (the current 2-point estimate is not credible).

**S5. Sky + hemisphere ambient + the indirect-lighting seam (= ENG-86 + new small issue,
per §4.4-B and §10).**
Scope: ENG-86 as written (dome/gradient, depth-equal after opaque, HDR by construction)
plus hemisphere ambient. **Critically, the ambient term is introduced as an *indirect
lighting provider*, not as a better constant** — see §10. `mesh.frag` currently computes
`ambient = uFrame.ambient.rgb * uFrame.ambient.w` inline (line 524); that one expression
*is* the engine's entire indirect-lighting term. S5 replaces it with a call through a
narrow interface — "what indirect radiance arrives at this point, from this normal?" —
whose hemisphere implementation is the first provider. This costs nothing extra here (the
line is being rewritten regardless) and is the whole insurance policy that keeps baked,
hybrid, and fully-dynamic GI available later without touching materials or assets.
DoD: outdoor reference shadows read sky-lit, not black; indoor unchanged via ambient
override; **the indirect term is reached through the provider interface, with the
hemisphere provider swappable at one call site**. PG: ledger. Size: 2 + 1. *Before the
atlas so its visual gates are judged in a lit world.*

**S6. Local-light shadow atlas + importance cap (= ENG-139 first half, §4.2-B).**
Scope: tiered D16 atlas; size-class allocator with rect+matrix records (extends
`PointLightGPU`/`SpotLightGPU` — mirrored in cluster_cull.comp and mesh.frag); spot 1
face / point 6 faces, widened-FOV borders, interior-clamped PCF; **top-N importance
selection with hysteresis** (§1.3 — arithmetic-mandated); per-tile bias auto-scaled;
overflow demotes size classes. **The cap ships fully exposed** (§4.2): master enable
toggle, separate point and spot caps, and a per-light `shadowPriority` override including
an always-shadowed pin — all runtime-adjustable, persisted, and surfaced in the F11 panel
and the light inspector, never internal constants. Alpha-tested casters inside this issue
*iff* ENG-84 has landed, else its named fast-follow. DoD: point light occluded in all 6
directions incl. across seams; no placed light leaks at defaults; demoted faces acceptable
at typical range; **the cap does not bind on the reference scene at default N** (sensible
content never sees it); **every cap knob round-trips through settings and takes effect
without a restart**, including the disabled case (which falls back to allocator-order
service, documented as such); a light pinned by `shadowPriority` keeps its shadow under
cap pressure that drops its neighbours; **`Lights.alvl` degrades gracefully at every tier
with zero authoring changes** — a light-count sweep (e.g. 8 → 270 shadowed lights) shows
smooth, monotonic cost growth with no knee, no crash, no correctness break, and no
cap-boundary flicker (the §1.2 no-cliff principle, made a test). VG: capture review on
reference + stress.
PG: ledger, explicitly labeled pre-caching (steady-state honesty arrives with S7);
the sweep curve published in the issue. Size: 5.

**S7. Atlas caching (= ENG-139 second half — core, lands immediately after S6).**
Scope: cached static depth per tile; copy-then-rasterize composite; tick-inferred
invalidation (§4.3); per-light update-rate throttle; atlas inspector in F11. DoD: a
resting shadowed light with no movers in range shows **zero per-frame shadow render cost
in capture** (the §1.2 pay-for-what-you-place gate, applied per light); a mover entering
range dirties only the faces whose frustum contains it; toggle off reproduces S6 exactly
(A/B). PG: ledger — including the steady-state vs. S6 delta (the number that justifies
caching's existence) and the measured practical ceiling for N per tier, which sets the
§6.0 defaults. **ENG-139 closes here.** Size: 4.

**S7b. Editor shadow diagnostics (new issue — leaving the fast path is never silent).**
Depends only on S6's data (cap and allocator state) plus S7's cache state where present;
schedule any time after S6, ideally before the atlas pair is called finished. The
programmer's philosophy answers rev 2's dangling "silent top-N?" question: if a
developer should not have to profile to stay fast, the editor owes them authoring-time
feedback the moment content leaves the fast path. Scope: a diagnostics readout (F11
panel section + per-light inspector line): *this light exceeded the shadowed-light cap
and renders unshadowed*; *this light's tiles were demoted a size class*; *N of M
shadowed lights active*; *caster count per sun cascade*. Silent demotion is mysterious
quality loss the author cannot act on; visible demotion is a thirty-second fix. DoD:
each condition above is visibly reported against a scene constructed to trigger it;
zero cost when the panel is closed (blank-scene gate applies to diagnostics too).
Size: 2.

**S8. Planned features, sequenced by look-per-effort (defaults decided at their gates):**
- **S8a. Global sky IBL probe** (§4.4-C; subset of ENG-35): generated sky cubemap, GGX
  prefilter, split-sum LUT; gives OpenPBR materials an environment. Size 3.
- **S8b. PCSS** (§4.1 sub-fork, restored): blocker search + variable penumbra; knob
  off / sun / sun+locals; default tier from the measured number. Size 3.
- **S8c. SSAO** (restored from old plan L4.5): depth-only hemisphere AO + spatial blur,
  applied to indirect light only (AO on direct light is wrong and looks it); no TAA
  dependency. Size 3.
- **S8d. Multi-view GPU culling for shadow views** (extend MeshCuller): infrastructure,
  built when the ledger shows CPU shadow submit mattering, not before. Size 3.

**S9. Geometric specular antialiasing (small, independent — schedulable any time).**
*This is a direct consequence of the no-TAA decision (§4.1-C) and belongs in this plan
because that decision was made here.* MSAA resolves **edge** aliasing but not **shading**
aliasing: a normal-mapped glossy surface viewed at a glancing angle or at distance
produces specular highlights smaller than a pixel, which flicker frame to frame as they
pop in and out. It is highly visible in motion — precisely the failure the programmer
rejects TAA for — and TAA is what most engines quietly use to hide it. The non-temporal
fix is standard: estimate the per-pixel normal variance from screen-space derivatives of
the shading normal, and widen the material's roughness so the specular lobe cannot be
narrower than the pixel footprint (Kaplanyan-style normal-variance / Frostbite's
specular AA; shipped in Unreal as the "Geometric Specular AA" material toggle).
Scope: a few lines in `mesh.frag`'s specular path plus a per-material enable and
variance-clamp knob. This is newly urgent because ENG-133 just landed OpenPBR's
metallic/roughness base layer — shiny metal with normal maps is exactly the sparkle case.
DoD: a rotating normal-mapped metallic sphere and a glancing-angle metal floor show no
frame-to-frame highlight flicker at default settings; knob off reproduces current
behaviour exactly; cost is negligible and published like everything else. Size: 1.

S1–S7 deliver the target look's load-bearing items (§2, 1–6) and the §1.4 "dynamic where
they were baked" claim for direct lighting; S7b delivers the fast-by-default developer
experience its philosophy promises. S8a–c are look features with their own gates; S8d
is measurement-driven infrastructure.

## 7. Refactors the recommendation forces (invasiveness ledger)

1. **Reusable depth-only scene submission** — the big one. `DrawScene`/SceneRenderer are
   married to one camera view (SceneRenderer.cpp:194-227). Needed: "render scene depth
   from view X into framebuffer Y with cull frustum Z" callable N times per frame
   (cascades + atlas tiles). Shape: a `ShadowPass` in modules/Render owning the depth
   pipeline + per-view submit, fed by the same DrawItem extraction. Moderate: new files
   + a seam in SceneRenderer; no existing behavior changes. (Churn is affordable per
   repo convention.)
2. **Light GPU struct layout change** (ClusterGrid.hpp:35-57 + cluster_cull.comp +
   mesh.frag): shadow record index/params on Point/Spot/Dir structs. Small but
   three-places-in-lockstep; the static_asserts enforce the discipline.
3. **mesh.frag binding growth**: shadow record buffer + cascade array + atlas texture +
   comparison sampler ⇒ new slots in MeshPass's one binding set and layout. Small.
4. **FrameConstants growth**: cascade matrices/splits, shadow params. Small.
5. **PostProcess/HDR restructure**: owned by ENG-135. Medium, already planned.
6. **Component codegen additions** (S3): trivial via ACOMP/AFIELD.
7. **Per-entity last-moved tick exposure** (S7): PropagateTransforms already computes
   change sets; exposing a last-moved tick is small; the per-light intersection query is
   bounded by the cap N. Small-medium.
8. **Quality-tier settings block** (§6.0): a small reflected settings struct the shadow
   systems read knobs from, editable in F11, serialized per user not per level. Small;
   must exist by S4.
9. **No test target in modules/Render** — fix *in* S4: cascade fit/snap/split math, the
   atlas allocator, and importance-cap scoring are exactly the pure-CPU logic doctest is
   for; lands test-first per structured_workflow.txt.

Explicitly *not* needed: reversed-Z (engine is standard-Z, `GLM_FORCE_DEPTH_ZERO_TO_ONE`,
Camera.cpp:17 — ortho cascades and short-range D16 locals don't need it),
deferred/visibility-buffer restructure, TAA and the whole temporal family (out by
standing decision, §4.1-C; MSAA covers AA natively in a forward renderer), ray tracing
and its BVH lifecycle (out by standing decision), depth prepass, and — hard
constraint — any upscaler or frame generation, anywhere.

## 7b. The GI direction — decided, and why it is recorded here

GI is **not** in this plan's scope. It is recorded here because the decision was reached
while planning shadows, it constrains one line of S5, and writing it down is what stops it
being re-argued from scratch later.

**Decision: hybrid — precomputed transfer, dynamically relit.** Bake the part that depends
on *geometry* (how much of surface B does surface A see) and leave the part that depends on
*lights* live. The sun moves, lights change colour and intensity, and the bounce follows —
at a small runtime combine rather than a trace. This is the Precomputed Radiance Transfer
family; Enlighten shipped it in Battlefield 3/4 and Unity 5 on 2011 console hardware, which
is the evidence for its cost class. The pieces:

- Baked transfer/probe grid over static geometry, relightable at runtime.
- **Dynamic objects sample the grid but do not contribute to it.** A player picks up correct
  bounce colour walking through a red room; nobody notices they aren't casting bounce onto
  the walls. This removes any need to re-solve when things move.
- **Time-sliced probe updates** for the day/night cycle: re-bake a few probes per frame and
  let the grid catch up over a second or two. The sun takes minutes to move, so the lag is
  imperceptible. This is what makes a moving sun compatible with baking at all.
- Cheap screen-space AO (S8c) for the contact-scale detail a coarse grid cannot resolve.

**Accepted cost of this choice, stated plainly:** moving geometry does not re-block bounce
(a door opens, light does not spill through until something re-bakes); destruction does not
update GI; a bake step returns, with build times; levels cannot be fully procedural. These
are exactly what a fully-dynamic solution's 3–13 ms buys, and they are capabilities this
engine's stated content does not need.

**The door problem is the known sharp edge.** Doors are ubiquitous and are precisely the case
precomputed transfer handles worst. The fix is a small authored category of objects that
trigger a local re-bake on state change — which is why **the mobility flag returns for GI**,
having been cut from shadows (§4.3). The distinction is real and worth restating: shadows
need *"has this moved"*, which the engine infers from transform ticks; baking needs *"will
this ever move"*, which only an author knows. The four L0 scenes cut in §5 — thin-wall,
enclosed-room, **door-room**, corridor — are the gates for exactly these failure modes and
travel with this work.

**Architecture: one interface, several providers — explicitly NOT Unity's pipeline split.**
Unity's Built-in/URP/HDRP division fragmented their asset ecosystem, made migration
impractical, and forced an irreversible choice at project creation. Copying that shape would
import the problem with the flexibility. It is also unnecessary, because baked, hybrid, and
fully-dynamic GI all answer the *same question*: how much indirect radiance arrives here,
from this direction. Put that question behind an interface and each becomes a **provider**:

| Provider | Answers with |
|---|---|
| Hemisphere (S5, first) | Sky/ground gradient by normal |
| Baked | Lightmap or probe lookup |
| **Hybrid (chosen direction)** | Probe lookup relit at runtime from current lights |
| Fully dynamic | Traced/voxel/cascade result |

Materials, shaders, assets and the rest of the renderer are identical across all four. A
project — or a level — can swap providers without invalidating content. **The seam costs
nothing to install because S5 is rewriting that exact line anyway**; that is the entire
insurance policy, and it is the only GI-related work in this plan.

Note the three are not equal-sized projects: baked and hybrid share nearly everything (bake
step, probe storage, sampling path), so hybrid is realistically "baked, plus runtime
relighting." Fully dynamic is the genuine outlier and a separate project if ever wanted.

**Radiance Cascades: assessed and set aside (2026-08-15), with reasons, so it is not
re-litigated.** Sannikov's technique is excellent in 2D and ships in Path of Exile 2. The
3D formulation was an open problem until [Split Radiance Cascades](https://arxiv.org/abs/2607.20384)
(Freeman & Sannikov, 22 Jul 2026) solved the storage explosion with sparse hashmap probes
and ray splitting. Measured in that paper: **13.2 ms single-frame on an RTX 3080 Laptop, of
which 6.1 ms is 1-spp ray tracing**; the quality figures use 10 frames of temporal
accumulation. It therefore fails two standing decisions at once (ray tracing, §4.1-C; and
for its best results, temporal accumulation) and costs ~10× this engine's entire current
frame. Its authors acknowledge light leaking below probe spacing, inability to resolve sharp
mirror reflections, and interpolation bias.

Worth noting for balance: its distinguishing feature — good results *without* temporal
accumulation — is genuinely aligned with this engine's philosophy, and the paper usefully
**prices that constraint**: doing GI honestly every frame currently costs ~13 ms, which is
why everyone else accumulates temporally. That is an argument for precomputation, not
against the principle.

**No trigger is recorded for revisiting it, because the gap is structural rather than an
implementation gap.** Baked costs ~0.1 ms because the transport already happened offline;
any real-time solution must do transport every frame. Optimisation and hardware move dynamic
GI from "very expensive" to "affordable" — never to "free." If Assisi ever wants
destructible or procedural lighting, that is a *capability* decision, and the provider seam
is how it arrives.

## 8. Diff against the current Linear plan of record, issue by issue

| Issue | Current | Proposed | Why |
|---|---|---|---|
| ENG-133/134/138 (OpenPBR) | steps 1 | **keep** | done/in flight; untouched |
| ENG-84 masked alpha | step 2, blocker of L3 | **keep position, demote to soft blocker** | gates only the alpha-tested caster variant; committed content has zero cutout materials. Land as scheduled; don't stall S6 |
| ENG-135 → ENG-82 (HDR/tonemap) | step 3 | **keep; hard prerequisite of all shadow visual gates; DoD's fixed-ms budget language → ledger form** | §3, §1.2 |
| ENG-83 bloom | optional, "cut freely" | **keep; ships with measured cost; default decided at its gate** | §1.2 |
| ENG-136 L0 | step 4, 7 scenes, In Progress | **re-scope now and elevate**: 3 scenes + Lights.alvl, primitives, repeatable A/B+NVML capture mode, 1440p+1080p baselines — it is the instrument every later gate depends on | §5 — edit before it completes |
| ENG-80 mobility | step 4, blocker of L1/L3 | **cut as prerequisite; shrink to S3 shadow flags; mobility returns with baked GI** | §4.3 |
| ENG-33 CSM | step 4, 2 pts | **keep, re-spec per S4: knobs core, provisional 4×2048 default confirmed by measurement, ~5 pts, drop ENG-80 dependency** | §4.1, §6.0 |
| ENG-86 sky | step 5 (after CSM) | **move before atlas, pair with new hemisphere-ambient issue** | §4.4-B |
| ENG-139 atlas | step 6, one issue, mobility-blocked | **split into S6 (atlas+cap) + S7 (caching); both core; closes at S7; drop ENG-80 dependency** | §4.2 — cap and cache mandated by arithmetic, not budget |
| *(new)* shadow flags | — | **add** (S3, 1 pt) | §4.3 |
| *(new)* hemisphere ambient + indirect provider seam | — | **add** (S5, 1 pt) | §4.4, §7b — the seam is the whole GI insurance policy and costs nothing here |
| *(new)* quality-tier settings block | — | **add or fold into S4** (§7.8) | §6.0 core scope |
| *(new)* editor shadow diagnostics | — | **add** (S7b, 2 pts, after S6) | §1.2 principle 3 — leaving the fast path is never silent |
| *(new)* sun/local PCSS | — (rev 2 had killed it) | **add as planned feature (S8b); default tier by measurement** | §1.2, §4.1 |
| *(new)* SSAO | old plan L4.5; dropped silently in rev 1/2 | **restore as planned feature (S8c)** | §2 item 8 — the drop was a cost reflex |
| *(new)* geometric specular AA | — | **add** (S9, 1 pt, schedulable any time) | consequence of the no-TAA decision; MSAA does not fix shading aliasing, and OpenPBR's new metallic/roughness makes it visible now |
| ENG-35 IBL | deferred behind L4 | **carve out "global sky probe" as planned S8a; full cluster-indexed probes stay ENG-35 proper** | §4.4-C |
| ENG-137 probe GI | deferred indefinitely | **keep out of this plan; re-position as "next major project after S7"; record the chosen direction as hybrid/precomputed-transfer per §7b** — the 4 cut L0 scenes and the returning mobility flag travel with it | §1.4, §7b |
| ENG-85 blended pass | deferred | **keep deferred** | unrelated to shadows |

Out-of-scope list (transmission, subsurface, thin-film, anisotropy, OIT): **unchanged —
checked, these were design decisions in the milestone description, not cost exclusions
in disguise.** The cost-exclusions-in-disguise found and reversed: PCSS (rev 2), SSAO
(rev 1/2's silent drop), sky-IBL's "optional" hedge, and the 3-cascade default. Ray
tracing: re-recorded as a mapped future path with named preconditions (§4.1-C).
**Upscaling/frame generation: out of scope by hard constraint, not oversight.**

## 9. Open questions (flagged, not papered over)

1. **Floor-tier hardware.** The ×1.6 mapping (§5) derives from aggregate benchmark
   ratios, not measurement; every Low-tier ledger entry is provisional until a
   1060/1650-class card physically exists to test on.
2. **Who arbitrates "justified against what it buys"?** §1.2 answers "the programmer at
   the approval gate" — but a convention is worth setting early for *default* decisions:
   e.g. "a default-on feature should be visible in a blind A/B screenshot at its cost."
   Proposed, not decided.
3. **Tier selection UX**: auto-pick by VRAM/GPU name at first run vs. default Medium +
   manual. Small, but S4's settings block needs the answer.
   *(The former Q4 — silent top-N vs. editor indicator — is resolved by the
   fast-by-default philosophy: not silent. It is now the S7b deliverable.)*
4. **Front-face vs back-face culling** in the shadow pass interacts with normal-offset
   bias and thin geometry (a cube-built world is all thin walls); resolve empirically
   in S4.
5. **How far the mover-throttle and cap hysteresis can go before visible pop** — needs
   eyes, not analysis; S6/S7 gates.
6. **S8 internal order** (IBL vs PCSS vs SSAO after S7) — look-per-effort suggests
   S8a → S8c → S8b, but this is the programmer's taste call with the ledger in hand.
7. **NVML clock-guard thresholds** (§5) are educated guesses; S2 should validate them by
   measuring the same workload twice with a deliberately heated GPU.
8. **What counts as "typical sensible-scene light counts"** when setting default N
   (§4.2)? The reference scene's contract (§5) proposes the answer — default N should
   comfortably exceed its shadowed-light count — but the multiplier (2×? 4×?) is a
   judgment to make at S6's gate with the sweep curve in hand.

## Sources

- [UE5 Virtual Shadow Maps docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine) · [VSM requires Nanite (5.6+)](https://forums.unrealengine.com/t/cannot-use-virtual-shadow-maps-in-non-nanite-projects/2663232) · [VSM non-Nanite cost, 5.7](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7) · [VSM in Fortnite](https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4)
- [DOOM 2016 graphics study (atlas + static caching)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/) · [DOOM Eternal graphics study](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy)
- [Godot shadow atlas (quadrants, per-frame size reassignment)](https://docs.godotengine.org/en/3.1/tutorials/3d/lights_and_shadows.html)
- [MJP, "A Sampling of Shadow Techniques" (stable cascades, EVSM bleed)](https://therealmjp.github.io/posts/shadow-maps/) · [Microsoft, Cascaded Shadow Maps (splits, snapping)](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps) · [Tardif, CSM with soft shadows](https://alextardif.com/shadowmapping.html)
- [NVIDIA PCSS](https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf) · [Contact-hardening soft shadows](https://gamedev.net/tutorials/programming/graphics/contact-hardening-soft-shadows-made-fast-r4906)
- Steam Hardware Survey, July 2026 (GPU/resolution/VRAM shares as relayed by the programmer); GPU perf ratios (3070≈1.45× 3060, ≈2.9× GTX 1060) from public aggregate benchmarks — approximate, flagged in §9.1.
