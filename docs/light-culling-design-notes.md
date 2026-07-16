# Light culling & shadows — design notes

Captured 2026-07-15. **One piece is built** — the froxel per-cluster cap fix
described in "What was just fixed" below. **Everything after that is deferred**,
recorded here so the current clustered-forward light path isn't mistaken for the
scalable endpoint, and so the cost-vs-correctness split is understood rather than
re-litigated each time a dense-light scene misbehaves.

## What was just fixed (built)

Clustered ("froxel") forward lighting culls lights per-froxel in
`cluster_cull.comp` against a 16×9×24 view-space grid. It used to collect each
froxel's intersecting lights into a fixed 64-entry local scratch array and
**silently drop everything past the 64th** (`MAX_LIGHTS_PER_CLUSTER`).

A dense scene (`Lights.alvl`: 270 point + 29 spot lights) overflowed that cap in
its large, distant, wide-FOV froxels — each such froxel's AABB overlaps far more
than 64 of the radius-7 light spheres. The dropped lights showed up as lights
**blinking in and out along visible froxel borders**, worse the farther you got
from the lights and the higher the FOV (both make froxels physically larger, so
each overlaps more lights); fine up close and at low FOV. The froxel *geometry
and assignment math were correct* — a conservative view-space AABB cannot produce
false negatives — the only defect was the arbitrary truncation.

The fix removes the per-cluster cap entirely: the cull now counts intersections,
atomically reserves that many slots, then re-tests and writes indices straight
into the shared flat list (two passes, no local scratch array). The only bound
left is the shared index buffer, bumped 65536 → 262144 per light type and
hard-clamped so a reservation landing past the end degrades gracefully instead of
writing out of bounds. This is safe, not "unbounded": a cluster is still bounded
by the total uploaded light count (`kMaxPointLights = 1024`), and the buffer by
its capacity.

Touched: `cluster_cull.comp` (two-pass rewrite), `cube_min.frag`
(`kSpotIndexBase`), `ClusterGrid.hpp` (`kMaxLightIndices`) — the three values
must stay in sync.

## Two axes, kept separate

The single most useful thing to hold onto: **culling is for cost, shadows are for
correctness.** They solve different problems and must not be conflated ("cull
then shadow-map the remainder" quietly does exactly that, and it doesn't scale).

- **Cost** — how few lights and pixels do we do expensive work for?
- **Correctness** — does light correctly *not* pass through walls?

Removing a light by culling is strictly better than shadowing it: a culled light
costs nothing and can't leak, so **good culling shrinks the shadow burden.**

## The cost pipeline (deferred, in dependency order)

Coarse world-space → fine view-space → per-pixel. Each stage narrows what the
next one has to chew on.

1. **World chunks (Minecraft-style).** A uniform 3-D grid of fixed-size world
   cells — *not* portals or BSP cells. Bucket each light into its chunk(s); each
   frame gather only lights from chunks near the camera / overlapping the frustum
   (frustum-cull a whole chunk's light list in one AABB test). This makes the
   candidate-light gather *O(lights near the camera)* instead of *O(all lights in
   the world)* — the real path to thousands of lights in a large streamed world.
   A chunk is also the natural **streaming/residency unit**, so lights load and
   unload with their chunk; this stage lands with the asset-streaming work rather
   than before it. It does **not** understand walls (see "The sealed-room case").

2. **Frustum pre-cull (sphere-of-influence).** For each candidate light, test its
   sphere of influence (center + radius) against the 6 camera frustum planes;
   drop the ones that miss. This must be a **sphere** test, not a point test:
   a point test wrongly drops lights whose center is off-screen but whose radius
   reaches into the view (edge spill), producing dark screen borders. Sphere vs
   point is the same 6 plane evaluations plus one subtract each — it's free, so
   there's no "basic vs sphere" trade-off; the sphere form *is* the correct basic
   cull. Note this frees the 1024-light upload cap from off-screen lights too.
   Once a light's sphere passes the frustum test there is nothing further to cull
   at the coarse level — "sphere intersects frustum" is already the complete
   relevance criterion; the froxel cull does the fine-grained narrowing.

3. **Froxel cull (built).** The existing per-tile `sphereVsAABB` assignment. This
   *is* the fine-grained sphere-of-influence work — a separate "sphere culling"
   stage on top would find nothing it doesn't already handle.

4. **Depth pre-pass.** None exists today; the mesh pass is single-pass forward
   with depth test + write on, so occluded fragments are only skipped by hardware
   early-Z when a nearer fragment happened to write depth first (draw-order
   dependent — the `DrawItem` front-to-back sort helps but doesn't guarantee it).
   Add a cheap depth-only pass first, then run the lit pass depth-equal so the
   heavy (potentially hundreds-of-lights) fragment shader runs **exactly once per
   visible pixel**, never for hidden surfaces. This is what makes dense clustered
   forward affordable, and it's orthogonal to which lights survive culling.

The lever to reduce per-pixel *light count* (as opposed to pixel count) is finer
froxels — more Z / X-Y slices, each holding fewer lights — not re-introducing a
truncation cap. The cap "bounded cost by corrupting the image," which is the bug
we just removed.

## The correctness axis: shadows

Forward shading has no occlusion between a light and a surface. An unshadowed
light therefore **leaks through walls**. Shadows are the only fix.

You cannot shadow-map every light — point lights need 6 cube faces each, and every
shadow map re-renders scene geometry from the light's point of view. So shadows
must be a **budget**: only the top-N most important surviving lights (nearest,
brightest, largest screen footprint) get shadow maps, atlas-managed, and
**cached/baked for static lights** (a light and geometry that don't move → render
its map once, reuse it). Everything below the budget stays unshadowed.

Shadows are for lights that both reach the view **and** are occluded by *in-view*
geometry — a pillar shadowing an in-room lamp. Note also that HZB / screen-space
occlusion culling does not help the "light behind a wall" case: the interior wall
is itself visible, so there's nothing to occlusion-cull; the wall just needs to
*shadow* the light.

## The sealed-room case (worked example)

Camera inside a closed box, no lights inside, a bunch of lights just outside.

- **Frustum cull does not remove them.** The frustum is a geometric pyramid from
  the near plane to the far plane; it passes straight *through* the box walls and
  has no idea they exist. Any outside light whose sphere pokes into that pyramid
  survives.
- **Uniform chunks do not remove them either.** A chunk boundary is not a wall;
  the nearby-outside lights sit in the same or an adjacent chunk to the camera.
- Worse, without shadows those lights **leak through the wall** and wrongly light
  the interior.

So this case is a *shadow* problem, full stop — shadows occlude the outside lights
for the interior surfaces. (A real occlusion/visibility system — portals, or a
Minecraft-style per-sub-chunk flood-fill visibility graph — could additionally
*cull* them for cost, but that is a much larger, separate undertaking than base
chunks and is explicitly out of scope here.)

## Sequencing (effort ladder)

Roughly increasing effort; each is independently shippable:

1. **Frustum pre-cull** — small; a sphere-vs-frustum reject in
   `LightingSystem::Update` before upload.
2. **Depth pre-pass** — medium; high value, the main occlusion-cost lever.
3. **World chunks** — larger; broad-phase + streaming unit, lands with the
   asset-streaming work.
4. **Budgeted + cached shadow maps** — largest; the correctness piece.

The froxel per-cluster cap fix (built) is the foundation all of these sit on.

## Current state

- Clustered forward: `Render::ClusterGrid`, `Runtime::LightingSystem`,
  `cluster_build.comp` (per-froxel view-space AABB), `cluster_cull.comp`
  (sphere-vs-AABB assignment, **now uncapped per cluster**), `cube_min.frag`
  `ClusterIndex()` lookup. Grid 16×9×24, log Z-slices.
- All lights are uploaded every frame with **no frustum pre-cull** — every light
  is tested against every froxel regardless of visibility. Correct, but pays per
  light regardless of whether it's in view.
- **No depth pre-pass, no shadows.** Occluded surfaces can pay lighting cost; an
  unshadowed light leaks through geometry.
